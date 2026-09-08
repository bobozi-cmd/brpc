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

// Date: Fri Jun  5 18:25:40 CST 2015

// Do small memory allocations on continuous blocks.

#ifndef BUTIL_ARENA_H
#define BUTIL_ARENA_H

#include <stdint.h>
#include "butil/macros.h"

namespace butil {

struct ArenaOptions {
    size_t initial_block_size;
    size_t max_block_size;

    // Constructed with default options.
    ArenaOptions();
};

// 批量分配、统一释放 的内存池, 主要优化大量生命周期相同的小对象/字符串分配.
// 从一块连续内存中不断移动游标分配, 不支持单独释放, 最终由 Arena 一次性回收
class Arena {
public:
    explicit Arena(const ArenaOptions& options = ArenaOptions());
    ~Arena();
    void swap(Arena&);
    void* allocate(size_t n);
    void* allocate_aligned(size_t n);  // not implemented.
    void clear();

private:
    DISALLOW_COPY_AND_ASSIGN(Arena);
    // 每次向系统申请的内存都组织成一个 Block
    struct Block {
        // 剩余空间
        uint32_t left_space() const { return size - alloc_size; }
        
        Block* next; // 把不再作为当前块的内存串成链表
        uint32_t alloc_size; // 当前已经使用了多少字节，相当于分配游标
        uint32_t size; // 该块 data 区域的总大小
        char data[0];
    };

    void* allocate_in_other_blocks(size_t n);
    void* allocate_new_block(size_t n);
    Block* pop_block(Block* & head) {
        Block* saved_head = head;
        head = head->next;
        return saved_head;
    }
    
    Block* _cur_block; // 当前用于连续小内存分配的"热块"
    Block* _isolated_blocks; // 独立大块以及以前用完的当前块组成的链表, 本质上是"析构时需要释放的其他所有块"
    size_t _block_size; // 下一次普通块采用的目标大小
    ArenaOptions _options; // 初始块和最大普通块大小
};
// 高频调用, 在头文件中 inline, 不保证每次分配都对齐
inline void* Arena::allocate(size_t n) {
    if (_cur_block != NULL && _cur_block->left_space() >= n) {
        // 当前块还有足够空间时, 计算当前游标位置
        void* ret = _cur_block->data + _cur_block->alloc_size;
        // 将 alloc_size 向后移动 n 字节
        _cur_block->alloc_size += n;
        // 将 alloc_size 向后移动 n 字节
        return ret;
    }
    return allocate_in_other_blocks(n);
}

}  // namespace butil

#endif  // BUTIL_ARENA_H
