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

// Date: Tue Jul 10 17:40:58 CST 2012

#ifndef BTHREAD_TASK_GROUP_H
#define BTHREAD_TASK_GROUP_H

#include "butil/time.h"                             // cpuwide_time_ns
#include "bthread/task_control.h"
#include "bthread/task_meta.h"                     // bthread_t, TaskMeta
#include "bthread/work_stealing_queue.h"           // WorkStealingQueue
#include "bthread/remote_task_queue.h"             // RemoteTaskQueue
#include "butil/resource_pool.h"                    // ResourceId
#include "bthread/parking_lot.h"
#include "bthread/prime_offset.h"

namespace bthread {

// For exiting a bthread.
class ExitException : public std::exception {
public:
    explicit ExitException(void* value) : _value(value) {}
    ~ExitException() throw() {}
    const char* what() const throw() override {
        return "ExitException";
    }
    void* value() const {
        return _value;
    }
private:
    void* _value;
};

// Refer to https://rigtorp.se/isatomic/, On the modern CPU microarchitectures
// (Skylake and Zen 2) AVX/AVX2 128b/256b aligned loads and stores are atomic
// even though Intel and AMD officially doesn’t guarantee this.
// On X86, SSE instructions can ensure atomic loads and stores.
// Starting from Armv8.4-A, neon can ensure atomic loads and stores.
// Otherwise, use mutex to guarantee atomicity.
/*
 * - x86 使用对齐 SSE load/store;
 * - ARM NEON 平台使用向量 load/store;
 * - 其他平台用 mutex
 */
class AtomicInteger128 {
public:
    struct BAIDU_CACHELINE_ALIGNMENT Value {
        int64_t v1;
        int64_t v2;
    };

    AtomicInteger128() = default;
    explicit AtomicInteger128(Value value) : _value(value) {}

    Value load() const;
    Value load_unsafe() const {
        return _value;
    }

    void store(Value value);

private:
    Value _value{};
    // Used to protect `_cpu_time_stat' when __x86_64__, __ARM_NEON, and __riscv is not defined.
    FastPthreadMutex _mutex;
};

// 一个 TaskGroup 对应一个 worker pthread 上的 thread-local 调度器
// 保存当前 bthread & 就绪队列, 执行上下文切换, 和 global TaskControl 协作完成工作窃取 & worker 唤醒
// 一个 bthread 并不永久属于某个 TaskGroup, 可能被其他 worker 窃取, 在另一个 TaskGroup 上继续运行
class TaskGroup {
public:
    // Create task `fn(arg)' with attributes `attr' in TaskGroup *pg and put
    // the identifier into `tid'. Switch to the new task and schedule old task
    // to run.
    // Return 0 on success, errno otherwise.
    static int start_foreground(TaskGroup** pg,
                                bthread_t* __restrict tid,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg);

    // Create task `fn(arg)' with attributes `attr' in this TaskGroup, put the
    // identifier into `tid'. Schedule the new thread to run.
    //   Called from worker: start_background<false>
    //   Called from non-worker: start_background<true>
    // Return 0 on success, errno otherwise.
    template <bool REMOTE>
    int start_background(bthread_t* __restrict tid,
                         const bthread_attr_t* __restrict attr,
                         void * (*fn)(void*),
                         void* __restrict arg);

    // Suspend caller and run next bthread in TaskGroup *pg.
    static void sched(TaskGroup** pg);
    static void ending_sched(TaskGroup** pg);

    // Suspend caller and run bthread `next_tid' in TaskGroup *pg.
    // Purpose of this function is to avoid pushing `next_tid' to _rq and
    // then being popped by sched(pg), which is not necessary.
    
    /*
     * 为什么 sched_to 是 static 的, 并且接收 TaskGroup**?
     * 一个 bthread 挂起后, 可能被另一个 worker steal: `A 在 pg1 上挂起 -> 被 worker2 steal -> 在 pg2 上恢复`.
     * 所以 sched_to 返回时, 原来的 this 可能已经变成其他 worker 的TaskGroup, 所以不能依赖this, 通过返回前重新读取 TLS, 将pg更新成恢复后真正所在的 TaskGroup
    */
    static void sched_to(TaskGroup** pg, TaskMeta* next_meta);
    static void sched_to(TaskGroup** pg, bthread_t next_tid);
    static void exchange(TaskGroup** pg, TaskMeta* next_meta);

    // 必须等当前上下文已经停止运行之后，才能执行的收尾操作
    typedef void (*RemainedFn)(void*);
    void set_remained(RemainedFn cb, void* arg) {
        _last_context_remained = cb;
        _last_context_remained_arg = arg;
    }
    
    // Suspend caller for at least |timeout_us| microseconds.
    // If |timeout_us| is 0, this function does nothing.
    // If |group| is NULL or current thread is non-bthread, call usleep(3)
    // instead. This function does not create thread-local TaskGroup.
    // Returns: 0 on success, -1 otherwise and errno is set.
    static int usleep(TaskGroup** pg, uint64_t timeout_us);

    // Suspend caller and run another bthread. When the caller will resume
    // is undefined.
    static void yield(TaskGroup** pg);

    // Suspend caller until bthread `tid' terminates.
    static int join(bthread_t tid, void** return_value);

    // Returns true iff the bthread `tid' still exists. Notice that it is
    // just the result at this very moment which may change soon.
    // Don't use this function unless you have to. Never write code like this:
    //    if (exists(tid)) {
    //        Wait for events of the thread.   // Racy, may block indefinitely.
    //    }
    static bool exists(bthread_t tid);

    // Put attribute associated with `tid' into `*attr'.
    // Returns 0 on success, -1 otherwise and errno is set.
    static int get_attr(bthread_t tid, bthread_attr_t* attr);

    // Get/set TaskMeta.stop of the tid.
    static void set_stopped(bthread_t tid);
    static bool is_stopped(bthread_t tid);

    // The bthread running run_main_task();
    bthread_t main_tid() const { return _main_tid; }
    TaskStatistics main_stat() const;
    // Routine of the main task which should be called from a dedicated pthread.
    void run_main_task();

    // current_task is a function in macOS 10.0+
#ifdef current_task
#undef current_task
#endif
    // Meta/Identifier of current task in this group.
    TaskMeta* current_task() const { return _cur_meta; }
    bthread_t current_tid() const { return _cur_meta->tid; }
    // Uptime of current task in nanoseconds.
    int64_t current_uptime_ns() const
    { return butil::cpuwide_time_ns() - _cur_meta->cpuwide_start_ns; }

    // True iff current task is the one running run_main_task()
    bool is_current_main_task() const { return current_tid() == _main_tid; }
    // True iff current task is in pthread-mode.
    bool is_current_pthread_task() const
    { return _cur_meta->stack == _main_stack; }

    // Active time in nanoseconds spent by this TaskGroup.
    int64_t cumulated_cputime_ns() const;

    // 当前 TaskGroup 调用, 向 _rq 投递任务
    void ready_to_run(TaskMeta* meta, bool nosignal = false);
    // Flush tasks pushed to rq but signalled.
    void flush_nosignal_tasks();

    // 外部线程调用, 向 _remote_rq 投递任务
    void ready_to_run_remote(TaskMeta* meta, bool nosignal = false);
    void flush_nosignal_tasks_remote_locked(butil::Mutex& locked_mutex);
    void flush_nosignal_tasks_remote();

    // 自动根据 tls_task_group 是否是自己, 选择投递不同的任务队列
    void ready_to_run_general(TaskMeta* meta, bool nosignal = false);
    void flush_nosignal_tasks_general();

    // The TaskControl that this TaskGroup belongs to.
    TaskControl* control() const { return _control; }

    // Call this instead of delete.
    // 通过 timer 延迟删除, 因为 steal_task 在遍历其他 group 时, 没持有 _modify_group_mutex,
    // 因此另一个 worker 可能还持有刚被移出数组的 group 指针
    void destroy_self();

    // Wake up blocking ops in the thread.
    // Returns 0 on success, errno otherwise.
    static int interrupt(bthread_t tid, TaskControl* c, bthread_tag_t tag);

    // Get the meta associate with the task.
    static TaskMeta* address_meta(bthread_t tid);

    // Push a task into _rq, if _rq is full, retry after some time. This
    // process make go on indefinitely.
    void push_rq(bthread_t tid);

    // Returns size of local run queue.
    size_t rq_size() const {
        return _rq.volatile_size();
    }

    bthread_tag_t tag() const { return _tag; }

    pthread_t tid() const { return _tid; }

    int64_t current_task_cpu_clock_ns() {
        if (_last_cpu_clock_ns == 0) {
            return 0;
        }
        int64_t total_ns = _cur_meta->stat.cpu_usage_ns;
        total_ns += butil::cputhread_time_ns() - _last_cpu_clock_ns;
        return total_ns;
    }

private:
friend class TaskControl;

    // Last scheduling time, task type and cumulated CPU time.
    class CPUTimeStat {
        static constexpr int64_t LAST_SCHEDULING_TIME_MASK = 0x7FFFFFFFFFFFFFFFLL;
        static constexpr int64_t TASK_TYPE_MASK = 0x8000000000000000LL;
    public:
        CPUTimeStat() : _last_run_ns_and_type(0), _cumulated_cputime_ns(0) {}
        CPUTimeStat(AtomicInteger128::Value value)
            : _last_run_ns_and_type(value.v1), _cumulated_cputime_ns(value.v2) {}

        // Convert to AtomicInteger128::Value for atomic operations.
        explicit operator AtomicInteger128::Value() const {
            return {_last_run_ns_and_type, _cumulated_cputime_ns};
        }

        void set_last_run_ns(int64_t last_run_ns, bool main_task) {
            _last_run_ns_and_type = (last_run_ns & LAST_SCHEDULING_TIME_MASK) |
                                    (static_cast<int64_t>(main_task) << 63);
        }
        int64_t last_run_ns() const {
            return _last_run_ns_and_type & LAST_SCHEDULING_TIME_MASK;
        }
        int64_t last_run_ns_and_type() const {
            return _last_run_ns_and_type;
        }

        bool is_main_task() const {
            return _last_run_ns_and_type & TASK_TYPE_MASK;
        }

        void add_cumulated_cputime_ns(int64_t cputime_ns, bool main_task) {
            if (main_task) {
                return;
            }
            _cumulated_cputime_ns += cputime_ns;
        }
        int64_t cumulated_cputime_ns() const {
            return _cumulated_cputime_ns;
        }

    private:
        // The higher bit for task type, main task is 1, otherwise 0.
        // Lowest 63 bits for last scheduling time.
        int64_t _last_run_ns_and_type; // 最近一次调度时间 + 当前是否是 main_task
        // Cumulated CPU time in nanoseconds.
        int64_t _cumulated_cputime_ns; // 累计非 main_task 运行时间, 近似 worker 执行 bthread 的累计时间
    };

    class AtomicCPUTimeStat {
    public:
        CPUTimeStat load() const {
            return  _cpu_time_stat.load();
        }
        CPUTimeStat load_unsafe() const {
            return _cpu_time_stat.load_unsafe();
        }

        void store(CPUTimeStat cpu_time_stat) {
            _cpu_time_stat.store(AtomicInteger128::Value(cpu_time_stat));
        }

    private:
        AtomicInteger128 _cpu_time_stat; // 128-bit 原子快照保证两个 int64_t 同时读取
    };

    // You shall use TaskControl::create_group to create new instance.
    explicit TaskGroup(TaskControl* c);

    int init(size_t runqueue_capacity);

    // You shall call destroy_selfm() instead of destructor because deletion
    // of groups are postponed to avoid race.
    ~TaskGroup();

#ifdef BUTIL_USE_ASAN
    static void asan_task_runner(intptr_t);
#endif // BUTIL_USE_ASAN
    static void task_runner(intptr_t skip_remained);

    // Callbacks for set_remained()
    static void _release_last_context(void*);
    static void _add_sleep_event(void*);
    struct ReadyToRunArgs {
        bthread_tag_t tag;
        TaskMeta* meta;
        bool nosignal;
    };
    static void ready_to_run_in_worker(void*);
    static void ready_to_run_in_worker_ignoresignal(void*);
    static void priority_to_run(void*);

    // Wait for a task to run.
    // Returns true on success, false is treated as permanent error and the
    // loop calling this function should end.
    bool wait_task(bthread_t* tid);

    bool steal_task(bthread_t* tid) {
        if (_remote_rq.pop(tid)) {
            return true;
        }
#ifndef BTHREAD_DONT_SAVE_PARKING_STATE
        _last_pl_state = _pl->get_state();
#endif
        return _control->steal_task(tid, &_steal_seed, _steal_offset);
    }

    void set_tag(bthread_tag_t tag) { _tag = tag; }

    void set_pl(ParkingLot* pl) { _pl = pl; }

    /*
     * main task 是 worker pthread 原生栈的抽象, 其职责为:
     * - idle时在 ParkingLot 休眠
     * - 被唤醒后寻找任务
     * - 切换到普通 bthread
     * - 普通 bthread 全部跑完后, 重新回来等待
    */
    static bool is_main_task(TaskGroup* g, bthread_t tid) {
        return g->_main_tid == tid;
    }

    TaskMeta* _cur_meta{NULL}; // 当前正在此 worker 上运行的 bthread 的信息, 上下文切换时, `sched_to()` 会修改它
    
    // the control that this group belongs to
    TaskControl* _control{NULL}; // 所属的 global TaskControl
    int _num_nosignal{0}; // _rq 累积未通知 worker 的任务数
    int _nsignaled{0};
    AtomicCPUTimeStat _cpu_time_stat; // 当前运行状态和累积时间统计
    // last thread cpu clock
    int64_t _last_cpu_clock_ns{0};

    size_t _nswitch{0};
    /*
     * bthread 切换完成后需要执行的延迟回调, 有些操作不能在切换前执行, 因为旧的bthread仍然使用自己的栈和上下文, 比如:
     * - 一个bthread结束需要释放自己的栈, 需要切换到下一个栈, 在下一个任务中释放旧栈
     * - 把刚刚挂起的旧任务重新放回 rq
     * - 安装 sleep timer, 处理 priority queue ...
    */
    RemainedFn _last_context_remained{NULL};
    void* _last_context_remained_arg{NULL};

    ParkingLot* _pl{NULL}; // idle时用来休眠的 ParkingLot
#ifndef BTHREAD_DONT_SAVE_PARKING_STATE
    ParkingLot::State _last_pl_state;
#endif
    size_t _steal_seed{butil::fast_rand()};
    size_t _steal_offset{prime_offset(_steal_seed)};
    /*
     * main task           : current_tid == _main_tid && stack == _main_stack;
     * pthread-mode bthread: current_tid != _main_tid && stack == _main_stack;
     * stackful bthread.   : current_tid != _main_tid && stack != _main_stack;
    */
    ContextualStack* _main_stack{NULL};
    // `TaskGroup::init()` 时分配, 当 TaskGroup 没有普通 bthread 可以运行时, 会切回 _main_tid
    bthread_t _main_tid{INVALID_BTHREAD}; // = make_tid(), 本 worker 原生栈在 bthread 调度体系里的任务身份
    /*
     * 本 worker 的本地 work-steadling 就绪队列:
     * - 当前worker用 push/pop 操作自己的队列
     * - 其他worker用 steal 从另一端窃取任务
     * - 本地路径尽量避免加锁
    */
    WorkStealingQueue<bthread_t> _rq;
    RemoteTaskQueue _remote_rq; // 其他线程向本 group 投递任务的线程安全队列
    int _remote_num_nosignal{0}; // _remote_rq 累积未通知 worker 的任务数
    int _remote_nsignaled{0};

    int _sched_recursive_guard{0};
    // tag of this taskgroup
    bthread_tag_t _tag{BTHREAD_TAG_DEFAULT};

    // Worker thread id.
    pthread_t _tid{}; // = pthread_self(), 用于trace, 不参与 bthread 就绪队列和用户态调度
};

}  // namespace bthread

#include "task_group_inl.h"

#endif  // BTHREAD_TASK_GROUP_H
