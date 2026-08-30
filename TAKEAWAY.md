# bthread
- bthread 是一个可调度执行任务的抽象:
    - TaskMeta 保存任务的元数据和执行上下文入口
    - bthread_t 通过 version + slot 标识任务和复用资源
    - TaskGroup 负责调度任务运行

# TaskGroup
- TaskGroup 1:1 绑定 worker; 但是 bthread 不绑定 TaskGroup:
    - `TaskControl::steal_task(bthread_t* tid, ...)`: 任务窃取导致 tid 在不同 TaskGroup 之间流转
    - `static void sched_to(TaskGroup** pg, ...)`: sched_to 不能绑定 this TaskGroup, 需要通过重新读取 tls_task_group 更新 pg

# ResourcePool
- 什么时候真正析构和释放 pool 中的对象:
    - `return_resource()`: 只归还 id, 不会析构和释放
    - `~LocalPool()/clear_resources<T>()`: 线程退出, 只会删除 LocalPool, 把空闲 id 归还到全局, Block 和 对象依然存在
    - 在没有定义macro `BAIDU_CLEAR_RESOURCE_POOL_AFTER_ALL_THREADS_QUIT` 的情况下(在unittest里定义), 即使 `_nlocal` 归0, 也不会删除全局 Block, 因为:
        > 即使所有使用过 ResourcePool 的线程退出，仍可能有其他线程持有对象地址，贸然释放会造成悬空指针
    - ResourcePool 是一个带空闲列表的进程级 arena:
        - 内存占用随历史并发峰值增长
        - 峰值下降后，槽位会复用
        - RSS 通常不会因归还对象而明显下降
        - 最终由操作系统在进程退出时回收
- 并发控制:
    - lock-free:
        - 从本线程 _cur_free 取 ID 和 归还 ID
        - 从本线程 _cur_block 构造下一个对象
        - 根据 ID 查询地址
    - locked/atomic:
        - ResourcePool 单例初始化: _singleton_mutex
        - BlockGroup 创建: _block_group_mutex
        - 全局空闲 Chunk push/pop: _free_chunks_mutex
        - LocalPool 创建和测试清理协调: _change_thread_mutex
        - Block、BlockGroup 发布: atomic release/consume
    - 不保证 T 自身线程安全, 把对象指针或 ID 交给其他线程时, 调用方仍要建立正确的同步关系

# ExecutionQueue<T>
- 一个面向 actor/message-passing 场景的 MPSC 队列, 多个生产者并发提交任务, 同一个队列只有一个消费者串行执行, 消费者按需启动, 默认在队列清空之后退出, 并且一次回调可以批量处理多个任务
- 队列没有容量限制和背压, 生产速度长期高于消费速度时, 内存会持续增长
- 为什么原子链表最后还能保持FIFO:
    - 生产者通过 `_head.exchange()` 压栈, 因此刚写入时时 LIFO: `_head -> C -> B -> A`
    - 消费者已经在处理A, 当他处理到链表尾部时, 调用 `_more_tasks()`, 会把新增部分反转并接到 A 后: `A -> B -> C`, 所以消费顺序仍然是 FIFO

# fd
- `bthread_fd_wait()` 只等待"可能就绪", 不执行真正的 I/O, 也不保证返回后一定能读写, 应搭配非阻塞 fd 和 EAGAIN 重试循环
- 使用过 bthread fd 等待后, 应调用 bthread_close(), 否则等待者可能永久挂起
- 同一个 fd 的事件会唤醒所有等待者, 可能产生惊群
- 同一个 fd 同时用不同的事件集合等待并不理想, 默认 Linux 路径第二次 EPOLL_CTL_ADD 得到 EEXIST 后不会更新最初注册的事件集合
- 由于兼容旧内核 epoll one-shot 问题, 默认路径需要频繁执行 EPOLL_CTL_ADD/DEL, 不适合高频、性能关键的事件分发. brpc 自己的高性能 socket 处理通常使用专门的 EventDispatcher
- 什么是 epoll one-shot?
    - 普通 epoll 默认采用 "持续监听" 语义, fd 注册之后, 只要它满足就绪条件, epoll_wait() 就可能反复返回该 fd, 而如果注册时加入 `EPOLLONESHOT`:
        ```cpp
        evt.events = EPOLLIN | EPOLLONESHOT;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &evt);
        ```
    - 那么 epoll 只会通知一次, 然后该 fd 在 epoll 中自动被禁用, 如果想要继续监听, 必须显式重新激活: `epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &evt);`
    - one-shot 主要用来防止同一个 fd 在事件尚未处理完时, 被重复分发, 比如有多个 epoll 处理线程重复收到事件, 多个线程同时读取同一个 socket, 并发修改状态等, 使用 EPOLLONESHOT 后, 第一次事件返回便自动禁用 fd. 处理者完成后再 MOD 重新启用, 相当于暂时取得这个 fd 的事件处理权
    - EPOLLONESHOT 表示 epoll 只上报一次，不表示只唤醒一个业务 bthread, 一次 epoll 通知仍然可以通过 butex_wake_all() 唤醒很多 bthread
    - 和边缘触发 EPOLLET 的区别: EPOLLET 决定"什么时候通知", EPOLLONESHOT 决定"通知一次后是否自动禁用"; 即 EPOLLET 不会禁用 fd, 只是每次触发事件只通知一次

# Controller
- 一个 RPC 和一次 Call 的区别:
    - 一个用户看到的逻辑 RPC, 可能包含多个网络请求: 
        - 第一次 Call + (重试 Call + (Backup Call))
        - 内部的 `struct Controller::Call` 表示其中一次实际发送
- 所有成功、失败、超时、取消、重试和 backup request 最终都汇入 OnVersionedRPCReturned(), 再由 EndRPC() 统一完成资源回收和用户回调
- 为什么 `IssueRPC()` 需要记录 Call_id?
    - 因为网络响应可能是乱序的, 例如:
        1. 第一次请求超时;
        2. 框架发出重试;
        3. 第一次请求的迟到响应随后到达
    - 如果只使用同一个 ID, 旧响应可能被误认为重试响应. 版本化 ID 让 OnVersionedRPCReturned() 能识别并忽略已经过期的结果
- 普通重试和 backup request 的区别:
    - 重试: 上一请求已经失败, 之后再发一个
    - Backup Request: 上一请求可能仍在处理, 只是响应太慢, 于是并发发第二个, 谁先成功就采用谁
    - 负载均衡时, 失败过的服务器会暂时放入 _accessed, 尽量避免重试时立即选回同一个节点

# IOBuf
- IOBuf 本质是一个 `BlockRef` 队列:
    - `SmallView`: 最多两个引用时, 直接在 IOBuf 对象里保存, 不需要堆分配
        ```cpp
        struct SmallView {
            BlockRef refs[2];
        };
        ```
    -  `BigView`: 第三个无法合并的引用加入时, 转为 BigView, refs 是容量从 32 开始、按两倍扩容的环形队列, 这样从头部弹出完整引用只需移动 start, 不用搬移数组. 降到两个引用时, 又会退化回 SmallView
        ```cpp
        struct BigView {
            int32_t magic;
            uint32_t start;
            BlockRef* refs; // refs[(start + i) & cap_mask]
            uint32_t nref;
            uint32_t cap_mask;
            size_t nbytes;
        };
        ```
    - 转换代码: `IOBuf::_push_or_move_back_ref_to_smallview(const BlockRef& r)` 和 `IOBuf::_pop_or_moveout_front_ref()`
    - SmallView 和 BigView 放在同一个 union 中, 并且大小相等. SmallView 第一个引用的 offset 与 BigView 的 magic 位于同一位置. BigView 把 magic 设置为 -1, SmallView 的合法 offset 最高位为 0, 这样省掉了额外的类型字段:
        ```cpp
        bool IOBuf::_small() const {
            return _bv.magic >= 0;
        }
        ```
- 引用合并: 追加一个 BlockRef 时，如果它与末尾引用指向同一个 Block & 新引用的 offset 正好等于旧引用的 offset + length, 那么两者直接合并. 这对 TLS 共享 Block 很重要, 否则每次小字符串追加都会产生一个新引用.