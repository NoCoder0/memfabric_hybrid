/* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#ifndef MF_HYBM_DEVICE_COPY_COMMON_H
#define MF_HYBM_DEVICE_COPY_COMMON_H

#include <cstddef>
#include <cstdint>

constexpr uint32_t HYBM_DEVICE_COPY_MAX_RECORDS = 65536;

inline void HybmDeviceCopyCacheRange(const void *pointer, size_t bytes, bool invalidate)
{
#if defined(__aarch64__)
    uint64_t ctr = 0;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t lineBytes = 4ULL << ((ctr >> 16U) & 0xfU);
    const auto begin = reinterpret_cast<uintptr_t>(pointer);
    for (uintptr_t line = begin & ~(lineBytes - 1U); line < begin + bytes; line += lineBytes) {
        if (invalidate) {
            __asm__ __volatile__("dc civac, %0" ::"r"(line) : "memory");
        } else {
            __asm__ __volatile__("dc cvac, %0" ::"r"(line) : "memory");
        }
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    (void)pointer;
    (void)bytes;
    (void)invalidate;
#endif
}

#endif
