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

// Date: 2017/07/27 23:07:06

#ifndef BTHREAD_PARKING_LOT_H
#define BTHREAD_PARKING_LOT_H

#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "bthread/sys_futex.h"

namespace bthread {

DECLARE_bool(parking_lot_no_signal_when_no_waiter);

// Park idle workers.

// worker 找不到任务时安全地睡眠, 新任务到来时唤醒 worker, 并避免"任务已经到来，worker 却刚好睡下"的丢失唤醒问题
class BAIDU_CACHELINE_ALIGNMENT ParkingLot {
public:
    /*
     * 保存某一时刻 _pending_signal 的快照, 如果之后版本发生变化，就不要基于旧的判断入睡
     */
    class State {
    public:
        State(): val(0) {}
        bool stopped() const { return val & 1; }
    private:
    friend class ParkingLot;
        State(int val) : val(val) {}
        int val;
    };

    ParkingLot()
        : _pending_signal(0), _waiter_num(0)
        , _no_signal_when_no_waiter(FLAGS_parking_lot_no_signal_when_no_waiter) {}

    // Wake up at most `num_task' workers.
    // Returns #workers woken up.
    int signal(int num_task) {
        // 先修改版本, 防止一个准备睡眠的 worker 真正睡下
        _pending_signal.fetch_add((num_task << 1), butil::memory_order_release);
        if (_no_signal_when_no_waiter && _waiter_num.load(butil::memory_order_relaxed) == 0) {
            return 0;
        }
        // 唤醒 futex waiter, 返回内核实际唤醒的线程数
        return futex_wake_private(&_pending_signal, num_task);
    }

    // Get a state for later wait().
    State get_state() {
        return _pending_signal.load(butil::memory_order_acquire);
    }

    // Wait for tasks.
    // If the `expected_state' does not match, wait() may finish directly.
    void wait(const State& expected_state) {
        // 只有版本没变才允许睡眠, 用户态快速检查
        if (get_state().val != expected_state.val) {
            // Fast path, no need to futex_wait.
            return;
        }
        if (_no_signal_when_no_waiter) {
            _waiter_num.fetch_add(1, butil::memory_order_relaxed);
        }
        // futex 再次原子比较并等待
        futex_wait_private(&_pending_signal, expected_state.val, NULL);
        if (_no_signal_when_no_waiter) {
            _waiter_num.fetch_sub(1, butil::memory_order_relaxed);
        }
    }

    // Wakeup suspended wait() and make them unwaitable ever. 
    void stop() {
        // 永久关闭 ParkingLot, 设置最低位S=1
        _pending_signal.fetch_or(1);
        // 一次性唤醒最多 10000 个等待者, worker 被唤醒之后会检查 _pending_signal 是否stopped, 然后退出 worker-loop
        futex_wake_private(&_pending_signal, 10000);
    }

private:
    /*
     * `31                     1 0`
     * `+----------------------+---+`
     * `| signal 版本/计数部分   | S |`
     * `+----------------------+-^-+`
     * `                         stop 标志, =1表示停止`
    */
    butil::atomic<int> _pending_signal;
    butil::atomic<int> _waiter_num; // 记录可能正在 futex 上等待的 worker 数量, 避免没有等待者时调用 futex_wake
    // Whether to signal when there is no waiter.
    // In busy worker scenarios, signal overhead
    // can be reduced.
    bool _no_signal_when_no_waiter; // 开启 _waiter_num 优化
};

}  // namespace bthread

#endif  // BTHREAD_PARKING_LOT_H
