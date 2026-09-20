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

# flat_map
- 一个"桶数组 + 分离链表"的哈希表, 关键优化是: 每个桶的第一个元素直接存放在桶数组里, 只有哈希冲突后的元素才动态分配
- 为什么第一个元素要放在桶数组中?
    - 普通拉链法哈希表通常是: `bucket -> node -> node`, 查找第一个元素需要一次指针跳转和一次节点分配
    - 这里改成: `bucket+node -> node`, 当冲突较少时:
        - 查找通常只访问连续的 _buckets 数组
        - 首元素没有独立内存分配
        - 缓存局部性更好
        - 小map的桶数组(< 16个buckets)本身也位于 FlatMap 对象中
    - 代价是删除桶首元素时不能简单摘链, 需要把第二个节点搬到桶首
- 哈希和桶数量:
    - 默认情况下, 桶数被向上取整为 2 的幂: `hash_code & (nbucket - 1);` 这种比 `hash_code % nbucket;` 快, 但是要求哈希值的低位分布足够好, 如果低位质量差, 会产生严重冲突
    - 当桶数为 `2^k` 时, 位与运算只保留 `hash_code` 的低 `k` 位, 高位完全不参与桶下标的计算
        - "低位分布好" 指这些低位在不同 key 之间近似均匀且能充分变化. 例如桶数为 16 时, `hash(key) = key << 4` 的低 4 位永远是 0, 所有 key 都会落入 0 号桶, 即使它们的完整哈希值各不相同
        - 优质哈希函数应将输入变化充分扩散到输出的每一位. 自定义 hasher 时不要直接返回具有固定对齐或明显数值规律的结果, 可使用 MurmurHash 等具有良好雪崩效应的哈希算法
        - 质数桶数配合 `%` 会让更多比特影响桶下标, 对低位质量较差的哈希值更宽容, 但取模运算通常更慢
    - 扩容通常按照 `resize(_nbucket + 1)`, 由于会向上取2的幂, 所以效果相当于翻倍
- 容器内部使用 SingleThreadedPool，没有任何同步机制，本身不是线程安全的. save_iterator/restore_iterator 只是帮助调用者在外部加锁并分段遍历，不代表可以无锁并发访问.

# arena
- 为什么分配Block内存以 4/1 为界?
    - 此时进入普通块更换逻辑时满足: 当前剩余空间 < n <= _block_size / 4
    - 因为当前剩余空间小于 n, 所以放弃旧块时浪费的空间也小于等于旧块的四分之一, 控制普通块尾部碎片, 同时隔离偶发大请求
- 使用限制:
    - 不调用对象析构函数
    - allocate() 不保证每次分配都对齐
    - 不是线程安全的, _cur_block 和 alloc_size 都没有同步保护
    - 块大小使用 uint32_t
    - clear() 是释放而不是复用, 频繁 clear() 仍然会产生 malloc/free 开销

# bounded_queue
- 一个容量固定、底层使用环形数组的双端队列. 创建后不会自动扩容, 头尾插入、删除和按下标访问都是 O(1), 适合容量上限明确且对分配开销、延迟抖动敏感的场景
- 常见使用场景:
    - 滑动时间窗口: 保存最近 N 次监控采样、延迟或 QPS 数据, 使用 `elim_push()` 在队满后覆盖最老元素
    - 有界 FIFO 缓存: 使用 `push()` 从 bottom 加入、`pop()` 从 top 删除, 例如 HPACK 动态表淘汰最老 Header
    - 最近访问记录: 保存 RPC 重试中最近选择过的服务器, 容量满后遗忘最早记录
    - 固定容量任务缓冲区: `push()` 在队满时返回 false, 由上层决定拒绝、重试或降级
    - 小型双端队列: 通过 `push_top()`、`pop_bottom()` 支持两端操作, 并可用 `top(i)`、`bottom(i)` 随机访问
- 两种队满策略:
    - `push()`: 保留旧元素并返回 false, 适合任务不能被静默丢弃的场景
    - `elim_push()`: 覆盖 top 端最老元素并保留最新 N 条数据, 适合采样和历史记录
- 内存可以由队列通过 `malloc` 持有, 也可以由调用方提供, 因此适合栈上小队列、对象内嵌存储或一次性联合分配; 外部存储必须满足 `T` 的对齐要求, 且生命周期不能短于队列
- 它本身不是线程安全的. 多线程访问必须由调用方加锁, 或改用专门的 SPSC/MPMC 并发队列
- 不适合容量无法预估、需要自动扩容、需要阻塞等待或要求 lock-free 并发访问的场景

# 负载均衡和容错
## LoadBalancer
- 核心抽象API, 覆盖 "节点变化 -> 请求开始 -> 请求结束" 三阶段:
    - AddServer(const ServerId& server) + RemoveServer(const ServerId& server): 命名服务发现节点变化后, 会通知负载均衡器增加或删除节点. 职责分离: NamingService 负责感知有哪些节点, LoadBalancer 负责选择哪个节点.
    - SelectServer(const SelectIn& in, SelectOut* out): 每次真正发起请求前, Controller::IssueRPC 调用其选择服务
    - Feedback(const CallInfo& info): 完成请求后, 告知 LB 本次 Call 的开始时间、目标节点和错误码, 自适应算法可计算延迟并调整后续选择概率.
    - brpc把LB分成两类:
        - 无反馈算法: `rr`、`random`、`wrr`、`wr`、一致性哈希
        - 有反馈算法: `la` (locality-aware)

- Round Robin 为什么不用全局原子计数器做轮询?
    - 最简单的实现: `index = atomic_counter.fetch_add(1) % node_count;`, 但在高并发下，所有线程都会修改同一个原子变量，那个 cache line 会不断在 CPU 核之间同步
    - 所以 brpc 使用 TLS, 让每个线程独立维护自己的状态, 初始化时, 不同线程:
        - 从随机位置开始
        - 使用不同的大质数作为步长
        - 不争抢一个全局计数器
    - 因此它不保证严格的请求序列一定是 A -> B -> C -> D, 但在请求量足够大时, 各节点被选中的次数会很接近.

- Round Robin 为什么使用 `vector + map` 保存 Servers?
    - 两种容器服务于不同的访问模式:
        - `server_list: vector<ServerId>` 支持按 RR 的 `offset` 以 O(1) 取得候选节点, 连续存储也有利于遍历不可用节点时的 CPU cache 局部性
        - `server_map: map<ServerId, size_t>` 保存 ServerId 到 vector 下标的映射, 用于判重以及在 O(logN) 时间内定位待删除节点
    - 删除节点时不调用 `vector::erase`, 而是用末尾节点覆盖待删除位置, 更新该节点在 map 中的下标后再 `pop_back()`. 这样定位为 O(logN), vector 内的删除为 O(1), 代价是节点顺序可能改变; RR 不依赖稳定顺序, 因而可以接受
    - 只用 `map` 虽然足以实现轮询, 但 map 没有随机下标访问. 根据 `offset` 取得第 i 个元素需要从迭代器前进, 最坏为 O(N); 保存跨请求的迭代器也会与动态更新、双缓冲版本切换产生生命周期问题
    - 只用 `vector` 能高效选择节点, 但判重和定位指定待删除节点需要 O(N) 扫描
    - `unordered_map` 不能替代 vector: 它的平均 O(1) 是按 key 查找, 并不支持按 RR 下标取得第 i 个元素; 迭代顺序也不稳定, rehash 还会使迭代器失效
    - `unordered_map<ServerId, size_t>` 可以在技术上替代当前辅助索引 map, 将判重和定位降为平均 O(1). 但该索引只在命名服务更新节点时使用, 每次 RPC 的热点选择路径只访问 vector, 因此收益通常有限; `map` 则提供稳定的 O(logN) 最坏复杂度且没有 rehash 延迟峰值

- RandomizedLoadBalancer 和 RR 的区别:
    - rr 保存每线程的 offset, 下次调用接着往前走, 因此短时间内分布通常更均匀;
    - random 每次重新随机选起点, 因此连续几次选中 A 完全可能, 请求足够多时才趋向均匀

- ExcludedServers 为什么保存 SocketId, 而不是 IP 地址?
    - 一个地址对应的旧连接可能已经失败，随后又创建了新连接. SocketId 带有对象身份信息, 可以区分同一个IP的新旧 Socket.

## 策略对比

| 策略 | 选择方式 | 是否使用调用结果反馈 | 适合的场景 |
| --- | --- | --- | --- |
| `rr` | 每线程保存位置和步长, 近似均匀轮询 | 否 | 节点能力相近, 希望短期分布较均匀 |
| `random` | 每次随机选起点 | 否 | 节点能力相近, 可接受短期随机波动 |
| `wrr` | 按配置权重轮询, 每线程保存权重进度 | 否 | 节点能力不同, 权重相对稳定 |
| `wr` | 按配置权重随机抽取 | 否 | 节点能力不同, 可接受短期随机波动 |
| `la` | 根据反馈动态调整节点权重并抽取 | 是 | 延迟、负载或网络条件持续变化 |
| `c_murmurhash` / `c_md5` / `c_ketama` | 根据请求的 `request_code` 做一致性哈希, 当前要求 32 位 | 否 | 缓存、分片等需要稳定映射的场景 |

## `wrr` 与 `wr`: 配置权重

- 权重取自 `ServerId.tag`, 须能解析为正整数; 新节点加入后立即按该权重参与选址, 不自带按时间爬坡.
- `wrr` 把权重视为一圈份额, 用与总权重互质的步长推进; 线程局部的 `position`、`stride` 和 `remain_server` 保存进度. 它比独立随机抽样更平稳, 但不保证跨线程的全局严格顺序.
- `wrr` 遇到被排除或不可用的节点, 在本次选择中临时过滤该节点并按剩余权重重算步长; 只有找到可用节点, 才把临时进度写回线程局部状态. 全部不可用时返回 `EHOSTDOWN`.
- `wr` 保存累计权重, 用随机数和 `lower_bound` 定位节点. 随机抽样可能重复, 因此找不到可用节点时还有一轮查找未试过节点的兜底逻辑.

## `la`: 反馈闭环

- 新节点的初始权重取现有节点的平均权重, 并非从低流量开始预热.
- `SelectServer()` 选中节点时调用 `AddInflight()`: 先根据已有在途请求的等待时间重算权重, 必要时拒绝这次选择; 真正选中后才累计开始时间和在途数量, 并设置 `need_feedback`.
- 每次实际 Call 完成后, `Controller::Call::OnComplete()` 将开始时间、节点和错误码交给 `Feedback()`. 成功调用进入最多 128 条记录的统计队列, 基础权重近似为 `QPS * WEIGHT_SCALE / 平均延迟`; 失败调用按耗时、超时及重试阶段构造惩罚, 不被当作一次快速成功.
- 当前权重还会考虑在途请求: `在途平均等待时间 = 当前时间 - 开始时间之和 / 在途数量`. 当它超过历史平均延迟的阈值时, 即使请求尚未超时, 也会降低节点权重; 不会降到配置的最小权重以下.
- 按权重选址使用数组形式的二叉树: 每个节点保存自身权重和左子树权重之和, 沿树查找约为 `O(log N)`. `DoublyBufferedData` 保护较少变化的节点列表; 高频变化的权重由共享的 `Weight` 对象、节点锁及原子权重和维护. 读到短暂不一致的权重组合时可能重新选择, 不要求每次读取都是全局原子快照.

## 故障、恢复与冷启动

- 命名服务负责“节点是否属于集群”, `Socket::Address()` 与 `IsAvailable()` 负责“此刻是否可接请求”. `Socket` 失败后, 节点即使仍在候选列表中, 也会被选址跳过; 启用健康检查时, 连接恢复并 `Revive()` 后可重新参与选址.
- 默认的连接失败处理与可选的请求级熔断不同. 开启 `ChannelOptions.enable_circuit_breaker` 后, 每次 Call 的错误码和耗时进入长、短两套统计; 任一套判定不健康, 就隔离对应的主 `Socket`. 初始化阶段按完整窗口的错误次数预算判定, 而非用最初几个样本的瞬时错误率.
- 当前源码没有“为每个新节点设置冷启动时长并自动爬坡”的现成开关. 可由发布/服务发现系统分阶段提高 `wrr` 权重, 但修改 tag 会被识别为移除旧节点、加入新节点, 并非原地调权; 若需精确且平滑的冷启动时间, 应扩展自定义 `LoadBalancer`.
- `min_working_instances` / `hold_seconds` 是 `rr`、`random` 在**整个集群宕机后恢复**时的客户端限流
