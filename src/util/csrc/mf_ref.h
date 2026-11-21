/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef MF_REF_H
#define MF_REF_H

#include <cstdint>
#include <new>
#include <utility>

namespace ock {
namespace mf {
/*
 * 1 base class smart ptr
 * 2 macro for master ptr if not use base class
 */
#if defined(ENABLE_ARM_ATOMICS) && defined(__aarch64__)
static inline int16_t faa_int16_relax(int16_t *val, int16_t add)
{
    int16_t old;
    asm volatile("ldaddh %w[add], %w[old], [%[addr]]"
                 : [old] "=r"(old), "+Q"(*val)
                 : [addr] "r"(val), [add] "r"(add)
                 : "memory");
    return old;
}

static inline int16_t faa_int16_acq_rel(int16_t *val, int16_t add)
{
    int16_t old;
    asm volatile("ldaddalh %w[add], %w[old], [%[addr]]"
                 : [old] "=r"(old), "+Q"(*val)
                 : [addr] "r"(val), [add] "r"(add)
                 : "memory");
    return old;
}

static inline int16_t faa_int16_release(int16_t *val, int16_t add)
{
    int16_t old;
    asm volatile("ldaddlh %w[add], %w[old], [%[addr]]"
                 : [old] "=r"(old), "+Q"(*val)
                 : [addr] "r"(val), [add] "r"(add)
                 : "memory");
    return old;
}

#define ATOMIC_INT16_FAA_RELAX(VAR_PTR, N)   faa_int16_relax(VAR_PTR, N)
#define ATOMIC_INT16_FAA_ACQ_REL(VAR_PTR, N) faa_int16_acq_rel(VAR_PTR, N)
#define ATOMIC_INT16_FAA_REL(VAR_PTR, N)     faa_int16_release(VAR_PTR, N)

#else
#define ATOMIC_INT16_FAA_RELAX(VAR_PTR, N)   __atomic_fetch_add(VAR_PTR, N, __ATOMIC_RELAXED)
#define ATOMIC_INT16_FAA_ACQ_REL(VAR_PTR, N) __atomic_fetch_add(VAR_PTR, N, __ATOMIC_ACQ_REL)
#define ATOMIC_INT16_FAA_REL(VAR_PTR, N)     __atomic_fetch_add(VAR_PTR, N, __ATOMIC_RELEASE)
#endif

class Referable {
public:
    Referable() = default;
    virtual ~Referable() = default;

    inline void IncreaseRef()
    {
        ATOMIC_INT16_FAA_RELAX(&ref_count_, 1);
    }

    inline void DecreaseRef()
    {
        if (ATOMIC_INT16_FAA_REL(&ref_count_, -1) == 1) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            delete this;
        }
    }

protected:
    int16_t ref_count_ = 0;
};

template<typename T>
class Ref {
public:
    Ref() noexcept = default;

    /* fix: can't be explicit */
    Ref(T *newObj) noexcept
    {
        /*
         * if new obj is not null, increase reference count and assign to mObj
         * else nothing need to do as mObj is nullptr by default
         */
        if (newObj != nullptr) {
            newObj->IncreaseRef();
            mObj = newObj;
        }
    }

    Ref(const Ref<T> &other) noexcept
    {
        /*
         * if other's obj is not null, increase reference count and assign to mObj
         * else nothing need to do as mObj is nullptr by default
         */
        if (other.mObj != nullptr) {
            other.mObj->IncreaseRef();
            mObj = other.mObj;
        }
    }

    Ref(Ref<T> &&other) noexcept : mObj(std::exchange(other.mObj, nullptr))
    {
        /*
         * move constructor
         * since this mObj is null, just exchange
         */
    }

    ~Ref()
    {
        if (mObj != nullptr) {
            mObj->DecreaseRef();
        }
    }

    inline Ref<T> &operator=(T *newObj)
    {
        this->Set(newObj);
        return *this;
    }

    inline Ref<T> &operator=(const Ref<T> &other)
    {
        if (this != &other) {
            this->Set(other.mObj);
        }
        return *this;
    }

    Ref<T> &operator=(Ref<T> &&other) noexcept
    {
        if (this != &other) {
            auto tmp = mObj;
            mObj = std::__exchange(other.mObj, nullptr);
            if (tmp != nullptr) {
                tmp->DecreaseRef();
            }
        }
        return *this;
    }

    inline bool operator==(const Ref<T> &other) const
    {
        return mObj == other.mObj;
    }

    inline bool operator==(T *other) const
    {
        return mObj == other;
    }

    inline bool operator!=(const Ref<T> &other) const
    {
        return mObj != other.mObj;
    }

    inline bool operator!=(T *other) const
    {
        return mObj != other;
    }

    inline T *operator->() const
    {
        return mObj;
    }

    inline T *Get() const
    {
        return mObj;
    }

    inline void Set(T *newObj)
    {
        if (newObj == mObj) {
            return;
        }

        if (newObj != nullptr) {
            newObj->IncreaseRef();
        }

        if (mObj != nullptr) {
            mObj->DecreaseRef();
        }

        mObj = newObj;
    }

private:
    T *mObj = nullptr;
};

template<typename C, typename... ARGS>
inline Ref<C> MakeRef(ARGS... args)
{
    return new (std::nothrow) C(args...);
}

template<typename To, typename From>
Ref<To> RefDynamicCast(const Ref<From> &from)
{
    return Ref<To>(dynamic_cast<To *>(from.Get()));
}

/**
 * @param zero overhead cast, use this if the following conditions satisfied:
 * 1 if you sure the the child class is certain class, you can use this one
 * 2 if you don't switch thread and make sure you hold the reference
 *
 * @tparam To              [in] target class type
 * @tparam From            [in] source class type
 * @param from             [in] source ref object
 * @return target raw ptr
 */
template<typename To, typename From>
inline To *RefStaticCastToRaw(const Ref<From> &from)
{
    return static_cast<To *>(from.Get());
}
} // namespace mf
} // namespace ock

#endif //MF_REF_H
