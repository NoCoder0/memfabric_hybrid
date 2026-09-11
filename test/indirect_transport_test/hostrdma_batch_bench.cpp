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
 *   - 数据正确性：remote 第 i 块由 FillBlock 填充（前 4B = 块号+1 小端，其余 = (i+1)&0xFF）；
 *       local 端在每轮数据到达（拷贝完成）之后、计时区之外做整块校验（先验身份 tag 再 memcmp
 *       内容），能识别漏写 / 错位 / 字节错；校验不入计时区间，故不影响任何耗时数字。
 */

#include <algorithm>
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
    bool old = false; /* --old=1 老版口径(如 e30bf29，无收端聚合): 只跑直写 baseline、只用单 url、无 ack/scatter */
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
            "  --old=1                    老版口径(默认0，跑 e30bf29 等无聚合 MF 用):\n"
            "                             只跑直写 baseline、只用单 url、无 staging/scatter/ack\n"
            "                             不传 --old = 最新版: baseline 直写 + cont(收端聚合, 带 ack)\n"
            "  --with-store=0|1           本进程是否内嵌启动 config store(默认0)\n",
            prog);
}

bool ParseArgs(int argc, char *argv[], BenchArgs &a)
{
    for (int i = 1; i < argc; ++i) {
        std::string kv = argv[i];
        if (kv == "--old") {
            a.old = true;
            continue;
        }
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
        } else if (k == "--old") {
            a.old = v != "0";
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

/* 每块数据布局（remote 填充 / local 校验共用同一规则）：
     前 4 字节 = 块号+1 的小端（唯一身份标识，+1 是为了避开初始化值 0）
     其余字节 = (块号+1) & 0xFF
   这样校验既能验"内容对不对"，也能验"是不是这一块"（识别漏写 / 错位）。 */
constexpr uint32_t kBlockIdBytes = 4;

uint32_t BlockTag(uint32_t i)
{
    return i + 1U;
}

/* 块内填充字节的期望值（计时区内的轻量首字节检查也复用它） */
uint8_t BlockFillByte(uint32_t i)
{
    return static_cast<uint8_t>(BlockTag(i) & 0xFF);
}

/* 按上述规则填充第 i 块 */
void FillBlock(void *va, uint32_t i, uint64_t size)
{
    auto *p = static_cast<uint8_t *>(va);
    const uint32_t tag = BlockTag(i);
    if (size >= kBlockIdBytes) {
        p[0] = static_cast<uint8_t>(tag & 0xFF);
        p[1] = static_cast<uint8_t>((tag >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((tag >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((tag >> 24) & 0xFF);
        memset(p + kBlockIdBytes, static_cast<int>(BlockFillByte(i)), size - kBlockIdBytes);
    } else {
        memset(p, static_cast<int>(BlockFillByte(i)), size);
    }
}

/* 数据校验结果：ok=false 时记录首个不一致位置，便于定位 */
struct VerifyResult {
    bool ok = true;
    bool vaFailed = false;
    bool idMismatch = false; /* 身份不符（漏写/错位），而非内容字节错 */
    uint32_t blockIdx = 0;
    uint32_t actualTag = 0; /* 身份不符时：该块实际携带的 tag（0 = 从未被写过） */
    uint64_t byteOff = 0;
    uint8_t expect = 0;
    uint8_t actual = 0;
};

/* 整块校验：先验身份（前 4B tag），再用 memcmp 比内容。
   vas      —— 该组块已解析好的本地 VA（调用方缓存复用，避免每轮重复 GVA 转换）；
   refBlock —— 256 个参考块，refBlock[v] 为整块填 v 的 size 字节。
   正常路径每块只做 1 次 tag 比较 + 1 次 memcmp（SIMD），逐字节扫描仅在失败路径上做。 */
VerifyResult VerifyBlocks(const std::vector<void *> &vas, uint32_t count, uint64_t size, const uint8_t *refBlock)
{
    VerifyResult r;
    const bool hasId = (size >= kBlockIdBytes);
    const uint64_t from = hasId ? kBlockIdBytes : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto *p = reinterpret_cast<const uint8_t *>(vas[i]);
        if (p == nullptr) {
            r.ok = false;
            r.vaFailed = true;
            r.blockIdx = i;
            return r;
        }

        if (hasId) {
            const uint32_t got = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                 (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            if (got != BlockTag(i)) {
                r.ok = false;
                r.idMismatch = true;
                r.blockIdx = i;
                r.actualTag = got;
                return r;
            }
        }

        const uint8_t want = BlockFillByte(i);
        if (memcmp(p + from, refBlock + static_cast<size_t>(want) * size + from, size - from) != 0) {
            r.ok = false;
            r.blockIdx = i;
            for (uint64_t b = from; b < size; ++b) { /* 仅失败路径逐字节定位 */
                if (p[b] != want) {
                    r.byteOff = b;
                    r.expect = want;
                    r.actual = p[b];
                    return r;
                }
            }
            return r;
        }
    }
    return r;
}

/* 打印首个校验失败的位置（只打一条，避免影响后续输出节奏） */
void PrintVerifyFail(const char *tag, uint32_t round, const VerifyResult &r)
{
    if (r.vaFailed) {
        printf("%s VERIFY FAIL round=%u block=%u (GVA->VA failed)\n", tag, round, r.blockIdx);
    } else if (r.idMismatch) {
        printf("%s VERIFY FAIL round=%u block=%u: identity tag=0x%08x expected=0x%08x (%s)\n", tag, round, r.blockIdx,
               r.actualTag, BlockTag(r.blockIdx),
               r.actualTag == 0 ? "block never written (still initial 0)" : "holds another block's data");
    } else {
        printf("%s VERIFY FAIL round=%u block=%u byte=%llu expect=0x%02x actual=0x%02x\n", tag, round, r.blockIdx,
               static_cast<unsigned long long>(r.byteOff), static_cast<unsigned>(r.expect),
               static_cast<unsigned>(r.actual));
    }
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
    /* --old：老 MF(无收端聚合) 直写口径 —— 强制单 url、只跑 baseline(直写)，无 staging/scatter/ack */
    if (a.old) {
        auto sep = a.hcomUrl.find(';');
        if (sep != std::string::npos) {
            a.hcomUrl = a.hcomUrl.substr(0, sep);
        }
        a.mode = "baseline";
        printf("[bench] old-mode=1: force direct-write baseline, single url, no staging/scatter/ack\n");
    }
    const bool isLocal = (a.role == "local");
    printf("[bench] role=%s rank=%u count=%u size=%llu stride=%llu rounds=%u mode=%s\n", a.role.c_str(), a.rank,
           a.count, static_cast<unsigned long long>(a.size), static_cast<unsigned long long>(a.stride), a.rounds,
           a.mode.c_str());
    /* 链路档位：hcom-url 含 ';'（多个 url）即为双卡/双连接，否则单卡/单连接 */
    const bool dualUrl = a.hcomUrl.find(';') != std::string::npos;
    printf("[bench] link-mode=%s hcom-url=%s\n", dualUrl ? "dual-link(2 NIC)" : "single-link(1 NIC)",
           a.hcomUrl.c_str());

    /* 布局常量（两端同一公式）
       [0, stagingEnd)               连续 staging（接收侧）
       [stagingEnd, +4096)           预留 gap
       [dispBase, dispBase+count*stride)  600 个离散目标/源（stride 间隔模拟离散）
       [dramBytes-16, dramBytes-8)     ack 槽（握手模式：remote 轮询，local 写回）
       [dramBytes-8, dramBytes)        完成 flag 槽（放段尾，避开源/目标/staging 区） */
    const uint64_t dramBytes = a.dramMB * 1024 * 1024;
    const uint64_t stagingEnd = AlignUp(a.count * a.size, 4096); /* staging 连续区尾部 */
    const uint64_t dispBase = stagingEnd + 4096;                 /* 离散源/目标区起点 */
    const uint64_t needBytes = dispBase + a.count * a.stride;
    const uint64_t ackOff = dramBytes - 16;                      /* ack 槽：段尾倒数第二 8B */
    const uint64_t flagOff = dramBytes - 8;                      /* 完成 flag 槽：段尾 8B */
    if (needBytes > ackOff) {
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

    /* 3. 初始化数据：remote 源填值 i；local 的 staging/目标清 0；双方 ack/flag 槽清 0 */
    {
        /* 双方各自的 ack/flag 槽清 0（握手模式 expect/seq 从 1 起，需先归零） */
        void *ackVa0 = nullptr;
        if (GvaToHostVa(bm, selfGva + ackOff, &ackVa0)) {
            *reinterpret_cast<uint64_t *>(ackVa0) = 0;
        }
        void *flagVa0 = nullptr;
        if (GvaToHostVa(bm, selfGva + flagOff, &flagVa0)) {
            *reinterpret_cast<uint64_t *>(flagVa0) = 0;
        }
        if (!isLocal) {
            for (uint32_t i = 0; i < a.count; ++i) {
                void *srcVa = nullptr;
                if (GvaToHostVa(bm, selfGva + i * a.stride, &srcVa)) {
                    FillBlock(srcVa, i, a.size); /* 前 4B 携带块号，其余填 (i+1)&0xFF */
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

    /* 校验用参考块：refBlock[v] 为整块填 v 的 a.size 字节。
       校验放在全部轮次跑完之后，用 memcmp（SIMD）比对，避免逐字节循环的额外开销。 */
    std::vector<uint8_t> refBlock(static_cast<size_t>(256) * a.size);
    for (uint32_t v = 0; v < 256; ++v) {
        memset(refBlock.data() + static_cast<size_t>(v) * a.size, static_cast<int>(v), a.size);
    }

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
            std::vector<uint64_t> costs; /* 热循环不打日志，跑完后统一打印，避免日志影响计时/节奏 */
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
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t cost = t1 - t0;
                    sumUs += cost;
                    costs.push_back(cost);
                }
                /* baseline 阶段为单向流：发方不等收方，保持原有轮周期与 cost_us 口径。
                   收端每轮只做轻量首字节检查 + 计时区外的整块校验，不会落后于发方周期 */
                ++seq;
            }
            for (uint32_t k = 0; k < costs.size(); ++k) {
                printf("baseline sender round %u cost_us=%llu\n", k,
                       static_cast<unsigned long long>(costs[k]));
            }
            printf("baseline sender avg_us=%llu (rounds=%u)\n",
                   static_cast<unsigned long long>(sumUs / a.rounds), a.rounds);
        } else {
            PrintLabel("baseline (observer)");
            uint64_t sumUs = 0;
            std::vector<uint64_t> costs; /* 热循环不打日志，跑完后统一打印 */
            std::vector<int> errs;
            std::vector<VerifyResult> verifies;
            /* 600 个目标地址固定，一次性解析后复用（轻量检查与整块校验共用） */
            std::vector<void *> dstVas(a.count, nullptr);
            for (uint32_t i = 0; i < a.count; ++i) {
                GvaToHostVa(bm, selfGva + dispBase + i * a.stride, &dstVas[i]);
            }
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时） */
            uint64_t expect = 1;
            for (uint32_t r = 0; r < kTotal; ++r) {
                void *flagVa = nullptr;
                GvaToHostVa(bm, selfGva + flagOff, &flagVa);
                if (flagVa == nullptr) {
                    printf("baseline observer flag_va_failed at iter %u\n", r);
                    break;
                }
                const uint64_t t0 = NowUs(); /* receiver 时延 = 本轮从等完成 flag 到轻量检查完 */
                while (*reinterpret_cast<volatile const uint64_t *>(flagVa) != expect) {
                } /* 自旋等 flag（完成很快，不用 sleep） */
                int bad = 0;
                for (uint32_t i = 0; i < a.count; ++i) {
                    if (dstVas[i] == nullptr ||
                        *reinterpret_cast<const uint8_t *>(dstVas[i]) != BlockFillByte(i)) {
                        bad = 1;
                        break;
                    }
                }
                const uint64_t t1 = NowUs();
                /* 拷贝完成之后、计时区之外：整块校验本轮数据（身份 tag + memcmp 内容） */
                VerifyResult vr = VerifyBlocks(dstVas, a.count, a.size, refBlock.data());
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t cost = t1 - t0;
                    sumUs += cost;
                    costs.push_back(cost);
                    errs.push_back((bad == 0 && vr.ok) ? 0 : 1);
                    verifies.push_back(vr);
                }
                ++expect;
            }
            for (uint32_t k = 0; k < costs.size(); ++k) {
                printf("baseline observer round %u err=%d cost_us=%llu\n", k, errs[k],
                       static_cast<unsigned long long>(costs[k]));
            }
            for (uint32_t k = 0; k < verifies.size(); ++k) {
                if (!verifies[k].ok) {
                    PrintVerifyFail("baseline observer", k, verifies[k]);
                    break;
                }
            }
            printf("baseline observer avg_us=%llu (rounds=%u) verify=%s\n",
                   static_cast<unsigned long long>(sumUs / a.rounds), a.rounds,
                   std::all_of(verifies.begin(), verifies.end(), [](const VerifyResult &v) { return v.ok; }) ? "OK"
                                                                                                            : "FAIL");
        }
    }

    /* 场景 2：cont —— remote 写 local 连续 staging -> flag；local 轮询 flag -> scatter */
    if (runCont) {
        if (!isLocal) {
            PrintLabel("cont (sender, ack/E2E)");
            std::vector<void *> srcs(a.count), dsts(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                srcs[i] = reinterpret_cast<void *>(selfGva + i * a.stride); /* 离散源 */
                dsts[i] = reinterpret_cast<void *>(peerGva + i * a.size);   /* 连续 staging */
            }
            uint64_t sumUs = 0;
            uint64_t sumTransportUs = 0;
            std::vector<uint64_t> costs, transportCosts; /* 热循环不打日志，跑完后统一打印 */
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            uint64_t seq = 1;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t t0 = NowUs(); /* sender E2E 起点：batch 拷贝 + 写完成 flag + 等 ack */
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
                const uint64_t tW = NowUs(); /* 传输段完成：数据落 staging + flag 已投出（未等 ack） */
                if (ret == 0) {
                    /* 握手：等 local scatter 完写回的 ack(值=seq)，此时 E2E 含 local scatter */
                    void *ackVa = nullptr;
                    if (GvaToHostVa(bm, selfGva + ackOff, &ackVa)) {
                        while (*reinterpret_cast<volatile const uint64_t *>(ackVa) != seq) {
                        }
                    }
                }
                const uint64_t t1 = NowUs();
                if (ret != 0) {
                    printf("cont sender abort at iter %u ret=%d\n", r, ret);
                    break;
                }
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t cost = t1 - t0;      /* E2E = 传输 + local scatter + ack */
                    const uint64_t transport = tW - t0; /* 传输 = 写到 staging + flag（不含 local/ack） */
                    sumUs += cost;
                    sumTransportUs += transport;
                    costs.push_back(cost);
                    transportCosts.push_back(transport);
                }
                ++seq;
            }
            for (uint32_t k = 0; k < costs.size(); ++k) {
                printf("cont sender round %u e2e_us=%llu transport_us=%llu\n", k,
                       static_cast<unsigned long long>(costs[k]),
                       static_cast<unsigned long long>(transportCosts[k]));
            }
            printf("cont sender avg_us=%llu transport_avg_us=%llu (rounds=%u)\n",
                   static_cast<unsigned long long>(sumUs / a.rounds),
                   static_cast<unsigned long long>(sumTransportUs / a.rounds), a.rounds);
        } else {
            PrintLabel("cont (receiver poll+scatter)");
            uint64_t sumConvUs = 0;
            uint64_t sumScatterUs = 0;
            std::vector<void *> srcVas(a.count), dstVas(a.count); /* 每轮 GVA->VA 转换结果 */
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            uint64_t expect = 1;
            std::vector<uint64_t> convCosts, scatterCosts; /* 跑完后统一打印 */
            std::vector<int> errs;
            std::vector<VerifyResult> stagVerifies, dispVerifies;
            for (uint32_t r = 0; r < kTotal; ++r) {
                void *flagVa = nullptr;
                GvaToHostVa(bm, selfGva + flagOff, &flagVa);
                if (flagVa == nullptr) {
                    printf("cont receiver flag_va_failed at iter %u\n", r);
                    break;
                }
                while (*reinterpret_cast<volatile const uint64_t *>(flagVa) != expect) {
                } /* 自旋等 flag（完成很快，不用 sleep） */
                int bad = 0;
                /* GVA->VA 转换（每轮真实执行，属收端聚合本地开销的一部分） */
                const uint64_t tc0 = NowUs();
                for (uint32_t i = 0; i < a.count; ++i) {
                    if (!GvaToHostVa(bm, selfGva + i * a.size, &srcVas[i]) ||
                        !GvaToHostVa(bm, selfGva + dispBase + i * a.stride, &dstVas[i])) {
                        bad = 1;
                        break;
                    }
                }
                const uint64_t tc1 = NowUs();
                /* scatter：纯 memcpy(staging -> 离散目标) */
                const uint64_t ts0 = NowUs();
                for (uint32_t i = 0; i < a.count; ++i) {
                    memcpy(dstVas[i], srcVas[i], a.size);
                }
                const uint64_t ts1 = NowUs();
                *reinterpret_cast<uint64_t *>(flagVa) = 0; /* 清 flag */
                {
                    /* 握手：本端 scatter/清 flag 完后写 ack=expect 回 remote，remote 收到才进入下一轮 */
                    void *ackSrcVa = nullptr;
                    int32_t ackRet = -1;
                    if (GvaToHostVa(bm, selfGva + ackOff, &ackSrcVa)) {
                        *reinterpret_cast<uint64_t *>(ackSrcVa) = expect;
                    }
                    smem_copy_params ap{reinterpret_cast<void *>(selfGva + ackOff),
                                        reinterpret_cast<void *>(peerGva + ackOff), sizeof(expect), nullptr};
                    ackRet = smem_bm_copy(bm, &ap, SMEMB_COPY_AUTO, 0);
                    if (ackRet != 0) {
                        printf("cont receiver ack write failed at iter %u ret=%d\n", r, ackRet);
                        break;
                    }
                }
                /* 拷贝完成（ack 已回）之后、计时区之外：整块校验本轮数据。
                   staging 反映"传输是否完整正确"，离散目标反映"scatter 是否正确"，
                   分开便于区分故障点；复用 conv 段已解析的 VA，无额外 GVA 转换。 */
                VerifyResult vStag = VerifyBlocks(srcVas, a.count, a.size, refBlock.data());
                VerifyResult vDisp = VerifyBlocks(dstVas, a.count, a.size, refBlock.data());
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t convCost = tc1 - tc0;
                    const uint64_t scatterCost = ts1 - ts0;
                    sumConvUs += convCost;
                    sumScatterUs += scatterCost;
                    convCosts.push_back(convCost);
                    scatterCosts.push_back(scatterCost);
                    errs.push_back((bad == 0 && vStag.ok && vDisp.ok) ? 0 : 1);
                    stagVerifies.push_back(vStag);
                    dispVerifies.push_back(vDisp);
                }
                ++expect;
            }
            for (uint32_t k = 0; k < convCosts.size(); ++k) {
                printf("cont receiver round %u err=%d conv_us=%llu scatter_us=%llu\n", k, errs[k],
                       static_cast<unsigned long long>(convCosts[k]),
                       static_cast<unsigned long long>(scatterCosts[k]));
            }
            for (uint32_t k = 0; k < stagVerifies.size(); ++k) {
                if (!stagVerifies[k].ok) {
                    PrintVerifyFail("cont receiver staging", k, stagVerifies[k]);
                    break;
                }
            }
            for (uint32_t k = 0; k < dispVerifies.size(); ++k) {
                if (!dispVerifies[k].ok) {
                    PrintVerifyFail("cont receiver scattered", k, dispVerifies[k]);
                    break;
                }
            }
            printf("cont receiver conv_avg_us=%llu scatter_avg_us=%llu (rounds=%u) verify=%s\n",
                   static_cast<unsigned long long>(sumConvUs / a.rounds),
                   static_cast<unsigned long long>(sumScatterUs / a.rounds), a.rounds,
                   (std::all_of(stagVerifies.begin(), stagVerifies.end(),
                                [](const VerifyResult &v) { return v.ok; }) &&
                    std::all_of(dispVerifies.begin(), dispVerifies.end(),
                                [](const VerifyResult &v) { return v.ok; }))
                       ? "OK"
                       : "FAIL");
        }
    }

    smem_bm_leave(bm, 0);
    smem_bm_destroy(bm);
    smem_bm_uninit(0);
    printf("[bench] done\n");
    return 0;
}
