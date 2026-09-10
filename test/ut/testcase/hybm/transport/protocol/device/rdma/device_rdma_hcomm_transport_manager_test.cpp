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

#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#define private   public
#define protected public
#include "device_rdma_hcomm_transport_manager.h"
#include "dl_acl_api.h"
#undef private
#undef protected

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::device;

namespace {
const EndpointHandle MOCK_ENDPOINT = reinterpret_cast<EndpointHandle>(0xA001UL);
const HcommMemHandle MOCK_MEM_HANDLE = reinterpret_cast<HcommMemHandle>(0xA002UL);
constexpr uint64_t MOCK_LOCAL_ADDR = 0x100000UL;
constexpr uint64_t MOCK_SIZE = 0x1000UL;
constexpr ChannelHandle MOCK_CHANNEL = 0xA003UL;
constexpr ThreadHandle MOCK_THREAD = 0xA004UL;
constexpr uint32_t MOCK_PORT = 5000U;

// ── DlAclApi function pointer guard ──
// DlAclApi inline functions call through function pointers (pAclrtGetDevice etc).
// We set them directly (private→public makes them accessible).
struct DlAclApiFnGuard {
    aclrtGetDeviceFunc oldGetDevice{DlAclApi::pAclrtGetDevice};
    aclrtGetPhyDevIdByLogicDevIdFunc oldGetPhyDev{DlAclApi::pAclrtGetPhyDevIdByLogicDevId};
    rtGetDeviceInfoFunc oldGetDeviceInfo{DlAclApi::pRtGetDeviceInfo};
    aclrtGetVersionFunc oldGetVersion{DlAclApi::pAclrtGetVersion};
    aclrtMallocFunc oldMalloc{DlAclApi::pAclrtMalloc};
    aclrtFreeFunc oldFree{DlAclApi::pAclrtFree};
    aclrtMemcpyFunc oldMemcpy{DlAclApi::pAclrtMemcpy};
    aclrtCreateStreamFunc oldCreateStream{DlAclApi::pAclrtCreateStream};
    aclrtDestroyStreamFunc oldDestroyStream{DlAclApi::pAclrtDestroyStream};
    aclrtCreateNotifyFunc oldCreateNotify{DlAclApi::pAclrtCreateNotify};
    aclrtDestroyNotifyFunc oldDestroyNotify{DlAclApi::pAclrtDestroyNotify};
    aclrtWaitAndResetNotifyFunc oldWaitAndResetNotify{DlAclApi::pAclrtWaitAndResetNotify};

    ~DlAclApiFnGuard()
    {
        DlAclApi::pAclrtGetDevice = oldGetDevice;
        DlAclApi::pAclrtGetPhyDevIdByLogicDevId = oldGetPhyDev;
        DlAclApi::pRtGetDeviceInfo = oldGetDeviceInfo;
        DlAclApi::pAclrtGetVersion = oldGetVersion;
        DlAclApi::pAclrtMalloc = oldMalloc;
        DlAclApi::pAclrtFree = oldFree;
        DlAclApi::pAclrtMemcpy = oldMemcpy;
        DlAclApi::pAclrtCreateStream = oldCreateStream;
        DlAclApi::pAclrtDestroyStream = oldDestroyStream;
        DlAclApi::pAclrtCreateNotify = oldCreateNotify;
        DlAclApi::pAclrtDestroyNotify = oldDestroyNotify;
        DlAclApi::pAclrtWaitAndResetNotify = oldWaitAndResetNotify;
    }
};

// Default HCOMM mocks used by all DeviceRdmaHcommTransportManagerTest tests
int32_t StubHcommEndpointCreate(const EndpointDesc *, EndpointHandle *handle)
{
    *handle = reinterpret_cast<EndpointHandle>(0xA001UL);
    return 0;
}
int32_t StubHcommMemReg(EndpointHandle, const char *, const HcommCommMem *, HcommMemHandle *handle)
{
    *handle = reinterpret_cast<HcommMemHandle>(0xB001UL);
    return 0;
}
int32_t StubHcommMemUnreg(EndpointHandle, HcommMemHandle)
{
    return 0;
}
int32_t StubHcommMemExport(EndpointHandle, HcommMemHandle, void **desc, uint32_t *descLen)
{
    static uint8_t stubDesc[16]{};
    *desc = stubDesc;
    *descLen = sizeof(stubDesc);
    return 0;
}
int32_t StubHcommMemImport(EndpointHandle, const void *, uint32_t, HcommCommMem *outMem)
{
    outMem->type = COMM_MEM_TYPE_DEVICE;
    outMem->addr = reinterpret_cast<void *>(0xC001UL);
    outMem->size = 0x1000UL;
    return 0;
}
int32_t StubHcommMemUnimport(EndpointHandle, const void *, uint32_t)
{
    return 0;
}
int32_t StubHcommChannelCreate(EndpointHandle, CommEngine, HcommChannelDesc *, uint32_t, ChannelHandle *ch)
{
    *ch = 0xA003UL;
    return 0;
}
int32_t StubHcommChannelDestroy(const ChannelHandle *, uint32_t)
{
    return 0;
}
int32_t StubHcommChannelGetStatus(const ChannelHandle *, uint32_t, int32_t *status)
{
    *status = 0;
    return 0;
}
int32_t StubHcommThreadAlloc(CommEngine, uint32_t, const uint32_t *, ThreadHandle *thread)
{
    *thread = 0xA004UL;
    return 0;
}
int32_t StubHcommThreadFree(const ThreadHandle *, uint32_t)
{
    return 0;
}
int32_t StubHcommEndpointDestroy(EndpointHandle)
{
    return 0;
}

void SetupDefaultHcommMocks()
{
    DlHcommApi::gHcommEndpointCreate = StubHcommEndpointCreate;
    DlHcommApi::gHcommEndpointDestroy = StubHcommEndpointDestroy;
    DlHcommApi::gHcommMemReg = StubHcommMemReg;
    DlHcommApi::gHcommMemUnreg = StubHcommMemUnreg;
    DlHcommApi::gHcommMemExport = StubHcommMemExport;
    DlHcommApi::gHcommMemImport = StubHcommMemImport;
    DlHcommApi::gHcommMemUnimport = StubHcommMemUnimport;
    DlHcommApi::gHcommChannelCreate = StubHcommChannelCreate;
    DlHcommApi::gHcommChannelDestroy = StubHcommChannelDestroy;
    DlHcommApi::gHcommChannelGetStatus = StubHcommChannelGetStatus;
    DlHcommApi::gHcommThreadAlloc = StubHcommThreadAlloc;
    DlHcommApi::gHcommThreadFree = StubHcommThreadFree;
}

// ── Hcomm API function pointer guard (saves/restores all HCOMM state) ──
struct DlHcommApiFnGuard {
    bool oldLoaded{DlHcommApi::gLoaded};
    hcommEndpointCreateFunc oldEndpointCreate{DlHcommApi::gHcommEndpointCreate};
    hcommEndpointDestroyFunc oldEndpointDestroy{DlHcommApi::gHcommEndpointDestroy};
    hcommMemRegFunc oldMemReg{DlHcommApi::gHcommMemReg};
    hcommMemUnregFunc oldMemUnreg{DlHcommApi::gHcommMemUnreg};
    hcommMemExportFunc oldMemExport{DlHcommApi::gHcommMemExport};
    hcommMemImportFunc oldMemImport{DlHcommApi::gHcommMemImport};
    hcommMemUnimportFunc oldMemUnimport{DlHcommApi::gHcommMemUnimport};
    hcommChannelCreateFunc oldChannelCreate{DlHcommApi::gHcommChannelCreate};
    hcommChannelDestroyFunc oldChannelDestroy{DlHcommApi::gHcommChannelDestroy};
    hcommChannelGetStatusFunc oldChannelGetStatus{DlHcommApi::gHcommChannelGetStatus};
    hcommThreadAllocFunc oldThreadAlloc{DlHcommApi::gHcommThreadAlloc};
    hcommThreadFreeFunc oldThreadFree{DlHcommApi::gHcommThreadFree};
    hcommReadOnThreadFunc oldReadOnThread{DlHcommApi::gHcommReadOnThread};
    hcommWriteOnThreadFunc oldWriteOnThread{DlHcommApi::gHcommWriteOnThread};
    hcommBatchTransferOnThreadFunc oldBatchTransferOnThread{DlHcommApi::gHcommBatchTransferOnThread};

    ~DlHcommApiFnGuard()
    {
        DlHcommApi::gLoaded = oldLoaded;
        DlHcommApi::gHcommEndpointCreate = oldEndpointCreate;
        DlHcommApi::gHcommEndpointDestroy = oldEndpointDestroy;
        DlHcommApi::gHcommMemReg = oldMemReg;
        DlHcommApi::gHcommMemUnreg = oldMemUnreg;
        DlHcommApi::gHcommMemExport = oldMemExport;
        DlHcommApi::gHcommMemImport = oldMemImport;
        DlHcommApi::gHcommMemUnimport = oldMemUnimport;
        DlHcommApi::gHcommChannelCreate = oldChannelCreate;
        DlHcommApi::gHcommChannelDestroy = oldChannelDestroy;
        DlHcommApi::gHcommChannelGetStatus = oldChannelGetStatus;
        DlHcommApi::gHcommThreadAlloc = oldThreadAlloc;
        DlHcommApi::gHcommThreadFree = oldThreadFree;
        DlHcommApi::gHcommReadOnThread = oldReadOnThread;
        DlHcommApi::gHcommWriteOnThread = oldWriteOnThread;
        DlHcommApi::gHcommBatchTransferOnThread = oldBatchTransferOnThread;
    }
};

TransportOptions MakeOptions()
{
    TransportOptions opts{};
    opts.rankId = 0;
    opts.rankCount = 2; // 2
    opts.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    opts.initialType = HYBM_TYPE_AICPU_INITIATE;
    opts.role = HYBM_ROLE_PEER;
    opts.nic = "192.168.1.1:5000";
    return opts;
}

class DeviceRdmaHcommTransportManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        SetupDlAclMocks();
        guard_ = std::make_unique<DlHcommApiFnGuard>();
        DlHcommApi::gLoaded = true; // bypass dlopen(libhcomm.so)
        SetupDefaultHcommMocks();
        // Set up fake EID file so BuildLocalEndpointDesc can find NPU IP
        SetupEidFile();
    }

    void TearDown() override
    {
        guard_.reset();
        aclGuard_.reset();
        TeardownEidFile();
    }

    void SetupEidFile()
    {
        // Pipe-based EID: no disk write, works everywhere /proc is mounted
        int pipeFds[2];
        if (pipe(pipeFds) == 0) {
            std::string content = "0:192.168.1.100\n";
            write(pipeFds[1], content.data(), content.size());
            close(pipeFds[1]);
            eidFdPath_ = "/proc/self/fd/" + std::to_string(pipeFds[0]);
            eidPipeRd_ = pipeFds[0];
            oldEidEnv_ = std::getenv("MF_DEVICE_URMA_EID_FILE");
            setenv("MF_DEVICE_URMA_EID_FILE", eidFdPath_.c_str(), 1);
        }
    }

    void TeardownEidFile()
    {
        if (eidPipeRd_ >= 0) {
            close(eidPipeRd_);
            eidPipeRd_ = -1;
        }
        if (oldEidEnv_) {
            setenv("MF_DEVICE_URMA_EID_FILE", oldEidEnv_, 1);
        } else {
            unsetenv("MF_DEVICE_URMA_EID_FILE");
        }
    }

    void SetupDlAclMocks()
    {
        aclGuard_ = std::make_unique<DlAclApiFnGuard>();

        DlAclApi::pAclrtGetDevice = [](int32_t *deviceId) -> int32_t {
            *deviceId = 0;
            return 0;
        };
        DlAclApi::pAclrtGetPhyDevIdByLogicDevId = [](int32_t, int32_t *phyId) -> int32_t {
            *phyId = 0;
            return 0;
        };
        DlAclApi::pRtGetDeviceInfo = [](uint32_t, int32_t, int32_t infoType, int64_t *value) -> int32_t {
            if (infoType == INFO_TYPE_SDID || infoType == INFO_TYPE_SERVER_ID || infoType == INFO_TYPE_SUPER_POD_ID) {
                *value = 0;
            }
            return 0;
        };
        DlAclApi::pAclrtGetVersion = [](int32_t *major, int32_t *minor, int32_t *patch) -> int32_t {
            *major = 9; // 9
            *minor = 1; // 1
            *patch = 0; // 0
            return 0;
        };
        DlAclApi::pAclrtMalloc = [](void **ptr, size_t, uint32_t) -> int32_t {
            *ptr = reinterpret_cast<void *>(0xD001UL);
            return 0;
        };
        DlAclApi::pAclrtMemcpy = [](void *, size_t, const void *, size_t, uint32_t) -> int32_t { return 0; };
        DlAclApi::pAclrtFree = [](void *) -> int32_t { return 0; };
        DlAclApi::pAclrtCreateStream = [](void **stream) -> int32_t {
            *stream = reinterpret_cast<void *>(0xE001UL);
            return 0;
        };
        DlAclApi::pAclrtDestroyStream = [](void *) -> int32_t { return 0; };
        DlAclApi::pAclrtCreateNotify = [](void **notify, uint64_t) -> int32_t {
            *notify = reinterpret_cast<void *>(0xE002UL);
            return 0;
        };
        DlAclApi::pAclrtDestroyNotify = [](void *) -> int32_t { return 0; };
        DlAclApi::pAclrtWaitAndResetNotify = [](void *, void *, uint32_t) -> int32_t { return 0; };
    }

    std::unique_ptr<DlAclApiFnGuard> aclGuard_;
    std::unique_ptr<DlHcommApiFnGuard> guard_;
    int eidPipeRd_{-1};
    std::string eidFdPath_{};
    const char *oldEidEnv_{nullptr};
};

// Create a DeviceRdmaHcommTransportManager with kernel handles preset (bypasses real AICPU kernel)
inline std::shared_ptr<DeviceRdmaHcommTransportManager> CreateHcommMgr()
{
    auto mgr = std::make_shared<DeviceRdmaHcommTransportManager>();
    mgr->deviceFuncHandles_.batchWrite = reinterpret_cast<aclrtFuncHandle>(0x1001UL);
    mgr->deviceFuncHandles_.batchRead = reinterpret_cast<aclrtFuncHandle>(0x1002UL);
    return mgr;
}

// CloseDevice helper (file-scope for non-capturing function pointer)
bool gEndpointDestroyed = false;
int32_t StubEndpointDestroy(EndpointHandle)
{
    gEndpointDestroyed = true;
    return 0;
}

} // namespace

TEST_F(DeviceRdmaHcommTransportManagerTest, OpenDevice_Success)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };

    auto mgr = CreateHcommMgr();
    auto opts = MakeOptions();
    auto ret = mgr->OpenDevice(opts);
    ASSERT_EQ(ret, BM_OK);
    ASSERT_EQ(mgr->localEndpoint_, MOCK_ENDPOINT);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, OpenDevice_Fail_EndpointCreate)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *) -> int32_t { return -1; };

    auto mgr = CreateHcommMgr();
    auto ret = mgr->OpenDevice(MakeOptions());
    ASSERT_EQ(ret, BM_DL_FUNCTION_FAILED);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, RegisterAndUnregisterMemory)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = MOCK_MEM_HANDLE;
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    TransportMemoryRegion mr{};
    mr.addr = MOCK_LOCAL_ADDR;
    mr.size = MOCK_SIZE;
    mr.flags = REG_MR_FLAG_HBM;

    auto ret = mgr->RegisterMemoryRegion(mr);
    ASSERT_EQ(ret, BM_OK);

    ret = mgr->UnregisterMemoryRegion(MOCK_LOCAL_ADDR);
    ASSERT_EQ(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, GetPrivateData)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    auto priv = mgr->GetPrivateData();
    RdmaHcommPrivateData parsed{};
    std::memcpy(&parsed, priv.key.keys, sizeof(parsed));
    ASSERT_EQ(parsed.magic, RDMA_HCOMM_PRIVATE_DATA_MAGIC);
    ASSERT_EQ(parsed.version, RDMA_HCOMM_PRIVATE_DATA_VERSION);
    ASSERT_EQ(parsed.protocol, COMM_PROTOCOL_ROCE);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, CloseDevice)
{
    gEndpointDestroyed = false;
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = StubEndpointDestroy;

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);
    auto ret = mgr->CloseDevice();
    ASSERT_EQ(ret, BM_OK);
    ASSERT_TRUE(gEndpointDestroyed);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, CloseDevice_WithResources)
{
    gEndpointDestroyed = false;
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = StubEndpointDestroy;
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB002UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    // Register some memory to create localRegistrations_
    TransportMemoryRegion mr{};
    mr.addr = MOCK_LOCAL_ADDR;
    mr.size = MOCK_SIZE;
    mr.flags = REG_MR_FLAG_HBM;
    ASSERT_EQ(mgr->RegisterMemoryRegion(mr), BM_OK);

    // Add a CompletionContext with notify/stream/pending transfers
    auto ctx = std::make_shared<DeviceRdmaHcommTransportManager::CompletionContext>();
    ctx->notify = reinterpret_cast<void *>(0xE002UL);
    ctx->notifyId = 42U;
    ctx->notifyAddr = 0x3000UL;
    ctx->notifyLen = 0x100UL;
    ctx->notifyHcommHandle = reinterpret_cast<HcommMemHandle>(0xB003UL);
    ctx->stream = reinterpret_cast<void *>(0xE001UL);
    // Add a pending transfer
    ctx->pendingTransfers.emplace_back();
    ctx->pendingTransfers.back().rankId = 1;
    mgr->registry_.push_back(ctx);

    // Close should clean everything up
    auto ret = mgr->CloseDevice();
    ASSERT_EQ(ret, BM_OK);
    ASSERT_TRUE(gEndpointDestroyed);
    ASSERT_TRUE(mgr->registry_.empty());
    ASSERT_TRUE(mgr->localRegistrations_.empty());
}

TEST_F(DeviceRdmaHcommTransportManagerTest, OpenDevice_RejectInvalidOptions)
{
    auto mgr = CreateHcommMgr();
    auto opts = MakeOptions();
    opts.rankCount = 0; // 0
    auto ret = mgr->OpenDevice(opts);
    ASSERT_EQ(ret, BM_INVALID_PARAM);

    opts.rankCount = 2; // 2
    opts.rankId = 5;    // 5
    ret = mgr->OpenDevice(opts);
    ASSERT_EQ(ret, BM_INVALID_PARAM);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, ConvertGvaToDva_UnregisteredAddr)
{
    // No registrations → ConvertGvaToDva returns input addr unchanged
    auto mgr = CreateHcommMgr();
    uint64_t dva = mgr->ConvertGvaToDva(0xDEAD0000UL);
    ASSERT_EQ(dva, 0xDEAD0000UL);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, ImportPeerMems_EmptyKeys)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    // Empty memKeys → OK
    auto ret = mgr->ImportPeerMems(1, {});
    ASSERT_EQ(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, ReadWriteAsync_NotOpened_Fails)
{
    auto mgr = CreateHcommMgr();
    constexpr uint64_t SZ = 1024;
    // RemoteIo checks remoteRanks_; empty → not connected
    ASSERT_EQ(mgr->ReadRemoteAsync(0, 0, 0, SZ), BM_NOT_CONNECTED);
    ASSERT_EQ(mgr->WriteRemoteAsync(0, 0, 0, SZ), BM_NOT_CONNECTED);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, UnregisterMemoryUnknownAddr)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    // Unregistering an address that was never registered → OK (no-op)
    auto ret = mgr->UnregisterMemoryRegion(0xCAFE0000UL);
    ASSERT_EQ(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, RemoteIoZeroSizeReturnsOk)
{
    auto mgr = CreateHcommMgr();
    // RemoteIo returns BM_OK immediately for size==0, no remote rank needed
    ASSERT_EQ(mgr->ReadRemoteAsync(1, 0x1000, 0x2000, 0), BM_OK);
    ASSERT_EQ(mgr->WriteRemoteAsync(1, 0x1000, 0x2000, 0), BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, QueryMemoryKeyBeforeRegister)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    // QueryMemoryKey for unregistered address → should fail or return empty key
    TransportMemoryKey key{};
    auto ret = mgr->QueryMemoryKey(0xCAFE0000UL, key);
    ASSERT_NE(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, CloseDeviceWithoutOpen)
{
    auto mgr = CreateHcommMgr();
    // CloseDevice when never opened → should be OK (no-op)
    auto ret = mgr->CloseDevice();
    ASSERT_EQ(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, GetPrivateDataWithoutOpen)
{
    auto mgr = CreateHcommMgr();
    // GetPrivateData without OpenDevice → should return empty descriptor
    RdmaHcommPrivateData priv{};
    auto ret = mgr->GetPrivateData();
    ASSERT_EQ(ret.key.keys[0], 0UL);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, RegisterMemoryDramFallback)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = MOCK_MEM_HANDLE;
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    // DRAM flag - should try DVA mapping, fallback to HVA if no DVA available
    // MOCK_LOCAL_ADDR = 0x100000, no DVA mapping exists in mock → should use HVA
    TransportMemoryRegion mr{};
    mr.addr = MOCK_LOCAL_ADDR;
    mr.size = MOCK_SIZE;
    mr.flags = REG_MR_FLAG_DRAM;
    auto ret = mgr->RegisterMemoryRegion(mr);
    ASSERT_EQ(ret, BM_OK);

    ret = mgr->UnregisterMemoryRegion(MOCK_LOCAL_ADDR);
    ASSERT_EQ(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, RegisterEndpointWithHbm)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB002UL);
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    // HBM memory type → should register on HBM address directly
    TransportMemoryRegion mr{};
    mr.addr = 0x2000000000UL;
    mr.size = 0x100000UL;
    mr.flags = REG_MR_FLAG_HBM;
    auto ret = mgr->RegisterMemoryRegion(mr);
    ASSERT_EQ(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, RegisterMemoryRegionZeroSize)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    TransportMemoryRegion mr{};
    mr.addr = MOCK_LOCAL_ADDR;
    mr.size = 0;
    mr.flags = REG_MR_FLAG_HBM;
    auto ret = mgr->RegisterMemoryRegion(mr);
    ASSERT_NE(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, OpenDeviceCreatesDevStreamAndNotify)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    auto ret = mgr->OpenDevice(MakeOptions());
    ASSERT_EQ(ret, BM_OK);
    // OpenDevice should create kernel stream, transfer flag buffer, and generate
    ASSERT_NE(mgr->openGeneration_, nullptr);
    ASSERT_EQ(mgr->openGeneration_->id, 1U);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, RegisterAndQueryMemoryHbm)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = MOCK_MEM_HANDLE;
        return 0;
    };
    DlHcommApi::gHcommMemUnreg = [](EndpointHandle, HcommMemHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    TransportMemoryRegion mr{};
    mr.addr = MOCK_LOCAL_ADDR;
    mr.size = MOCK_SIZE;
    mr.flags = REG_MR_FLAG_HBM;
    ASSERT_EQ(mgr->RegisterMemoryRegion(mr), BM_OK);

    // Store a mock key to verify HCommand export path
    auto &registration = mgr->localRegistrations_[MOCK_LOCAL_ADDR];
    ASSERT_NE(registration.handle, nullptr);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, SynchronizeRankNoState)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    // rank not found in remoteRanks_ → should return BM_NOT_CONNECTED
    auto ret = mgr->Synchronize(99);
    ASSERT_EQ(ret, BM_NOT_CONNECTED);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, RemoteIoOnlyWithSizeZero)
{
    auto mgr = CreateHcommMgr();
    // size=0 on async (no Synchronize) → RemoteIo returns BM_OK immediately
    ASSERT_EQ(mgr->ReadRemoteAsync(1, 0x1000, 0x2000, 0), BM_OK);
    ASSERT_EQ(mgr->WriteRemoteAsync(1, 0x1000, 0x2000, 0), BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, OpenDeviceAclrtGetVersionFails)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };
    // AclrtGetVersion returns error → OpenDevice should still succeed (native fallback)
    DlAclApi::pAclrtGetVersion = [](int32_t *, int32_t *, int32_t *) -> int32_t { return -1; };

    auto mgr = CreateHcommMgr();
    auto opts = MakeOptions();
    opts.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    auto ret = mgr->OpenDevice(opts);
    ASSERT_EQ(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, CloseDeviceTwice)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);
    ASSERT_EQ(mgr->CloseDevice(), BM_OK);
    // Close again → should be no-op
    ASSERT_EQ(mgr->CloseDevice(), BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, OpenDeviceNullProtocol)
{
    auto mgr = CreateHcommMgr();
    auto opts = MakeOptions();
    opts.protocol = 0;
    auto ret = mgr->OpenDevice(opts);
    ASSERT_EQ(ret, BM_OK);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, ReadWrite_NotConnected)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);
    constexpr uint64_t SZ = 1024;

    // Not connected → all remote ops return BM_NOT_CONNECTED
    ASSERT_EQ(mgr->ReadRemote(1, 0x1000, 0x2000, SZ), BM_NOT_CONNECTED);
    ASSERT_EQ(mgr->WriteRemote(1, 0x1000, 0x2000, SZ), BM_NOT_CONNECTED);
    ASSERT_EQ(mgr->ReadRemoteAsync(1, 0x1000, 0x2000, SZ), BM_NOT_CONNECTED);
    ASSERT_EQ(mgr->WriteRemoteAsync(1, 0x1000, 0x2000, SZ), BM_NOT_CONNECTED);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, QueryHasRegisteredEmpty)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };

    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    // No registrations → should return false
    bool hasReg = mgr->QueryHasRegistered(0x1000, 0x100);
    ASSERT_FALSE(hasReg);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, UpdateMemoryKey)
{
    auto mgr = CreateHcommMgr();
    TransportMemoryKey key{};
    void *newAddr = reinterpret_cast<void *>(0x12345678UL);
    mgr->UpdateMemoryKey(key, newAddr);
    // UpdateMemoryKey writes to HcommKeyMetaAt().addr which is at TRANS_KEY_DEV_SLOT2
    auto &meta = HcommKeyMetaAt(key);
    ASSERT_EQ(meta.addr, 0x12345678UL);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, UpdateMemoryKeyNullAddr)
{
    auto mgr = CreateHcommMgr();
    TransportMemoryKey key{};
    mgr->UpdateMemoryKey(key, nullptr);
    // null addr → no change
    ASSERT_EQ(HcommKeyMetaAt(key).addr, 0UL);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, GetPrivateDataAfterOpen)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    auto mgr = CreateHcommMgr();
    ASSERT_EQ(mgr->OpenDevice(MakeOptions()), BM_OK);

    auto priv = mgr->GetPrivateData();
    auto *parsed = reinterpret_cast<const RdmaHcommPrivateData *>(priv.key.keys);
    ASSERT_EQ(parsed->magic, RDMA_HCOMM_PRIVATE_DATA_MAGIC);
    ASSERT_EQ(parsed->version, RDMA_HCOMM_PRIVATE_DATA_VERSION);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, OpenDeviceTransferFlagBuffer)
{
    DlHcommApi::gHcommEndpointCreate = [](const EndpointDesc *, EndpointHandle *handle) -> int32_t {
        *handle = MOCK_ENDPOINT;
        return 0;
    };
    DlHcommApi::gHcommEndpointDestroy = [](EndpointHandle) -> int32_t { return 0; };
    DlHcommApi::gHcommMemReg = [](EndpointHandle, const char *, const HcommCommMem *,
                                  HcommMemHandle *handle) -> int32_t {
        *handle = reinterpret_cast<HcommMemHandle>(0xB002UL);
        return 0;
    };

    auto mgr = CreateHcommMgr();
    // OpenDevice with transfer flag buffer registration
    auto ret = mgr->OpenDevice(MakeOptions());
    ASSERT_EQ(ret, BM_OK);
    // openGeneration_ should be valid after OpenDevice success
    ASSERT_NE(mgr->openGeneration_, nullptr);
}

TEST_F(DeviceRdmaHcommTransportManagerTest, ConvertGvaToDva_KnownAddr)
{
    auto mgr = CreateHcommMgr();
    // Unknown address → returns input address unchanged
    uint64_t result = mgr->ConvertGvaToDva(0xABCD0000UL);
    ASSERT_EQ(result, 0xABCD0000UL);
}
