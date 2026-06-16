/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef ZBAL_COMM_OP_TILINE_H
#define ZBAL_COMM_OP_TILINE_H

#include <map>
#include <memory>
#include <string>

namespace zbal {
namespace operators {

// ---------------------------------------------------------------------------
// Why this exists:
//
//   NpuCommunicatorDefault is the communication layer's core abstraction —
//   it manages topology, meta regions, and collective operations. Some
//   fused operators (e.g. fused_deep_moe) need device-side state that
//   outlives a single kernel call: tiling buffers allocated lazily on the
//   first invocation and reused thereafter, with a "written" flag so H2D
//   copies only happen once.
//
//   Adding per-operator members directly to NpuCommunicatorDefault violates
//   the Single Responsibility Principle: the Communicator should not know
//   about individual operator internals. As more fused operators are added,
//   the class would accumulate unrelated fields.
//
//   OpTilingStore solves this by providing a type-erased key-value slot.
//   Each operator defines its own state struct (with a destructor that
//   cleans up device allocations), and stores it under a unique string key.
//   The Communicator only holds the store — it never needs to know what's
//   inside.
//
// How to use:
//
//   1. Define a state struct inheriting from OpTilingBase in your .cpp file:
//
//        struct MyOpTiling : public OpTilingBase {
//            void *devBuf = nullptr;
//            bool  written = false;
//            ~MyOpTiling() { if (devBuf) DlCannApi::AclrtFree(devBuf); }
//        };
//
//   2. In the operator method, retrieve or create the state:
//
//        auto &s = opTilings_.GetOrCreate<MyOpTiling>("my_op");
//        if (!s.devBuf) {
//            DlCannApi::AclrtMalloc(&s.devBuf, size, ACL_MEM_MALLOC_NORMAL_ONLY);
//        }
//        // ... use s.devBuf, s.written ...
//
//   3. The state is automatically destroyed (calling ~MyOpTiling) when the
//      Communicator is destroyed (which destroys the OpTilingStore).
//
// Why this is organized this way:
//
//   - OpTilingBase provides a virtual destructor so that deleting through
//     the base pointer correctly invokes the derived destructor, which
//     includes operator-specific cleanup (e.g. AclrtFree of device buffers).
//     Without this, device memory would leak on Communicator teardown.
//
//   - The template GetOrCreate<T> avoids the need for static_cast at each
//     call site. The key string acts as both the map lookup and implicit
//     documentation of ownership.
//
//   - OpTilingStore is deliberately placed in the Communicator layer (not
//     the kernel layer) because the lifetime of operator state is tied to
//     the Communicator: when the communicator is destroyed, all associated
//     operator tiling must be released. This co-location is natural — the
//     store is simply another piece of the Communicator's internal
//     bookkeeping, alongside CommGroupInfo and profiling state.
// ---------------------------------------------------------------------------

class OpTilingBase {
public:
    virtual ~OpTilingBase() = default;
};

class OpTilingStore {
public:
    OpTilingStore() = default;
    ~OpTilingStore() = default;

    // Non-copyable, non-movable (states hold device pointers that shouldn't
    // be aliased).
    OpTilingStore(const OpTilingStore &) = delete;
    OpTilingStore &operator=(const OpTilingStore &) = delete;
    OpTilingStore(OpTilingStore &&) = delete;
    OpTilingStore &operator=(OpTilingStore &&) = delete;

    // Retrieve existing state or create a new one under `key`.
    // T must inherit from OpTilingBase and be default-constructible.
    template<typename T, typename... Args>
    T &GetOrCreate(const std::string &key, Args &&...args)
    {
        auto it = tilings_.find(key);
        if (it != tilings_.end()) {
            return *static_cast<T *>(it->second.get());
        }
        auto state = std::make_unique<T>(std::forward<Args>(args)...);
        T *ptr = state.get();
        tilings_.emplace(key, std::move(state));
        return *ptr;
    }

    // Check whether a key exists.
    bool Has(const std::string &key) const
    {
        return tilings_.find(key) != tilings_.end();
    }

    // Explicitly remove state for `key` (invokes destructor immediately).
    void Remove(const std::string &key)
    {
        tilings_.erase(key);
    }

    // Remove all states.
    void Clear()
    {
        tilings_.clear();
    }

private:
    std::map<std::string, std::unique_ptr<OpTilingBase>> tilings_;
};

} // namespace operators
} // namespace zbal

#endif // ZBAL_COMM_OP_TILINE_H
