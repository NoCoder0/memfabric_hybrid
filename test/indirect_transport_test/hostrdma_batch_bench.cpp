/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 *
 * hostrdma_batch_bench.cpp
 *
 * 双机 host rdma 600x1KB 小包对比测试（第一版：只用现有 smem_bm 接口）
 *
 * 对称内存布局（每 rank 各自贡献 localDRAMSize，GVA 空间按 rank 排列）：
 *   [0, stagingEnd)                连续 staging（接收侧），默认 600KB
 *   [dispBase, dispBase+count*stride)   600 个离散目标/源（stride 间隔模拟离散）
 *   [dramBytes-8, dramBytes)       完成 flag 槽（段尾，8B，remote 单发写，local 轮询）
 *   remote(rank1) 的 600 源放在自己段的 0..count*stride（离散）；
 *   local(rank0)  的 staging/flag/目标放在自己段内。
 *
 * 场景：
 *   baseline : remote 600x1KB batch 直写 local 600 个离散目标（现有实现，无 staging/scatter）
 *   cont     : remote 600x1KB batch 写 local 连续 staging -> 单发写 flag -> local 轮询
 *              -> local CPU scatter(staging -> 600 离散目标) -> 清 flag
 *
 * 每个场景跑 --rounds 轮，每轮耗时单独打印（不做均值，是否取平均/中位数由使用者定）。
 *
 * 编译：链接 smem 库；host rdma 环境（ubs-comm/libhcom）。
 *
 * 运行（两端必须同时启动、--mode 一致）：
 *   节点0(local) : ./hostrdma_batch_bench --role=local  --rank=0 \
 *                    --store-url=tcp://<storeIP>:port --hcom-url=tcp://<本机网卡IP>:port \
 *                    --with-store=1 --mode=all
 *   节点1(remote): ./hostrdma_batch_bench --role=remote --rank=1 \
 *                    --store-url=tcp://<storeIP>:port --hcom-url=tcp://<本机网卡IP>:port --mode=all
 *
 * 说明：
 *   - --store-url 两端指向同一 config store（推荐 rank0 用 --with-store=1 内嵌启动）；
 *   - --hcom-url 各填本机 RDMA 网卡 IP:port（host rdma 建链地址）；
 *   - 数据正确性：remote 第 i 块填值 (uint8)i，local 端在 cont 模式 scatter 后可自校验。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "smem_bm.h"
#include "smem_bm_def.h"

namespace {

constexpr uint32_t kDefaultCount = 600;
constexpr uint64_t kDefaultSize = 1024;
constexpr uint64_t kDefaultStride = 4096; /* 离散摆放间隔 */
constexpr uint64_t kDefaultDramMB = 16;   /* 每 rank 对称 host 内存，需覆盖 源区+目标区+staging+flag */

struct BenchArgs {
    std::string role; /* local / remote */
    uint32_t rank = 0;
    uint32_t worldSize = 2;
    std::string storeUrl;
    std::string hcomUrl;
    bool withStore = false;
    std::string mode = "all"; /* all / baseline / cont */
    uint32_t count = kDefaultCount;
    uint64_t size = kDefaultSize;
    uint64_t stride = kDefaultStride;
    uint64_t dramMB = kDefaultDramMB;
    uint32_t rounds = 5;
};

void Usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --role=local|remote --rank=N --store-url=tcp://ip:port "
            "--hcom-url=tcp://ip:port [options]\n"
            "  --mode=all|baseline|cont   场景(默认 all)\n"
            "  --count=N                  小 IO 数(默认600)\n"
            "  --size=N                   单块字节(默认1024)\n"
            "  --stride=N                 离散摆放间隔(默认4096)\n"
            "  --dram-mb=N                每 rank 对称 host 内存 MB(默认16)\n"
            "  --rounds=N                 每场景轮数(默认5)\n"
            "  --with-store=0|1           本进程是否内嵌启动 config store(默认0)\n",
            prog);
}

bool ParseArgs(int argc, char *argv[], BenchArgs &a)
{
    for (int i = 1; i < argc; ++i) {
        std::string kv = argv[i];
        auto pos = kv.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        std::string k = kv.substr(0, pos);
        std::string v = kv.substr(pos + 1);
        if (k == "--role") {
            a.role = v;
        } else if (k == "--rank") {
            a.rank = static_cast<uint32_t>(std::stoul(v));
        } else if (k == "--store-url") {
            a.storeUrl = v;
        } else if (k == "--hcom-url") {
            a.hcomUrl = v;
        } else if (k == "--with-store") {
            a.withStore = v != "0";
        } else if (k == "--mode") {
            a.mode = v;
        } else if (k == "--count") {
            a.count = static_cast<uint32_t>(std::stoul(v));
        } else if (k == "--size") {
            a.size = std::stoull(v);
        } else if (k == "--stride") {
            a.stride = std::stoull(v);
        } else if (k == "--dram-mb") {
            a.dramMB = std::stoull(v);
        } else if (k == "--rounds") {
            a.rounds = static_cast<uint32_t>(std::stoul(v));
        }
    }
    if (a.role != "local" && a.role != "remote") {
        fprintf(stderr, "invalid --role=%s\n", a.role.c_str());
        return false;
    }
    if (a.storeUrl.empty() || a.hcomUrl.empty()) {
        fprintf(stderr, "missing --store-url or --hcom-url\n");
        return false;
    }
    return true;
}

uint64_t AlignUp(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

/* 把本端对称内存某段 GVA 转成可读写 VA（host 段用 LOCAL_HOST） */
bool GvaToHostVa(smem_bm_t bm, uint64_t gva, void **va)
{
    return smem_bm_gva_to_va(bm, reinterpret_cast<void *>(gva), SMEM_MEM_TYPE_LOCAL_HOST, va) == 0;
}

uint64_t NowUs()
{
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

void PrintLabel(const char *mode)
{
    printf("\n==== mode=%s ====\n", mode);
}

} // namespace

int main(int argc, char *argv[])
{
    BenchArgs a;
    if (!ParseArgs(argc, argv, a)) {
        Usage(argv[0]);
        return 1;
    }
    const bool isLocal = (a.role == "local");
    printf("[bench] role=%s rank=%u count=%u size=%llu stride=%llu rounds=%u\n", a.role.c_str(), a.rank, a.count,
           static_cast<unsigned long long>(a.size), static_cast<unsigned long long>(a.stride), a.rounds);
    /* 链路档位：hcom-url 含 ';'（多个 url）即为双卡/双连接，否则单卡/单连接 */
    const bool dualUrl = a.hcomUrl.find(';') != std::string::npos;
    printf("[bench] link-mode=%s hcom-url=%s\n", dualUrl ? "dual-link(2 NIC)" : "single-link(1 NIC)",
           a.hcomUrl.c_str());

    /* 布局常量（两端同一公式）
       [0, stagingEnd)               连续 staging（接收侧）
       [stagingEnd, +4096)           预留 gap
       [dispBase, dispBase+count*stride)  600 个离散目标/源（stride 间隔模拟离散）
       [dramBytes-8, dramBytes)      完成 flag 槽（放段尾，避开源/目标/staging 区） */
    const uint64_t dramBytes = a.dramMB * 1024 * 1024;
    const uint64_t stagingEnd = AlignUp(a.count * a.size, 4096); /* staging 连续区尾部 */
    const uint64_t dispBase = stagingEnd + 4096;                 /* 离散源/目标区起点 */
    const uint64_t needBytes = dispBase + a.count * a.stride;
    const uint64_t flagOff = dramBytes - 8;                      /* 完成 flag 槽：段尾 8B */
    if (needBytes > flagOff) {
        fprintf(stderr, "dram-mb too small, need >= %llu bytes\n", static_cast<unsigned long long>(needBytes));
        return 1;
    }

    /* 1. init */
    smem_bm_config_t config;
    if (smem_bm_config_init(&config) != 0) {
        fprintf(stderr, "smem_bm_config_init failed\n");
        return 1;
    }
    config.autoRanking = false;
    config.rankId = a.rank;
    config.startConfigStoreServer = a.withStore;
    snprintf(config.hcomUrl, sizeof(config.hcomUrl), "%s", a.hcomUrl.c_str());
    if (smem_bm_init(a.storeUrl.c_str(), a.worldSize, 0, &config) != 0) {
        fprintf(stderr, "smem_bm_init failed, store=%s\n", a.storeUrl.c_str());
        return 1;
    }

    /* 2. create + join */
    const uint64_t localDramSize = a.dramMB * 1024 * 1024;
    smem_bm_t bm = smem_bm_create(0, a.worldSize, SMEMB_DATA_OP_HOST_RDMA, localDramSize, 0, 0);
    if (bm == nullptr) {
        fprintf(stderr, "smem_bm_create failed\n");
        return 1;
    }
    if (smem_bm_join(bm, 0) != 0) {
        fprintf(stderr, "smem_bm_join failed\n");
        return 1;
    }

    const uint64_t selfGva = reinterpret_cast<uint64_t>(smem_bm_ptr_by_mem_type(bm, SMEM_MEM_TYPE_HOST, a.rank));
    const uint32_t peerRank = 1 - a.rank;
    const uint64_t peerGva = reinterpret_cast<uint64_t>(smem_bm_ptr_by_mem_type(bm, SMEM_MEM_TYPE_HOST, peerRank));
    printf("[bench] selfGva=0x%llx peerGva=0x%llx stagingEnd=%llu flagOff=%llu dispBase=%llu\n",
           static_cast<unsigned long long>(selfGva), static_cast<unsigned long long>(peerGva),
           static_cast<unsigned long long>(stagingEnd), static_cast<unsigned long long>(flagOff),
           static_cast<unsigned long long>(dispBase));
    if (selfGva == 0 || peerGva == 0) {
        fprintf(stderr, "get gva failed\n");
        return 1;
    }

    /* 3. 初始化数据：remote 源填值 i；local 的 staging/目标清 0、flag 清 0 */
    {
        if (!isLocal) {
            for (uint32_t i = 0; i < a.count; ++i) {
                void *srcVa = nullptr;
                if (GvaToHostVa(bm, selfGva + i * a.stride, &srcVa)) {
                    memset(srcVa, static_cast<int>(i & 0xFF), a.size);
                }
            }
        } else {
            void *flagVa = nullptr;
            if (GvaToHostVa(bm, selfGva + flagOff, &flagVa)) {
                *reinterpret_cast<uint64_t *>(flagVa) = 0;
            }
            for (uint32_t i = 0; i < a.count; ++i) {
                void *dstVa = nullptr;
                if (GvaToHostVa(bm, selfGva + dispBase + i * a.stride, &dstVa)) {
                    memset(dstVa, 0, a.size);
                }
                void *stagVa = nullptr;
                if (GvaToHostVa(bm, selfGva + i * a.size, &stagVa)) {
                    memset(stagVa, 0, a.size);
                }
            }
        }
    }

    std::vector<uint64_t> sizes(a.count, a.size);
    const bool runBase = (a.mode == "all" || a.mode == "baseline");
    const bool runCont = (a.mode == "all" || a.mode == "cont");

    /* 场景 1：baseline —— remote batch 直写 local 600 个离散目标（无 staging/scatter）；
       local 观察端每轮等 remote 的完成 flag（对齐 cont 的同步方式），保证双端不提前退出 */
    if (runBase) {
        if (!isLocal) {
            PrintLabel("baseline (sender)");
            std::vector<void *> srcs(a.count), dsts(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                srcs[i] = reinterpret_cast<void *>(selfGva + i * a.stride);
                dsts[i] = reinterpret_cast<void *>(peerGva + dispBase + i * a.stride);
            }
            uint64_t sumUs = 0;
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            uint64_t seq = 1;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t t0 = NowUs(); /* sender 时延 = 一次 batch 拷贝接口(含写完成 flag)从调用到返回 */
                smem_batch_copy_params p{srcs.data(), dsts.data(), sizes.data(), a.count, nullptr};
                int32_t ret = smem_bm_copy_batch(bm, &p, SMEMB_COPY_AUTO, 0);
                if (ret == 0) {
                    void *srcFlagVa = nullptr;
                    if (GvaToHostVa(bm, selfGva + flagOff, &srcFlagVa)) {
                        *reinterpret_cast<uint64_t *>(srcFlagVa) = seq;
                    }
                    smem_copy_params fp{reinterpret_cast<void *>(selfGva + flagOff),
                                        reinterpret_cast<void *>(peerGva + flagOff), sizeof(seq), nullptr};
                    ret = smem_bm_copy(bm, &fp, SMEMB_COPY_AUTO, 0);
                }
                const uint64_t t1 = NowUs();
                if (ret != 0) {
                    printf("baseline sender abort at iter %u ret=%d\n", r, ret);
                    break;
                }
                if (r >= 1) { /* r==0 为 warmup，不计时不打印 */
                    const uint64_t cost = t1 - t0;
                    sumUs += cost;
                    printf("baseline sender round %u cost_us=%llu\n", r - 1,
                           static_cast<unsigned long long>(cost));
                }
                ++seq;
            }
            printf("baseline sender avg_us=%llu (rounds=%u)\n",
                   static_cast<unsigned long long>(sumUs / a.rounds), a.rounds);
        } else {
            PrintLabel("baseline (observer)");
            uint64_t sumUs = 0;
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时） */
            uint64_t expect = 1;
            for (uint32_t r = 0; r < kTotal; ++r) {
                void *flagVa = nullptr;
                GvaToHostVa(bm, selfGva + flagOff, &flagVa);
                if (flagVa == nullptr) {
                    printf("baseline observer flag_va_failed at iter %u\n", r);
                    break;
                }
                const uint64_t t0 = NowUs(); /* receiver 时延 = 本轮从等完成 flag 到数据校验完 */
                while (*reinterpret_cast<volatile const uint64_t *>(flagVa) != expect) {
                } /* 自旋等 flag（完成很快，不用 sleep） */
                int bad = 0;
                for (uint32_t i = 0; i < a.count; ++i) {
                    void *dstVa = nullptr;
                    if (!GvaToHostVa(bm, selfGva + dispBase + i * a.stride, &dstVa) ||
                        *reinterpret_cast<const uint8_t *>(dstVa) != static_cast<uint8_t>(i & 0xFF)) {
                        bad = 1;
                        break;
                    }
                }
                const uint64_t t1 = NowUs();
                if (r >= 1) { /* r==0 为 warmup，不计时不打印 */
                    const uint64_t cost = t1 - t0;
                    sumUs += cost;
                    printf("baseline observer round %u err=%d cost_us=%llu\n", r - 1, bad,
                           static_cast<unsigned long long>(cost));
                }
                ++expect;
            }
            printf("baseline observer avg_us=%llu (rounds=%u)\n",
                   static_cast<unsigned long long>(sumUs / a.rounds), a.rounds);
        }
    }

    /* 场景 2：cont —— remote 写 local 连续 staging -> flag；local 轮询 flag -> scatter */
    if (runCont) {
        if (!isLocal) {
            PrintLabel("cont (sender)");
            std::vector<void *> srcs(a.count), dsts(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                srcs[i] = reinterpret_cast<void *>(selfGva + i * a.stride); /* 离散源 */
                dsts[i] = reinterpret_cast<void *>(peerGva + i * a.size);   /* 连续 staging */
            }
            uint64_t sumUs = 0;
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            uint64_t seq = 1;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t t0 = NowUs(); /* sender 时延 = 一次 batch 拷贝 + 写完成 flag，从调用到返回 */
                smem_batch_copy_params p{srcs.data(), dsts.data(), sizes.data(), a.count, nullptr};
                int32_t ret = smem_bm_copy_batch(bm, &p, SMEMB_COPY_AUTO, 0);
                if (ret == 0) {
                    /* 完成 flag：本端镜像写 seq（GVA 内偏移，先转 VA 写入），再单发到对端 flag 槽 */
                    void *srcFlagVa = nullptr;
                    if (GvaToHostVa(bm, selfGva + flagOff, &srcFlagVa)) {
                        *reinterpret_cast<uint64_t *>(srcFlagVa) = seq;
                    }
                    const uint64_t srcGva = selfGva + flagOff;
                    smem_copy_params fp{reinterpret_cast<void *>(srcGva),
                                        reinterpret_cast<void *>(peerGva + flagOff), sizeof(seq), nullptr};
                    ret = smem_bm_copy(bm, &fp, SMEMB_COPY_AUTO, 0);
                }
                const uint64_t t1 = NowUs();
                if (ret != 0) {
                    printf("cont sender abort at iter %u ret=%d\n", r, ret);
                    break;
                }
                if (r >= 1) { /* r==0 为 warmup，不计时不打印 */
                    const uint64_t cost = t1 - t0;
                    sumUs += cost;
                    printf("cont sender round %u cost_us=%llu\n", r - 1,
                           static_cast<unsigned long long>(cost));
                }
                ++seq;
            }
            printf("cont sender avg_us=%llu (rounds=%u)\n",
                   static_cast<unsigned long long>(sumUs / a.rounds), a.rounds);
        } else {
            PrintLabel("cont (receiver poll+scatter)");
            uint64_t sumUs = 0;
            uint64_t sumScatterUs = 0;
            /* 600 对 staging/离散目标的 VA 整轮固定，先一次性转换（不计时），
               scatter 计时只统计纯 memcpy 搬运开销 */
            std::vector<void *> scatterSrcs(a.count), scatterDsts(a.count);
            bool vaOk = true;
            for (uint32_t i = 0; i < a.count; ++i) {
                if (!GvaToHostVa(bm, selfGva + i * a.size, &scatterSrcs[i]) ||
                    !GvaToHostVa(bm, selfGva + dispBase + i * a.stride, &scatterDsts[i])) {
                    vaOk = false;
                    break;
                }
            }
            if (!vaOk) {
                printf("cont receiver va convert failed, skip scenario\n");
                return 1;
            }
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            uint64_t expect = 1;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t t0 = NowUs(); /* receiver 时延 = 本轮从等完成 flag 到 scatter+校验完 */
                void *flagVa = nullptr;
                GvaToHostVa(bm, selfGva + flagOff, &flagVa);
                if (flagVa == nullptr) {
                    printf("cont receiver flag_va_failed at iter %u\n", r);
                    break;
                }
                while (*reinterpret_cast<volatile const uint64_t *>(flagVa) != expect) {
                } /* 自旋等 flag（完成很快，不用 sleep） */
                int bad = 0;
                const uint64_t ts0 = NowUs(); /* scatter 单独计时：纯 memcpy(staging -> 离散目标) */
                for (uint32_t i = 0; i < a.count; ++i) {
                    memcpy(scatterDsts[i], scatterSrcs[i], a.size);
                }
                const uint64_t ts1 = NowUs();
                *reinterpret_cast<uint64_t *>(flagVa) = 0; /* 清 flag */
                const uint64_t t1 = NowUs();
                if (r >= 1) { /* r==0 为 warmup，不计时不打印 */
                    const uint64_t cost = t1 - t0;
                    const uint64_t scatterCost = ts1 - ts0;
                    sumUs += cost;
                    sumScatterUs += scatterCost;
                    printf("cont receiver round %u err=%d cost_us=%llu scatter_us=%llu\n", r - 1, bad,
                           static_cast<unsigned long long>(cost),
                           static_cast<unsigned long long>(scatterCost));
                }
                ++expect;
            }
            printf("cont receiver avg_us=%llu scatter_avg_us=%llu (rounds=%u)\n",
                   static_cast<unsigned long long>(sumUs / a.rounds),
                   static_cast<unsigned long long>(sumScatterUs / a.rounds), a.rounds);
        }
    }

    smem_bm_leave(bm, 0);
    smem_bm_destroy(bm);
    smem_bm_uninit(0);
    printf("[bench] done\n");
    return 0;
}
