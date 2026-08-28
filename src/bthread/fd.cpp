// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - An M:N threading library to make applications more concurrent.

// Date: Thu Aug  7 18:56:27 CST 2014

#include "butil/compat.h"
#include <new>                                   // std::nothrow
#include <sys/poll.h>                            // poll()
#if defined(OS_MACOSX)
#include <sys/types.h>                           // struct kevent
#include <sys/event.h>                           // kevent(), kqueue()
#endif
#include "butil/atomicops.h"
#include "butil/time.h"
#include "butil/fd_utility.h"                     // make_non_blocking
#include "butil/logging.h"
#include "butil/third_party/murmurhash3/murmurhash3.h"   // fmix32
#include "butil/memory/scope_guard.h"
#include "bthread/butex.h"                       // butex_*
#include "bthread/task_group.h"                  // TaskGroup
#include "bthread/bthread.h"                             // bthread_start_urgent

namespace butil {
extern int pthread_fd_wait(int fd, unsigned events, const timespec* abstime);
}

// Implement bthread functions on file descriptors

namespace bthread {

extern BAIDU_THREAD_LOCAL TaskGroup* tls_task_group;

/* 
 * 分块懒分配数组, 先只分配一级索引 _blocks, 某一段 fd 第一次使用时才分配对应的 Block
 */
template <typename T, size_t NBLOCK, size_t BLOCK_SIZE>
class LazyArray {
    struct Block {
        butil::atomic<T> items[BLOCK_SIZE];
    };

public:
    LazyArray() {
        memset(static_cast<void*>(_blocks), 0, sizeof(butil::atomic<Block*>) * NBLOCK);
    }

    butil::atomic<T>* get_or_new(size_t index) {
        const size_t block_index = index / BLOCK_SIZE;
        if (block_index >= NBLOCK) {
            return NULL;
        }
        const size_t block_offset = index - block_index * BLOCK_SIZE;
        Block* b = _blocks[block_index].load(butil::memory_order_consume);
        if (b != NULL) {
            return b->items + block_offset;
        }
        // 分配一个新 Block
        b = new (std::nothrow) Block;
        if (NULL == b) {
            b = _blocks[block_index].load(butil::memory_order_consume);
            return (b ? b->items + block_offset : NULL);
        }
        // Set items to default value of T.
        std::fill(b->items, b->items + BLOCK_SIZE, T());
        Block* expected = NULL;
        if (_blocks[block_index].compare_exchange_strong(
                expected, b, butil::memory_order_release,
                butil::memory_order_consume)) {
            return b->items + block_offset;
        }
        // CAS 失败者删除自己创建的 Block, 使用胜出者的 Block
        delete b;
        return expected->items + block_offset;
    }

    // 只查询, 不分配, 主要供 epoll 事件处理和 bthread_close() 使用
    butil::atomic<T>* get(size_t index) const {
        const size_t block_index = index / BLOCK_SIZE;
        if (__builtin_expect(block_index < NBLOCK, 1)) {
            const size_t block_offset = index - block_index * BLOCK_SIZE;
            Block* const b = _blocks[block_index].load(butil::memory_order_consume);
            if (__builtin_expect(b != NULL, 1)) {
                return b->items + block_offset;
            }
        }
        return NULL;
    }

private:
    butil::atomic<Block*> _blocks[NBLOCK];
};
// 每个 fd 对应一个butex, 这是一个版本计数器, 和 ParkingLot 的版本快照思想相同
typedef butil::atomic<int> EpollButex;

static EpollButex* const CLOSING_GUARD = (EpollButex*)(intptr_t)-1L;

#ifndef NDEBUG
butil::static_atomic<int> break_nums = BUTIL_STATIC_ATOMIC_INIT(0);
#endif

// Able to address 67108864 file descriptors, should be enough.
LazyArray<EpollButex*, 262144/*NBLOCK*/, 256/*BLOCK_SIZE*/> fd_butexes;

static const int BTHREAD_DEFAULT_EPOLL_SIZE = 65536;

/*
 * 通过 `bthread_start_background()` 创建的一个 bthread, 阻塞在 epoll_wait(), 所以会长期占用一个worker pthread
 */
class EpollThread {
public:
    EpollThread()
        : _epfd(-1)
        , _stop(false)
        , _tid(0) {
    }

    int start(int epoll_size) {
        if (started()) {
            return -1;
        }
        _start_mutex.lock();
        // Double check
        if (started()) {
            _start_mutex.unlock();
            return -1;
        }
        // 创建 epoll fd
#if defined(OS_LINUX)
        _epfd = epoll_create(epoll_size);
#elif defined(OS_MACOSX)
        _epfd = kqueue();
#endif
        _start_mutex.unlock();
        if (_epfd < 0) {
            PLOG(FATAL) << "Fail to epoll_create/kqueue";
            return -1;
        }
        bthread_attr_t attr = BTHREAD_ATTR_NORMAL;
        bthread_attr_set_name(&attr, "EpollThread::run_this");
        // 创建后台 bthread
        if (bthread_start_background(
                &_tid, &attr, EpollThread::run_this, this) != 0) {
            close(_epfd);
            _epfd = -1;
            LOG(FATAL) << "Fail to create epoll bthread";
            return -1;
        }
        return 0;
    }

    // Note: This function does not wake up suspended fd_wait. This is fine
    // since stop_and_join is only called on program's termination
    // (g_task_control.stop()), suspended bthreads do not block quit of
    // worker pthreads and completion of g_task_control.stop().
    // 另一个线程关闭 epoll fd 不一定能唤醒正在执行的 epoll_wait
    int stop_and_join() {
        if (!started()) {
            return 0;
        }
        // No matter what this function returns, _epfd will be set to -1
        // (making started() false) to avoid latter stop_and_join() to
        // enter again.
        const int saved_epfd = _epfd;
        _epfd = -1;

        // epoll_wait cannot be woken up by closing _epfd. We wake up
        // epoll_wait by inserting a fd continuously triggering EPOLLOUT.
        // Visibility of _stop: constant EPOLLOUT forces epoll_wait to see
        // _stop (to be true) finally.
        _stop = true;
        // 创建一条 pipe, 把写端注册为 EPOLLOUT, 立即触发事件, 使 epoll_wait 看到 _stop = true
        int closing_epoll_pipe[2];
        if (pipe(closing_epoll_pipe)) {
            PLOG(FATAL) << "Fail to create closing_epoll_pipe";
            return -1;
        }
#if defined(OS_LINUX)
        epoll_event evt = { EPOLLOUT, { NULL } };
        if (epoll_ctl(saved_epfd, EPOLL_CTL_ADD,
                      closing_epoll_pipe[1], &evt) < 0) {
#elif defined(OS_MACOSX)
        struct kevent kqueue_event;
        EV_SET(&kqueue_event, closing_epoll_pipe[1], EVFILT_WRITE, EV_ADD | EV_ENABLE,
                0, 0, NULL);
        if (kevent(saved_epfd, &kqueue_event, 1, NULL, 0, NULL) < 0) {
#endif
            PLOG(FATAL) << "Fail to add closing_epoll_pipe into epfd="
                        << saved_epfd;
            return -1;
        }
        // 等待 bthread 退出, 关闭 pipe 两端 和 epfd
        const int rc = bthread_join(_tid, NULL);
        if (rc) {
            LOG(FATAL) << "Fail to join EpollThread, " << berror(rc);
            return -1;
        }
        close(closing_epoll_pipe[0]);
        close(closing_epoll_pipe[1]);
        close(saved_epfd);
        return 0;
    }

    int fd_wait(int fd, unsigned events, const timespec* abstime) {
        // 找到 fd 对应的 slot
        butil::atomic<EpollButex*>* p = fd_butexes.get_or_new(fd);
        if (NULL == p) {
            errno = ENOMEM;
            return -1;
        }

        // lazy 创建 butex
        EpollButex* butex = p->load(butil::memory_order_consume);
        if (NULL == butex) {
            // It is rare to wait on one file descriptor from multiple threads
            // simultaneously. Creating singleton by optimistic locking here
            // saves mutexes for each butex.
            butex = butex_create_checked<EpollButex>();
            butex->store(0, butil::memory_order_relaxed);
            EpollButex* expected = NULL;
            // CAS 防止多线程同时创建 butex
            if (!p->compare_exchange_strong(expected, butex,
                                            butil::memory_order_release,
                                            butil::memory_order_consume)) {
                butex_destroy(butex);
                butex = expected;
            }
        }
        // 处理正在关闭的 fd, CLOSING_GUARD 表示这个 fd 正在执行 bthread_close
        while (butex == CLOSING_GUARD) {  // bthread_close() is running.
            // 让出 CPU, 避免一边注册 fd, 另一边在删除关闭它
            if (sched_yield() < 0) {
                return -1;
            }
            butex = p->load(butil::memory_order_consume);
        }
        // Save value of butex before adding to epoll because the butex may
        // be changed before butex_wait. No memory fence because EPOLL_CTL_MOD
        // and EPOLL_CTL_ADD shall have release fence.
        const int expected_val = butex->load(butil::memory_order_relaxed); // 在注册fd之前保存version

        // 注册 event fd
#if defined(OS_LINUX)
# ifdef BAIDU_KERNEL_FIXED_EPOLLONESHOT_BUG
        epoll_event evt = { events | EPOLLONESHOT, { butex } };
        if (epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &evt) < 0) {
            if (epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &evt) < 0 &&
                    errno != EEXIST) {
                PLOG(FATAL) << "Fail to add fd=" << fd << " into epfd=" << _epfd;
                return -1;
            }
        }
# else
        epoll_event evt;
        evt.events = events;
        evt.data.fd = fd;
        if (epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &evt) < 0 &&
            errno != EEXIST) {
            PLOG(FATAL) << "Fail to add fd=" << fd << " into epfd=" << _epfd;
            return -1;
        }
# endif
#elif defined(OS_MACOSX)
        struct kevent kqueue_event;
        EV_SET(&kqueue_event, fd, events, EV_ADD | EV_ENABLE | EV_ONESHOT,
                0, 0, butex);
        if (kevent(_epfd, &kqueue_event, 1, NULL, 0, NULL) < 0) {
            PLOG(FATAL) << "Fail to add fd=" << fd << " into kqueuefd=" << _epfd;
            return -1;
        }
#endif
        // 等待 butex 版本变化
        while (butex->load(butil::memory_order_relaxed) == expected_val) {
            // 如果 *butex == expected_val, 才原子挂起当前 bthread
            if (butex_wait(butex, expected_val, abstime) < 0 &&
                errno != EWOULDBLOCK && errno != EINTR) {
                return -1;
            }
        }
        return 0;
    }

    // 只能唤醒等待在 butex 上的 bthread, 不能唤醒走 poll() 的 pthread
    int fd_close(int fd) {
        if (fd < 0) {
            // what close(-1) returns
            errno = EBADF;
            return -1;
        }
        butil::atomic<EpollButex*>* pbutex = bthread::fd_butexes.get(fd);
        if (NULL == pbutex) {
            // 没有使用过 bthread fd的接口, 直接调用系统 close()
            return close(fd);
        }
        // 设置关闭哨兵
        EpollButex* butex = pbutex->exchange(
            CLOSING_GUARD, butil::memory_order_relaxed);
        if (butex == CLOSING_GUARD) {
            // concurrent double close detected.
            errno = EBADF;
            return -1;
        }
        // 唤醒已有等待者, 此时等待者的 bthread_fd_wait() 可能返回 0
        // 它只知道发生版本变化, 但是不清楚原因, 后续真正执行 read/write 才会得到相应错误
        if (butex != NULL) {
            butex->fetch_add(1, butil::memory_order_relaxed);
            butex_wake_all(butex);
        }
        // 从 epoll 中删除并关闭
#if defined(OS_LINUX)
        epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, NULL);
#elif defined(OS_MACOSX)
        struct kevent evt;
        EV_SET(&evt, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(_epfd, &evt, 1, NULL, 0, NULL);
        EV_SET(&evt, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(_epfd, &evt, 1, NULL, 0, NULL);
#endif
        const int rc = close(fd);
        /*
         * 恢复旧 butex 而不是销毁它:
         *  1. 避免已有等待线程引用已释放内存
         *  2. fd数字被os复用之后, 可以继续复用这个版本计数器
         */
        pbutex->exchange(butex, butil::memory_order_relaxed);
        return rc;
    }

    bool started() const {
        return _epfd >= 0;
    }

private:
    static void* run_this(void* arg) {
        return static_cast<EpollThread*>(arg)->run();
    }

    void* run() {
        const int initial_epfd = _epfd;
        const size_t MAX_EVENTS = 32; // 每次最多取32个事件
#if defined(OS_LINUX)
        epoll_event* e = new (std::nothrow) epoll_event[MAX_EVENTS];
#elif defined(OS_MACOSX)
        typedef struct kevent KEVENT;
        struct kevent* e = new (std::nothrow) KEVENT[MAX_EVENTS];
#endif
        if (NULL == e) {
            LOG(FATAL) << "Fail to new epoll_event";
            return NULL;
        }

#if defined(OS_LINUX)
# ifndef BAIDU_KERNEL_FIXED_EPOLLONESHOT_BUG
        DLOG(INFO) << "Use DEL+ADD instead of EPOLLONESHOT+MOD due to kernel bug. Performance will be much lower.";
# endif
#endif
        while (!_stop) {
            const int epfd = _epfd;
#if defined(OS_LINUX)
            const int n = epoll_wait(epfd, e, MAX_EVENTS, -1);
#elif defined(OS_MACOSX)
            const int n = kevent(epfd, NULL, 0, e, MAX_EVENTS, NULL);
#endif
            if (_stop) {
                break;
            }

            if (n < 0) {
                if (errno == EINTR) {
#ifndef NDEBUG
                    break_nums.fetch_add(1, butil::memory_order_relaxed);
                    int* p = &errno;
                    const char* b = berror();
                    const char* b2 = berror(errno);
                    DLOG(FATAL) << "Fail to epoll epfd=" << epfd << ", "
                                << errno << " " << p << " " <<  b << " " <<  b2;
#endif
                    continue;
                }

                PLOG(INFO) << "Fail to epoll epfd=" << epfd;
                break;
            }

#if defined(OS_LINUX)
# ifndef BAIDU_KERNEL_FIXED_EPOLLONESHOT_BUG
            for (int i = 0; i < n; ++i) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, e[i].data.fd, NULL);
            }
# endif
#endif
            for (int i = 0; i < n; ++i) {
#if defined(OS_LINUX)
# ifdef BAIDU_KERNEL_FIXED_EPOLLONESHOT_BUG
                EpollButex* butex = static_cast<EpollButex*>(e[i].data.ptr);
# else
                butil::atomic<EpollButex*>* pbutex = fd_butexes.get(e[i].data.fd);
                EpollButex* butex = pbutex ?
                    pbutex->load(butil::memory_order_consume) : NULL;
# endif
#elif defined(OS_MACOSX)
                EpollButex* butex = static_cast<EpollButex*>(e[i].udata);
#endif
                // 收到事件后唤醒所有等待者, 会发生 惊群, 但是只有一个线程会抢到 I/O 机会
                // 所以 bthread_fd_wait() 和非阻塞 I/O 应该放到 loop 里多次尝试
                if (butex != NULL && butex != CLOSING_GUARD) {
                    butex->fetch_add(1, butil::memory_order_relaxed);
                    butex_wake_all(butex);
                }
            }
        }

        delete [] e;
        DLOG(INFO) << "EpollThread=" << _tid << "(epfd="
                   << initial_epfd << ") is about to stop";
        return NULL;
    }

    int _epfd; // Linux 上 epoll fd; MacOS 上 kqueue fd
    bool _stop; // 通知事件循环退出
    bthread_t _tid; // 专用 epoll bthread 的 id
    butil::Mutex _start_mutex; // 防止并非重复启动
};

EpollThread epoll_thread[BTHREAD_EPOLL_THREAD_NUM];

static inline EpollThread& get_epoll_thread(int fd) {
    if (BTHREAD_EPOLL_THREAD_NUM == 1UL) {
        // 所有 fd 使用同一个 EpollThread
        EpollThread& et = epoll_thread[0];
        et.start(BTHREAD_DEFAULT_EPOLL_SIZE);
        return et;
    }
    // 多实例时, 通过 fd 哈希分配, 避免连续 fd 全部集中到某种简单取模模式
    EpollThread& et = epoll_thread[butil::fmix32(fd) % BTHREAD_EPOLL_THREAD_NUM];
    et.start(BTHREAD_DEFAULT_EPOLL_SIZE);
    return et;
}

//TODO(zhujiashun): change name
int stop_and_join_epoll_threads() {
    // Returns -1 if any epoll thread failed to stop.
    int rc = 0;
    for (size_t i = 0; i < BTHREAD_EPOLL_THREAD_NUM; ++i) {
        if (epoll_thread[i].stop_and_join() < 0) {
            rc = -1;
        }
    }
    return rc;
}

// For pthreads.
int pthread_fd_wait(int fd, unsigned events,
                    const timespec* abstime) {
    return butil::pthread_fd_wait(fd, events, abstime);
}

}  // namespace bthread

extern "C" {

int bthread_fd_wait(int fd, unsigned events) {
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    bthread::TaskGroup* g = bthread::tls_task_group;
    if (NULL != g && !g->is_current_pthread_task()) {
        // bthread 路径, 不占用当前 worker
        return bthread::get_epoll_thread(fd).fd_wait(
            fd, events, NULL);
    }
    // pthread 路径, 阻塞该 pthread
    return bthread::pthread_fd_wait(fd, events, NULL);
}

int bthread_fd_timedwait(int fd, unsigned events,
                         const timespec* abstime) {
    if (NULL == abstime) {
        return bthread_fd_wait(fd, events);
    }
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    bthread::TaskGroup* g = bthread::tls_task_group;
    if (NULL != g && !g->is_current_pthread_task()) {
        return bthread::get_epoll_thread(fd).fd_wait(
            fd, events, abstime);
    }
    return bthread::pthread_fd_wait(fd, events, abstime);
}

int bthread_connect(int sockfd, const sockaddr* serv_addr,
                    socklen_t addrlen) {
    bthread::TaskGroup* g = bthread::tls_task_group;
    if (NULL == g || g->is_current_pthread_task()) {
        // blocking socket 可能阻塞当前 pthread
        return ::connect(sockfd, serv_addr, addrlen);
    }
    // 普通 bthread
    bool is_blocking = butil::is_blocking(sockfd);
    if (is_blocking) {
        // 为了避免阻塞 worker pthread, 先暂时把 socket 改成 non-blocking
        butil::make_non_blocking(sockfd);
    }
    // 用 guard 保证返回时恢复原状
    auto guard = butil::MakeScopeGuard([is_blocking, sockfd]() {
        if (is_blocking) {
            butil::make_blocking(sockfd);
        }
    });
    // 执行非阻塞连接, EINPROGRESS 表示连接正在异步进行, 0 表示立即连接成功
    const int rc = ::connect(sockfd, serv_addr, addrlen);
    if (rc == 0 || errno != EINPROGRESS) {
        return rc;
    }
    // 对于 EINPROGRESS, 等待 socket 可写, 此时可能连接成, 也可能异步连接失败
#if defined(OS_LINUX)
    if (bthread_fd_wait(sockfd, EPOLLOUT) < 0) {
#elif defined(OS_MACOSX)
    if (bthread_fd_wait(sockfd, EVFILT_WRITE) < 0) {
#endif
        return -1;
    }
    // 检查socket错误和TCP状态
    if (butil::is_connected(sockfd) != 0) {
        return -1;
    }

    return 0;
}

int bthread_timed_connect(int sockfd, const struct sockaddr* serv_addr,
                          socklen_t addrlen, const timespec* abstime) {
    if (!abstime) {
        return bthread_connect(sockfd, serv_addr, addrlen);
    }

    bool is_blocking = butil::is_blocking(sockfd);
    if (is_blocking) {
        butil::make_non_blocking(sockfd);
    }
    // Scoped non-blocking.
    auto guard = butil::MakeScopeGuard([is_blocking, sockfd]() {
        if (is_blocking) {
            butil::make_blocking(sockfd);
        }
    });

    const int rc = ::connect(sockfd, serv_addr, addrlen);
    if (rc == 0 || errno != EINPROGRESS) {
        return rc;
    }
#if defined(OS_LINUX)
    if (bthread_fd_timedwait(sockfd, EPOLLOUT, abstime) < 0) {
#elif defined(OS_MACOSX)
    if (bthread_fd_timedwait(sockfd, EVFILT_WRITE, abstime) < 0) {
#endif
        return -1;
    }

    if (butil::is_connected(sockfd) != 0) {
        return -1;
    }

    return 0;
}

// This does not wake pthreads calling bthread_fd_*wait.
int bthread_close(int fd) {
    return bthread::get_epoll_thread(fd).fd_close(fd);
}

}  // extern "C"
