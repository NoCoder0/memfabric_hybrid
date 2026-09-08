// ubs_dual_service_check.cpp
// 最小验证工具：同一进程能否创建 2 个 ubs Hcom service，各自 ServiceSetDeviceIpMask +
// ServiceBind 到不同 RDMA 卡并 ServiceStart 成功；conn 角色再对两个远端 URL 各 ServiceConnect。
// 用法（86/87 各一张双卡机）：
//   编译: g++ -std=c++17 -O2 -o ubs_dual_service_check ubs_dual_service_check.cpp -ldl \
//            -I <memfabric_hybrid>/src/hybm/csrc/under_api
//   运行(需 LD_LIBRARY_PATH 含 libhcom.so):
//   87(listen): ./ubs_dual_service_check --listen --ip1 192.168.75.87 --ip2 192.168.65.87 \
//               --p1 18587 --p2 18687
//   86(conn)  : ./ubs_dual_service_check --conn --ip1 192.168.75.86 --ip2 192.168.65.86 \
//               --p1 18586 --p2 18686 \
//               --peer1 tcp://192.168.75.87:18587 --peer2 tcp://192.168.65.87:18687
// 判据: svc0/svc1 两次 create+bind+start 均成功 => 同进程多 service 多卡可行；
//       conn 端两路 ServiceConnect 均 ret==0 => 双链路可建。
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// 复用仓库纯类型头（无 MF 依赖）
#include "hcom_service_c_define.h"

namespace {

// ---- 与 libhcom.so 实际导出符号一一对应（MF dl_hcom_api 同款）----
typedef int (*CreateFn)(Service_Type, const char *, Service_Options, Hcom_Service *);
typedef int (*BindFn)(Hcom_Service, const char *, Service_ChannelHandler);
typedef int (*StartFn)(Hcom_Service);
typedef int (*DestroyFn)(Hcom_Service, const char *);
typedef int (*ConnectFn)(Hcom_Service, const char *, Hcom_Channel *, Service_ConnectOptions);
typedef int (*DisConnectFn)(Hcom_Service, Hcom_Channel);
typedef void (*IpMaskFn)(Hcom_Service, const char *);
typedef void (*BrokerFn)(Hcom_Service, Service_ChannelHandler, Service_ChannelPolicy, uint64_t);

CreateFn gCreate = nullptr;
BindFn gBind = nullptr;
StartFn gStart = nullptr;
DestroyFn gDestroy = nullptr;
ConnectFn gConnect = nullptr;
DisConnectFn gDisConnect = nullptr;
IpMaskFn gSetIpMask = nullptr;
BrokerFn gRegBroker = nullptr;

void *gHandle = nullptr;

bool LoadLib()
{
    gHandle = dlopen("libhcom.so", RTLD_NOW | RTLD_NODELETE);
    if (gHandle == nullptr) {
        printf("FAIL: dlopen libhcom.so: %s\n", dlerror());
        return false;
    }
#define LOAD(fn, name, type)                                                                                           \
    do {                                                                                                               \
        *(void **)(&fn) = dlsym(gHandle, name);                                                                        \
        if (fn == nullptr) {                                                                                           \
            printf("FAIL: dlsym %s: %s\n", name, dlerror());                                                           \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)
    LOAD(gCreate, "ubs_hcom_service_create", CreateFn);
    LOAD(gBind, "ubs_hcom_service_bind", BindFn);
    LOAD(gStart, "ubs_hcom_service_start", StartFn);
    LOAD(gDestroy, "ubs_hcom_service_destroy", DestroyFn);
    LOAD(gConnect, "ubs_hcom_service_connect", ConnectFn);
    LOAD(gDisConnect, "ubs_hcom_service_disconnect", DisConnectFn);
    LOAD(gSetIpMask, "ubs_hcom_service_set_ipmask", IpMaskFn);
    LOAD(gRegBroker, "ubs_hcom_service_register_broken_handler", BrokerFn);
    return true;
#undef LOAD
}

int NewEndPointHandler(Hcom_Channel ch, uint64_t usrCtx, const char *payLoad)
{
    printf("[cb] new channel ch=%p ctx=%lu payload=%s\n", (void *)ch, usrCtx, payLoad ? payLoad : "<null>");
    return 0;
}

int BrokenHandler(Hcom_Channel ch, uint64_t usrCtx, const char *payLoad)
{
    printf("[cb] channel broken ch=%p ctx=%lu payload=%s\n", (void *)ch, usrCtx, payLoad ? payLoad : "<null>");
    return 0;
}

// 创建第 idx 个 service：bind 到本机 ip，listen url = tcp://ip:port
bool CreateService(int idx, const std::string &name, const std::string &ip, uint32_t port, Hcom_Service &svc,
                   std::string &url)
{
    Service_Options opt{};
    opt.maxSendRecvDataSize = 1024 * 1024; // 1MB，贴近 MF NO_XPU 配置
    opt.workerGroupMode = C_SERVICE_BUSY_POLLING;
    opt.workerThreadPriority = -20;

    int ret = gCreate(C_SERVICE_RDMA, name.c_str(), opt, &svc);
    printf("svc[%d] %s: create ret=%d\n", idx, name.c_str(), ret);
    if (ret != 0) {
        return false;
    }
    std::string ipMask = ip + "/32";
    gSetIpMask(svc, ipMask.c_str()); // void
    url = "tcp://" + ip + ":" + std::to_string(port);
    ret = gBind(svc, url.c_str(), NewEndPointHandler);
    printf("svc[%d] %s: bind %s ret=%d\n", idx, name.c_str(), url.c_str(), ret);
    if (ret != 0) {
        return false;
    }
    gRegBroker(svc, BrokenHandler, C_CHANNEL_RECONNECT, 1); // void
    ret = gStart(svc);
    printf("svc[%d] %s: start ret=%d\n", idx, name.c_str(), ret);
    return ret == 0;
}

} // namespace

int main(int argc, char **argv)
{
    bool listenMode = false;
    bool connMode = false;
    std::string ip1, ip2, peer1, peer2;
    uint32_t p1 = 18586;
    uint32_t p2 = 18686;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char *msg) -> std::string {
            if (i + 1 >= argc) {
                printf("FAIL: missing value for %s\n", msg);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "--listen") {
            listenMode = true;
        } else if (a == "--conn") {
            connMode = true;
        } else if (a == "--ip1") {
            ip1 = next("ip1");
        } else if (a == "--ip2") {
            ip2 = next("ip2");
        } else if (a == "--p1") {
            p1 = (uint32_t)atoi(next("p1").c_str());
        } else if (a == "--p2") {
            p2 = (uint32_t)atoi(next("p2").c_str());
        } else if (a == "--peer1") {
            peer1 = next("peer1");
        } else if (a == "--peer2") {
            peer2 = next("peer2");
        }
    }
    if (ip1.empty() || ip2.empty() || (!listenMode && !connMode)) {
        printf("usage: %s --listen|--conn --ip1 <card1ip> --ip2 <card2ip> [--p1 port1 --p2 port2]\n", argv[0]);
        printf("       conn additionally: --peer1 tcp://host1:p1 --peer2 tcp://host2:p2\n");
        return 1;
    }
    if (!LoadLib()) {
        return 1;
    }

    // 关键验证 1：同进程创建两个 service，各自 bind/start 不同网卡
    Hcom_Service svcA = 0;
    Hcom_Service svcB = 0;
    std::string urlA, urlB;
    bool okA = CreateService(0, "svc_card1", ip1, p1, svcA, urlA);
    bool okB = CreateService(1, "svc_card2", ip2, p2, svcB, urlB);
    if (okA && okB) {
        printf("RESULT: dual-service-create PASS (two services bound to card1=%s and card2=%s)\n", urlA.c_str(),
               urlB.c_str());
    } else {
        printf("RESULT: dual-service-create FAIL (svcA=%d svcB=%d)\n", okA, okB);
    }

    if (connMode) {
        if (peer1.empty() || peer2.empty()) {
            printf("FAIL: conn mode needs --peer1 --peer2\n");
            return 1;
        }
        sleep(1); // 等对端 listen 就绪
        // 关键验证 2：每路 service(每张卡) 各自连对端对应 url
        Service_ConnectOptions opt{};
        opt.mode = C_CLIENT_WORKER_POLL;
        opt.clientGroupId = 0;
        opt.serverGroupId = 0;
        opt.linkCount = 1;
        Hcom_Channel chA = 0;
        Hcom_Channel chB = 0;
        int retA = gConnect(svcA, peer1.c_str(), &chA, opt);
        int retB = gConnect(svcB, peer2.c_str(), &chB, opt);
        printf("connect svc0->%s ret=%d ch=%p\n", peer1.c_str(), retA, (void *)chA);
        printf("connect svc1->%s ret=%d ch=%p\n", peer2.c_str(), retB, (void *)chB);
        if (retA == 0 && retB == 0) {
            printf("RESULT: dual-link-connect PASS (both cards connected)\n");
            sleep(3);
            if (chA != 0) {
                gDisConnect(svcA, chA);
            }
            if (chB != 0) {
                gDisConnect(svcB, chB);
            }
        } else {
            printf("RESULT: dual-link-connect FAIL (retA=%d retB=%d)\n", retA, retB);
        }
    } else {
        printf("listen mode: waiting for incoming connects (press Ctrl-C to exit)...\n");
        sleep(60);
    }

    if (svcA != 0) {
        gDestroy(svcA, "svc_card1");
    }
    if (svcB != 0) {
        gDestroy(svcB, "svc_card2");
    }
    dlclose(gHandle);
    return 0;
}
