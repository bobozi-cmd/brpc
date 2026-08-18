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
