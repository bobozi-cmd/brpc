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

// Date: Sun Jul 13 15:04:18 CST 2014

#ifndef BUTIL_RESOURCE_POOL_INL_H
#define BUTIL_RESOURCE_POOL_INL_H

#include <iostream>                      // std::ostream
#include <pthread.h>                     // pthread_mutex_t
#include <algorithm>                     // std::max, std::min
#include "butil/atomicops.h"             // butil::atomic
#include "butil/macros.h"                // BAIDU_CACHELINE_ALIGNMENT
#include "butil/scoped_lock.h"           // BAIDU_SCOPED_LOCK
#include "butil/thread_local.h"          // thread_atexit
#include "butil/memory/aligned_memory.h" // butil::AlignedMemory
#include <vector>

#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
#define BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_ADD1                \
    (_global_nfree.fetch_add(1, butil::memory_order_relaxed))
#define BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_SUB1                \
    (_global_nfree.fetch_sub(1, butil::memory_order_relaxed))
#else
#define BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_ADD1
#define BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_SUB1
#endif

namespace butil {

// uint64_t id = block_index * BLOCK_NITEM + offset_in_block;
// - block_index = id / BLOCK_NITEM;
// - offset_in_block = id % BLOCK_NITEM;
// - group_index = block_index >> 16;
// - block_index_in_group = block_index & 0xffff;
// 最终地址: `_block_groups[group_index]->blocks[block_index_in_group]->items[offset]`
template <typename T>
struct ResourceId {
    uint64_t value;

    operator uint64_t() const {
        return value;
    }

    template <typename T2>
    ResourceId<T2> cast() const {
        ResourceId<T2> id = { value };
        return id;
    }
};

template <typename T, size_t NITEM> 
struct ResourcePoolFreeChunk {
    size_t nfree;
    ResourceId<T> ids[NITEM];
};
// for gcc 3.4.5
template <typename T> 
struct ResourcePoolFreeChunk<T, 0> {
    size_t nfree;
    ResourceId<T> ids[0]; // 可变长尾部数组
};

struct ResourcePoolInfo {
    size_t local_pool_num; // 当前存在的线程 LocalPool 数量, _nlocal
    size_t block_group_num; // 已创建的 BlockGroup 数, _ngroup
    size_t block_num;
    size_t item_num; // 成功构造过的对象总数，包括空闲和使用中
    size_t block_item_num;
    size_t free_chunk_item_num;
    size_t total_size; // block_num * block_item_num * sizeof(T)
#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
    size_t free_item_num;
#endif
};

static const size_t RP_MAX_BLOCK_NGROUP = 65536;
static const size_t RP_GROUP_NBLOCK_NBIT = 16;
static const size_t RP_GROUP_NBLOCK = (1UL << RP_GROUP_NBLOCK_NBIT);
static const size_t RP_INITIAL_FREE_LIST_SIZE = 1024;

template <typename T>
class ResourcePoolBlockItemNum {
    static const size_t N1 = ResourcePoolBlockMaxSize<T>::value / sizeof(T);
    static const size_t N2 = (N1 < 1 ? 1 : N1);
public:
    static const size_t value = (N2 > ResourcePoolBlockMaxItem<T>::value ?
                                 ResourcePoolBlockMaxItem<T>::value : N2);
};


template <typename T>
class BAIDU_CACHELINE_ALIGNMENT ResourcePool {
public:
    static const size_t BLOCK_NITEM = ResourcePoolBlockItemNum<T>::value;
    static const size_t FREE_CHUNK_NITEM = BLOCK_NITEM;

    // thread-local 的固定容量数组
    typedef ResourcePoolFreeChunk<T, FREE_CHUNK_NITEM>      FreeChunk;
    typedef ResourcePoolFreeChunk<T, 0> DynamicFreeChunk;

    // 大小容纳一个T, 地址满足 alignof(T), 尚未构造成 T 的原始字节区域
    typedef AlignedMemory<sizeof(T), __alignof__(T)> BlockItem;
    // When a thread needs memory, it allocates a Block. To improve locality,
    // items in the Block are only used by the thread.
    // To support cache-aligned objects, align Block.items by cacheline.
    struct BAIDU_CACHELINE_ALIGNMENT Block {
        // BlockItem 在第一次使用 slot 时, 通过 placement new 构造
        BlockItem items[BLOCK_NITEM];
        size_t nitem; // 该 Block 中已经成功构造了多少个对象

        Block() : nitem(0) {}
    };

    // A Resource addresses at most RP_MAX_BLOCK_NGROUP BlockGroups,
    // each BlockGroup addresses at most RP_GROUP_NBLOCK blocks. So a
    // resource addresses at most RP_MAX_BLOCK_NGROUP * RP_GROUP_NBLOCK Blocks.
    struct BlockGroup {
        butil::atomic<size_t> nblock;
        butil::atomic<Block*> blocks[RP_GROUP_NBLOCK];

        BlockGroup() : nblock(0) {
            // We fetch_add nblock in add_block() before setting the entry,
            // thus address_resource() may sees the unset entry. Initialize
            // all entries to NULL makes such address_resource() return NULL.
            memset(static_cast<void*>(blocks), 0, sizeof(butil::atomic<Block*>) * RP_GROUP_NBLOCK);
        }
    };


    // 每个线程一个 LocalPool
    class BAIDU_CACHELINE_ALIGNMENT LocalPool {
    public:
        explicit LocalPool(ResourcePool* pool)
            : _pool(pool)
            , _cur_block(NULL)
            , _cur_block_index(0) {
            _cur_free.nfree = 0;
        }

        ~LocalPool() {
            // Add to global _free_chunks if there're some free resources
            if (_cur_free.nfree) {
                // 把local 空闲的 ID 推入 global _free_chunks
                _pool->push_free_chunk(_cur_free);
            }

            _pool->clear_from_destructor_of_local_pool();
        }

        static void delete_local_pool(void* arg) {
            delete(LocalPool*)arg;
        }

        // We need following macro to construct T with different CTOR_ARGS
        // which may include parenthesis because when T is POD, "new T()"
        // and "new T" are different: former one sets all fields to 0 which
        // we don't want.
#define BAIDU_RESOURCE_POOL_GET(CTOR_ARGS)                                  \
        /* Fetch local free id */                                           \
        /* 申请线程本地空闲 ID, 不加锁, 不申请内存, 不调构造函数, 类似本地 LiFO */   \
        if (_cur_free.nfree) {                                              \
            const ResourceId<T> free_id = _cur_free.ids[--_cur_free.nfree]; \
            *id = free_id;                                                  \
            BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_SUB1;                         \
            return unsafe_address_resource(free_id);                        \
        }                                                                   \
        /* Fetch a FreeChunk from global.                                   \
           TODO: Popping from _free needs to copy a FreeChunk which is      \
           costly, but hardly impacts amortized performance. */             \
        /* 从全局 _free_chunks 取出一个批次, 加锁, 一次获取一批 ID */             \
        if (_pool->pop_free_chunk(_cur_free)) {                             \
            --_cur_free.nfree;                                              \
            const ResourceId<T> free_id =  _cur_free.ids[_cur_free.nfree];  \
            *id = free_id;                                                  \
            BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_SUB1;                         \
            return unsafe_address_resource(free_id);                        \
        }                                                                   \
        /* 没有可以复用的ID, 检查当前线程的 _cur_block, 构造T */                 \
        T* p = NULL;                                                        \
        /* Fetch memory from local block */                                 \
        if (_cur_block && _cur_block->nitem < BLOCK_NITEM) {                \
            id->value = _cur_block_index * BLOCK_NITEM + _cur_block->nitem; \
            auto item = _cur_block->items + _cur_block->nitem;              \
            p = new (item->void_data()) T CTOR_ARGS;                        \
            if (!ResourcePoolValidator<T>::validate(p)) {                   \
                /* 只验证新构造的对象, 复用空闲对象时不再验证 */                   \
                p->~T();                                                    \
                return NULL;                                                \
            }                                                               \
            ++_cur_block->nitem;                                            \
            return p;                                                       \
        }                                                                   \
        /* Fetch a Block from global */                                     \
        _cur_block = add_block(&_cur_block_index);                          \
        if (_cur_block != NULL) {                                           \
            id->value = _cur_block_index * BLOCK_NITEM + _cur_block->nitem; \
            auto item = _cur_block->items + _cur_block->nitem;              \
            p = new (item->void_data()) T CTOR_ARGS;                        \
            if (!ResourcePoolValidator<T>::validate(p)) {                   \
                p->~T();                                                    \
                return NULL;                                                \
            }                                                               \
            ++_cur_block->nitem;                                            \
            return p;                                                       \
        }                                                                   \
        return NULL;                                                        \
 

        inline T* get(ResourceId<T>* id) {
            BAIDU_RESOURCE_POOL_GET();
        }

        template<typename... Args>
        inline T* get(ResourceId<T>* id, Args&&... args) {
            BAIDU_RESOURCE_POOL_GET((std::forward<Args>(args)...));
        }

#undef BAIDU_RESOURCE_POOL_GET

        // 不析构, 不clear, 只是把id放到local _cur_free, 本地chunk满了归还到 global _free_chunks
        inline int return_resource(ResourceId<T> id) {
            // Return to local free list
            if (_cur_free.nfree < ResourcePool::free_chunk_nitem()) {
                _cur_free.ids[_cur_free.nfree++] = id;
                BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_ADD1;
                return 0;
            }
            // Local free list is full, return it to global.
            // For copying issue, check comment in upper get()
            if (_pool->push_free_chunk(_cur_free)) {
                _cur_free.nfree = 1;
                _cur_free.ids[0] = id;
                BAIDU_RESOURCE_POOL_FREE_ITEM_NUM_ADD1;
                return 0;
            }
            return -1;
        }

    private:
        ResourcePool* _pool;
        Block* _cur_block;
        size_t _cur_block_index;
        FreeChunk _cur_free;
    };

    static inline T* unsafe_address_resource(ResourceId<T> id) {
        const size_t block_index = id.value / BLOCK_NITEM;
        return (T*)(_block_groups[(block_index >> RP_GROUP_NBLOCK_NBIT)]
                    .load(butil::memory_order_consume)
                    ->blocks[(block_index & (RP_GROUP_NBLOCK - 1))]
                    .load(butil::memory_order_consume)->items) +
               id.value - block_index * BLOCK_NITEM;
    }

    static inline T* address_resource(ResourceId<T> id) {
        const size_t block_index = id.value / BLOCK_NITEM;
        const size_t group_index = (block_index >> RP_GROUP_NBLOCK_NBIT);
        if (__builtin_expect(group_index < RP_MAX_BLOCK_NGROUP, 1)) {
            BlockGroup* bg =
                _block_groups[group_index].load(butil::memory_order_consume);
            if (__builtin_expect(bg != NULL, 1)) {
                Block* b = bg->blocks[block_index & (RP_GROUP_NBLOCK - 1)]
                           .load(butil::memory_order_consume);
                if (__builtin_expect(b != NULL, 1)) {
                    const size_t offset = id.value - block_index * BLOCK_NITEM;
                    if (__builtin_expect(offset < b->nitem, 1)) {
                        return (T*)b->items + offset;
                    }
                }
            }
        }

        return NULL;
    }

    // 构造参数只对 新slot 有效, 复用slot时, 复用的是旧 slot 的参数, 所以 The object should be cleared before usage.
    // brpc 通过 id + version 做标识, 复用时通过 OnCreated() 初始化状态
    template<typename... Args>
    inline T* get_resource(ResourceId<T>* id, Args&&... args) {
        LocalPool* lp = get_or_new_local_pool();
        if (__builtin_expect(lp != NULL, 1)) {
            return lp->get(id, std::forward<Args>(args)...);
        }
        return NULL;
    }

    inline int return_resource(ResourceId<T> id) {
        LocalPool* lp = get_or_new_local_pool();
        if (__builtin_expect(lp != NULL, 1)) {
            return lp->return_resource(id);
        }
        return -1;
    }

    void clear_resources() {
        LocalPool* lp = _local_pool;
        if (lp) {
            _local_pool = NULL;
            butil::thread_atexit_cancel(LocalPool::delete_local_pool, lp);
            delete lp;
        }
    }

    static inline size_t free_chunk_nitem() {
        const size_t n = ResourcePoolFreeChunkMaxItem<T>::value();
        return n < FREE_CHUNK_NITEM ? n : FREE_CHUNK_NITEM;
    }

    template <typename F>
    void for_each_resource(F const& f) {
        for (size_t i = 0; i < _ngroup.load(butil::memory_order_acquire); ++i) {
            BlockGroup* bg = _block_groups[i].load(butil::memory_order_consume);
            if (NULL == bg) {
                break;
            }
            size_t nblock = std::min(bg->nblock.load(butil::memory_order_relaxed),
                                     RP_GROUP_NBLOCK);
            for (size_t j = 0; j < nblock; ++j) {
                Block* b = bg->blocks[j].load(butil::memory_order_consume);
                if (NULL != b) {
                    for (size_t k = 0; k < b->nitem; ++k) {
                        auto item = b->items + k;
                        T* obj = (T*)item->void_data();
                        f(obj);
                    }
                }
            }
        }
    }
    
    // Number of all allocated objects, including being used and free.
    ResourcePoolInfo describe_resources() const {
        ResourcePoolInfo info;
        info.local_pool_num = _nlocal.load(butil::memory_order_relaxed);
        info.block_group_num = _ngroup.load(butil::memory_order_acquire);
        info.block_num = 0;
        info.item_num = 0;
        info.free_chunk_item_num = free_chunk_nitem();
        info.block_item_num = BLOCK_NITEM;
#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
        info.free_item_num = _global_nfree.load(butil::memory_order_relaxed);
#endif

        for (size_t i = 0; i < info.block_group_num; ++i) {
            BlockGroup* bg = _block_groups[i].load(butil::memory_order_consume);
            if (NULL == bg) {
                break;
            }
            size_t nblock = std::min(bg->nblock.load(butil::memory_order_relaxed),
                                     RP_GROUP_NBLOCK);
            info.block_num += nblock;
            for (size_t j = 0; j < nblock; ++j) {
                Block* b = bg->blocks[j].load(butil::memory_order_consume);
                if (NULL != b) {
                    info.item_num += b->nitem;
                }
            }
        }
        info.total_size = info.block_num * info.block_item_num * sizeof(T);
        return info;
    }

    static inline ResourcePool* singleton() {
        ResourcePool* p = _singleton.load(butil::memory_order_consume);
        if (p) {
            return p;
        }
        pthread_mutex_lock(&_singleton_mutex);
        p = _singleton.load(butil::memory_order_consume);
        if (!p) {
            p = new ResourcePool();
            _singleton.store(p, butil::memory_order_release);
        } 
        pthread_mutex_unlock(&_singleton_mutex);
        return p;
    }

private:
    ResourcePool() {
        _free_chunks.reserve(RP_INITIAL_FREE_LIST_SIZE);
        pthread_mutex_init(&_free_chunks_mutex, NULL);
    }

    ~ResourcePool() {
        pthread_mutex_destroy(&_free_chunks_mutex);
    }

    // Create a Block and append it to right-most BlockGroup.
    static Block* add_block(size_t* index) {
        Block* const new_block = new (std::nothrow) Block;
        if (NULL == new_block) {
            return NULL;
        }

        size_t ngroup;
        do {
            ngroup = _ngroup.load(butil::memory_order_acquire);
            if (ngroup >= 1) {
                // 在最后一个 BlockGroup 中预留索引
                BlockGroup* const g =
                    _block_groups[ngroup - 1].load(butil::memory_order_consume);
                const size_t block_index =
                    g->nblock.fetch_add(1, butil::memory_order_relaxed);
                if (block_index < RP_GROUP_NBLOCK) {
                    // 发布 block 指针
                    g->blocks[block_index].store(
                        new_block, butil::memory_order_release);
                    *index = (ngroup - 1) * RP_GROUP_NBLOCK + block_index;
                    return new_block;
                }
                g->nblock.fetch_sub(1, butil::memory_order_relaxed);
            }
        } while (add_block_group(ngroup)); // 如果当前 BlockGroup 已满, 则申请新的

        // Fail to add_block_group.
        delete new_block;
        return NULL;
    }

    // Create a BlockGroup and append it to _block_groups.
    // Shall be called infrequently because a BlockGroup is pretty big.
    static bool add_block_group(size_t old_ngroup) {
        BlockGroup* bg = NULL;
        BAIDU_SCOPED_LOCK(_block_group_mutex); // 串行创建 BlockGroup, 因为频率低
        const size_t ngroup = _ngroup.load(butil::memory_order_acquire);
        if (ngroup != old_ngroup) {
            // Other thread got lock and added group before this thread.
            return true;
        }
        if (ngroup < RP_MAX_BLOCK_NGROUP) {
            bg = new(std::nothrow) BlockGroup;
            if (NULL != bg) {
                // Release fence is paired with consume fence in address() and
                // add_block() to avoid un-constructed bg to be seen by other
                // threads.
                _block_groups[ngroup].store(bg, butil::memory_order_release);
                _ngroup.store(ngroup + 1, butil::memory_order_release);
            }
        }
        return bg != NULL;
    }

    inline LocalPool* get_or_new_local_pool() {
	    LocalPool* lp = BAIDU_GET_VOLATILE_THREAD_LOCAL(_local_pool);
        if (lp != NULL) {
            return lp;
        }
        // 创建 & 初始化 _local_pool
        lp = new(std::nothrow) LocalPool(this);
        if (NULL == lp) {
            return NULL;
        }
        BAIDU_SCOPED_LOCK(_change_thread_mutex); //avoid race with clear()
	    BAIDU_SET_VOLATILE_THREAD_LOCAL(_local_pool, lp);
        butil::thread_atexit(LocalPool::delete_local_pool, lp); // 线程退出时删除 _local_pool
        _nlocal.fetch_add(1, butil::memory_order_relaxed);
        return lp;
    }

    void clear_from_destructor_of_local_pool() {
        // Remove tls
        _local_pool = NULL;

        if (_nlocal.fetch_sub(1, butil::memory_order_relaxed) != 1) {
            return;
        }

        // Can't delete global even if all threads(called ResourcePool
        // functions) quit because the memory may still be referenced by 
        // other threads. But we need to validate that all memory can
        // be deallocated correctly in tests, so wrap the function with 
        // a macro which is only defined in unittests.
#ifdef BAIDU_CLEAR_RESOURCE_POOL_AFTER_ALL_THREADS_QUIT
        BAIDU_SCOPED_LOCK(_change_thread_mutex);  // including acquire fence.
        // Do nothing if there're active threads.
        if (_nlocal.load(butil::memory_order_relaxed) != 0) {
            return;
        }
        // All threads exited and we're holding _change_thread_mutex to avoid
        // racing with new threads calling get_resource().

        // Clear global free list.
        FreeChunk dummy;
        while (pop_free_chunk(dummy));

        // Delete all memory
        const size_t ngroup = _ngroup.exchange(0, butil::memory_order_relaxed);
        for (size_t i = 0; i < ngroup; ++i) {
            BlockGroup* bg = _block_groups[i].load(butil::memory_order_relaxed);
            if (NULL == bg) {
                break;
            }
            size_t nblock = std::min(bg->nblock.load(butil::memory_order_relaxed),
                                     RP_GROUP_NBLOCK);
            for (size_t j = 0; j < nblock; ++j) {
                Block* b = bg->blocks[j].load(butil::memory_order_relaxed);
                if (NULL == b) {
                    continue;
                }
                for (size_t k = 0; k < b->nitem; ++k) {
                    T* const objs = (T*)b->items;
                    objs[k].~T();
                }
                delete b;
            }
            delete bg;
        }

        memset(_block_groups, 0, sizeof(BlockGroup*) * RP_MAX_BLOCK_NGROUP);
#endif
    }

private:
    bool pop_free_chunk(FreeChunk& c) {
        // Critical for the case that most return_object are called in
        // different threads of get_object.
        if (_free_chunks.empty()) {
            return false;
        }
        pthread_mutex_lock(&_free_chunks_mutex);
        if (_free_chunks.empty()) {
            // 加锁之后 double-check, 防止在 use-after-free 的情况
            pthread_mutex_unlock(&_free_chunks_mutex);
            return false;
        }
        // 从尾部 LIFO 取出 DynamicFreeChunk, 里面是一组 Chunk
        DynamicFreeChunk* p = _free_chunks.back();
        _free_chunks.pop_back();
        pthread_mutex_unlock(&_free_chunks_mutex); // 此时 p 被线程独占, 可以尽早释放锁
        // 把 chunks 复制到 thread-local chunk (通常是 _cur_free)
        c.nfree = p->nfree;
        memcpy(c.ids, p->ids, sizeof(*p->ids) * p->nfree);
        free(p); // 释放 ID 列表, 对应 T 对象存在 ResourcePool 的 Block 中
        return true;
    }

    bool push_free_chunk(const FreeChunk& c) {
        // 按实际 ID 数量申请可变长内存(所以用malloc), 头部大小 + ID 数组大小
        DynamicFreeChunk* p = (DynamicFreeChunk*)malloc(
            offsetof(DynamicFreeChunk, ids) + sizeof(*c.ids) * c.nfree);
        if (!p) {
            return false;
        }
        // 填充动态 Chunk
        p->nfree = c.nfree;
        memcpy(p->ids, c.ids, sizeof(*c.ids) * c.nfree);
        // 加锁发布到全局列表
        pthread_mutex_lock(&_free_chunks_mutex);
        _free_chunks.push_back(p);
        pthread_mutex_unlock(&_free_chunks_mutex);
        return true;
    }
    
    static butil::static_atomic<ResourcePool*> _singleton;
    static pthread_mutex_t _singleton_mutex;
    STATIC_MEMBER_BAIDU_VOLATILE_THREAD_LOCAL(LocalPool*, _local_pool);
    static butil::static_atomic<long> _nlocal; // local pool 的数量
    static butil::static_atomic<size_t> _ngroup;
    static pthread_mutex_t _block_group_mutex;
    static pthread_mutex_t _change_thread_mutex;
    static butil::static_atomic<BlockGroup*> _block_groups[RP_MAX_BLOCK_NGROUP];

    /*
     * 全局空闲 chunk 池:
     * _free_chunks
     *  ├── DynamicFreeChunk -> [nfree][id0][id1]...
     *  ├── DynamicFreeChunk -> [nfree][id0][id1]...
     *  └── ...
    */
    std::vector<DynamicFreeChunk*> _free_chunks;
    pthread_mutex_t _free_chunks_mutex;

#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
    static butil::static_atomic<size_t> _global_nfree;
#endif
};

// Declare template static variables:

template <typename T>
const size_t ResourcePool<T>::FREE_CHUNK_NITEM;

// 对于每一个 T 都有一个 thread-local 的 _local_pool
template <typename T>
BAIDU_THREAD_LOCAL typename ResourcePool<T>::LocalPool*
ResourcePool<T>::_local_pool = NULL;

template <typename T>
butil::static_atomic<ResourcePool<T>*> ResourcePool<T>::_singleton =
    BUTIL_STATIC_ATOMIC_INIT(NULL);

template <typename T>
pthread_mutex_t ResourcePool<T>::_singleton_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
butil::static_atomic<long> ResourcePool<T>::_nlocal = BUTIL_STATIC_ATOMIC_INIT(0);

template <typename T>
butil::static_atomic<size_t> ResourcePool<T>::_ngroup = BUTIL_STATIC_ATOMIC_INIT(0);

template <typename T>
pthread_mutex_t ResourcePool<T>::_block_group_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
pthread_mutex_t ResourcePool<T>::_change_thread_mutex =
    PTHREAD_MUTEX_INITIALIZER;

template <typename T>
butil::static_atomic<typename ResourcePool<T>::BlockGroup*>
ResourcePool<T>::_block_groups[RP_MAX_BLOCK_NGROUP] = {};

#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
template <typename T>
butil::static_atomic<size_t> ResourcePool<T>::_global_nfree = BUTIL_STATIC_ATOMIC_INIT(0);
#endif

template <typename T>
inline bool operator==(ResourceId<T> id1, ResourceId<T> id2) {
    return id1.value == id2.value;
}

template <typename T>
inline bool operator!=(ResourceId<T> id1, ResourceId<T> id2) {
    return id1.value != id2.value;
}

// Disable comparisons between different typed ResourceId
template <typename T1, typename T2>
bool operator==(ResourceId<T1> id1, ResourceId<T2> id2);

template <typename T1, typename T2>
bool operator!=(ResourceId<T1> id1, ResourceId<T2> id2);

inline std::ostream& operator<<(std::ostream& os,
                                ResourcePoolInfo const& info) {
    return os << "local_pool_num: " << info.local_pool_num
              << "\nblock_group_num: " << info.block_group_num
              << "\nblock_num: " << info.block_num
              << "\nitem_num: " << info.item_num
              << "\nblock_item_num: " << info.block_item_num
              << "\nfree_chunk_item_num: " << info.free_chunk_item_num
              << "\ntotal_size: " << info.total_size;
#ifdef BUTIL_RESOURCE_POOL_NEED_FREE_ITEM_NUM
              << "\nfree_num: " << info.free_item_num
#endif
           ;
}

}  // namespace butil

#endif  // BUTIL_RESOURCE_POOL_INL_H
