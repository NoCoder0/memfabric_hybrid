/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef MEMFABRIC_HYBRID_ACC_OFFLOAD_ENTRY_H
#define MEMFABRIC_HYBRID_ACC_OFFLOAD_ENTRY_H

#include <cstddef>
#include <cstdint>
#include "acc_offload.h"
#include "acc_offload_logger.h"

namespace ock {
namespace offload {

class AccOffloadEntry {
public:
    virtual ~AccOffloadEntry() = default;

    virtual int32_t Initialize(const offload_config_t &config) = 0;

    virtual void UnInitialize() = 0;

    virtual void *MallocHost(size_t size) = 0;

    virtual void FreeHost(void *ptr) = 0;

    virtual int32_t GetDva(uint64_t hostPtr, uint64_t *dvaPtr) = 0;

    virtual uint64_t HostMalloc(uint64_t size, uint64_t flags)
    {
        (void)flags;
        return reinterpret_cast<uint64_t>(MallocHost(size));
    }

    virtual int32_t HostRegister(uint64_t hostPtr) = 0;

    virtual int32_t HostGetDevicePtr(uint64_t hostPtr, uint64_t *dvaPtr)
    {
        return GetDva(hostPtr, dvaPtr);
    }

    virtual int32_t HostFree(uint64_t hostPtr, uint64_t flags) = 0;

    /* optional per entry: entries that do not override reject with a logged error */
    virtual int32_t SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr,
                               uint8_t devIdx)
    {
        (void)srcPtrs;
        (void)dstPtrs;
        (void)lenPtrs;
        (void)sizePtr;
        (void)devIdx;
        OFFLOAD_LOG_ERROR("sparse copy is not supported by this pool entry");
        return -1;
    }

    virtual int32_t GroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *numLocalExpertPtr,
                                  int64_t *groupList, int64_t *packedGroupList, uint8_t devIdx)
    {
        (void)srcPtrs;
        (void)dstPtrs;
        (void)lenPtrs;
        (void)numLocalExpertPtr;
        (void)groupList;
        (void)packedGroupList;
        (void)devIdx;
        OFFLOAD_LOG_ERROR("group pack copy is not supported by this pool entry");
        return -1;
    }

    virtual int32_t KvExchangeCopy(uint64_t *metaPtr, uint8_t devIdx)
    {
        (void)metaPtr;
        (void)devIdx;
        OFFLOAD_LOG_ERROR("kv exchange copy is not supported by this pool entry");
        return -1;
    }

    virtual int32_t RegisterEntryTable(uint32_t entryBytes, uint32_t rowsPerSlot)
    {
        (void)rowsPerSlot;
        OFFLOAD_LOG_ERROR("entry table is not supported by this pool entry, entryBytes: " << entryBytes);
        return -1;
    }

    virtual int32_t EntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint8_t devIdx)
    {
        (void)dstPtr;
        (void)idsPtr;
        (void)countPtr;
        OFFLOAD_LOG_ERROR("entry gather is not supported by this pool entry, deviceId: " << devIdx);
        return -1;
    }
};

} // namespace offload
} // namespace ock

#endif // MEMFABRIC_HYBRID_ACC_OFFLOAD_ENTRY_H
