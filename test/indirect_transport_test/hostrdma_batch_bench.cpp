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
 *   [dramBytes-8, dramBytes)       完成 flag 槽（段尾，8B；值=全局累计已完成块数，跨轮单调不归零）
 *   remote(rank1) 的 600 源放在自己段的 0..count*stride（离散）；
 *   local(rank0)  的 staging/flag/目标放在自己段内。
 *
 * 场景：
 *   baseline : remote 600x1KB batch 直写 local 600 个离散目标（现有实现，无 staging/scatter）
 *   cont     : remote 600x1KB 按 --chunk(默认128) 分块写 local 连续 staging，每批拷完立刻把
 *              "全局累计已完成块数"写给对端 flag 槽；local 每次见到 flag 推进就增量 scatter
 *              已到达的那一段（staging 连续，偏移 = 块号*size）。这样后续批次的 RDMA 写时延
 *              与本地 scatter 时延在两端重叠，降低端到端时延。
 *
 * 轮次同步：不用 ack，两端每轮跑完各自空转 RoundGap(kRoundGapMs)，把轮次彻底隔离开
 *   （收端有充足时间散完+校验，发端才进下一轮），因此不存在跨轮覆盖，也不会两端失步。
 *   去掉 ack 后发端只能测"纯传输"，端到端 ≈ 发端 transport_avg_us + 收端 tail_avg_us。
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
constexpr uint32_t kDefaultChunk = 128;   /* cont: 每 chunk 个小 IO 发一次 flag */

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
    uint32_t chunk = kDefaultChunk; /* cont: 每 chunk 个 IO 提交一批并推进一次 flag */
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
            "  --chunk=N                  cont: 每 N 个小 IO 提交一批并发一次 flag(默认128)\n"
            "  --rounds=N                 每场景轮数(默认5)\n"
            "  --old=1                    老版口径(默认0，跑 e30bf29 等无聚合 MF 用):\n"
            "                             只跑直写 baseline、只用单 url、无 staging/scatter\n"
            "                             不传 --old = 最新版: baseline 直写 + cont(收端聚合)\n"
            "  --with-store=0|1           本进程是否内嵌启动 config store(默认0)\n"
            "  说明: 两场景均不使用 ack，每轮结束两端各空转 5000ms(计时区外)隔离轮次\n",
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
        } else if (k == "--chunk") {
            a.chunk = static_cast<uint32_t>(std::stoul(v));
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

/* 56-bit GVA 在本 benchmark 的路径上恒为关闭，GVA 就是本进程 VA，这里直接当指针用。
   依据（已核对代码）：
     - smem_bm_create 写死 enable56BitsGva = false（smem_bm.cpp:198），本 benchmark 只用该接口；
     - 未开启时 AllocReserveGva 令 gva = lva（hybm_va_manager.cpp:346）；
     - segment 直接按 GVA mmap（hybm_conn_based_segment.cpp:81）。
   因此全部 GVA->VA 转换都是恒等操作，测试里不再调用转换接口、也不做运行时判断。 */
void *HostPtr(uint64_t gva)
{
    return reinterpret_cast<void *>(gva);
}

uint64_t NowUs()
{
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

/* 每轮结束后两端各自空转（计时区之外），把轮次彻底隔离开，取代轮次 ack：
   收端有足够时间把本轮数据散完/校验完，发端才进入下一轮，因此不存在
   "第 N 轮还没读完、第 N+1 轮已写进同一段地址" 的覆盖，也不会两端失步。
   只增加总时长，不影响任何 cost_us。 */
constexpr uint32_t kRoundGapMs = 5000;

void RoundGap()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(kRoundGapMs));
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
    bool idMismatch = false; /* 身份不符（漏写/错位），而非内容字节错 */
    uint32_t blockIdx = 0;
    uint32_t actualTag = 0; /* 身份不符时：该块实际携带的 tag（0 = 从未被写过） */
    uint64_t byteOff = 0;
    uint8_t expect = 0;
    uint8_t actual = 0;
};

/* 整块校验：先验身份（前 4B tag），再用 memcmp 比内容。
   vas      —— 该组块的本地 VA（GVA==VA，调用方直接算好并复用）；
   refBlock —— 256 个参考块，refBlock[v] 为整块填 v 的 size 字节。
   正常路径每块只做 1 次 tag 比较 + 1 次 memcmp（SIMD），逐字节扫描仅在失败路径上做。 */
VerifyResult VerifyBlocks(const std::vector<void *> &vas, uint32_t count, uint64_t size, const uint8_t *refBlock)
{
    VerifyResult r;
    const bool hasId = (size >= kBlockIdBytes);
    const uint64_t from = hasId ? kBlockIdBytes : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto *p = reinterpret_cast<const uint8_t *>(vas[i]);
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
    if (r.idMismatch) {
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
    /* chunk 非法或超过总数：退化为整批一次 flag（等价旧口径） */
    if (a.chunk == 0 || a.chunk > a.count) {
        a.chunk = a.count;
    }
    const bool isLocal = (a.role == "local");
    printf("[bench] role=%s rank=%u count=%u size=%llu stride=%llu rounds=%u chunk=%u mode=%s\n", a.role.c_str(),
           a.rank, a.count, static_cast<unsigned long long>(a.size), static_cast<unsigned long long>(a.stride),
           a.rounds, a.chunk, a.mode.c_str());
    /* 链路档位：hcom-url 含 ';'（多个 url）即为双卡/双连接，否则单卡/单连接 */
    const bool dualUrl = a.hcomUrl.find(';') != std::string::npos;
    printf("[bench] link-mode=%s hcom-url=%s\n", dualUrl ? "dual-link(2 NIC)" : "single-link(1 NIC)",
           a.hcomUrl.c_str());

    /* 布局常量（两端同一公式）
       [0, stagingEnd)               连续 staging（接收侧）
       [stagingEnd, +4096)           预留 gap
       [dispBase, dispBase+count*stride)  600 个离散目标/源（stride 间隔模拟离散）
       [dramBytes-8, dramBytes)        完成 flag 槽（放段尾，避开源/目标/staging 区；
                                       cont 值 = 全局累计已完成块数，跨轮单调、不归零） */
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

    /* 56-bit GVA 在本路径恒为关闭（见 HostPtr 注释），GVA == VA，无任何转换，故这里不做自检 */
    printf("[bench] 56-bit GVA off (hardcoded in smem_bm_create): GVA == VA, no conversion\n");

    /* 3. 初始化数据：remote 源填值 i；local 的 staging/目标清 0；双方 flag 槽清 0 */
    {
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + flagOff)) = 0;
        if (!isLocal) {
            for (uint32_t i = 0; i < a.count; ++i) {
                FillBlock(HostPtr(selfGva + i * a.stride), i, a.size); /* 前 4B 携带块号，其余填 (i+1)&0xFF */
            }
        } else {
            for (uint32_t i = 0; i < a.count; ++i) {
                memset(HostPtr(selfGva + dispBase + i * a.stride), 0, a.size);
                memset(HostPtr(selfGva + i * a.size), 0, a.size);
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
                srcs[i] = HostPtr(selfGva + i * a.stride);
                dsts[i] = HostPtr(peerGva + dispBase + i * a.stride);
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
                    *reinterpret_cast<uint64_t *>(HostPtr(selfGva + flagOff)) = seq;
                    smem_copy_params fp{HostPtr(selfGva + flagOff), HostPtr(peerGva + flagOff), sizeof(seq), nullptr};
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
                /* baseline 为单向流（发方不等收方），但每轮之间同样空转隔离：
                   否则下一轮直写会覆盖收端正准备校验的同一批离散目标 */
                RoundGap();
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
            /* 600 个目标地址固定，一次性解析供每轮的整块校验使用（GVA==VA，无转换开销） */
            std::vector<void *> dstVas(a.count, nullptr);
            for (uint32_t i = 0; i < a.count; ++i) {
                dstVas[i] = HostPtr(selfGva + dispBase + i * a.stride);
            }
            auto *flagVa = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + flagOff));
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时） */
            uint64_t expect = 1;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t t0 = NowUs(); /* receiver 时延 = 本轮等完成 flag 的耗时（纯等待，不含任何校验） */
                while (*flagVa < expect) {
                } /* 自旋等 flag 水位推进（用 < 而非 != ，避免发端领先时两端失步死等） */
                const uint64_t t1 = NowUs();
                /* 计时区之外：拷贝完成后整块校验本轮数据（身份 tag + memcmp 内容）。
                   每轮之间有 RoundGap 隔离，校验期间不会被下一轮直写覆盖，结果可信。 */
                VerifyResult vr = VerifyBlocks(dstVas, a.count, a.size, refBlock.data());
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t cost = t1 - t0;
                    sumUs += cost;
                    costs.push_back(cost);
                    errs.push_back(vr.ok ? 0 : 1);
                    verifies.push_back(vr);
                }
                RoundGap();
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

    /* 场景 2：cont —— remote 分块写 local 连续 staging，每批写完推进一次 flag（全局累计块数）；
       local 见 flag 推进就增量 scatter。两端不做 ack，靠每轮 RoundGap 隔离轮次。 */
    if (runCont) {
        if (!isLocal) {
            PrintLabel("cont (sender, chunked flag, no ack)");
            std::vector<void *> srcs(a.count), dsts(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                srcs[i] = HostPtr(selfGva + i * a.stride); /* 离散源 */
                dsts[i] = HostPtr(peerGva + i * a.size);   /* 连续 staging */
            }
            /* 分块点：每 a.chunk 个 IO 提交一批、推进一次 flag（本轮已完成块数）。
               600/128 => 128,256,384,512,600 共 5 次 flag */
            std::vector<uint32_t> marks;
            for (uint32_t done = 0; done < a.count; done += a.chunk) {
                marks.push_back(std::min(done + a.chunk, a.count));
            }
            uint64_t sumTransportUs = 0;
            std::vector<uint64_t> transportCosts; /* 热循环不打日志，跑完后统一打印 */
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t t0 = NowUs(); /* 起点：分块拷贝 + 逐批 flag（不等收端，无 ack） */
                int32_t ret = 0;
                for (size_t ci = 0; ci < marks.size() && ret == 0; ++ci) {
                    const uint32_t begin = (ci == 0) ? 0 : marks[ci - 1];
                    const uint32_t len = marks[ci] - begin;
                    smem_batch_copy_params p{srcs.data() + begin, dsts.data() + begin, sizes.data() + begin, len,
                                             nullptr};
                    ret = smem_bm_copy_batch(bm, &p, SMEMB_COPY_AUTO, 0);
                    if (ret != 0) {
                        break;
                    }
                    /* 本批拷贝接口是同步的（内部等写完成），因此这里可以立刻把进度写给对端 flag 槽：
                       local 见到即提前 scatter 本批，与后续批次的 RDMA 写重叠。
                       flag 值 = 全局累计已完成块数（跨轮单调、不归零），收端据此定位 staging 偏移。
                       src 必须用本端已注册内存，故先写本端 flag 槽再单发过去。 */
                    *reinterpret_cast<uint64_t *>(HostPtr(selfGva + flagOff)) =
                        static_cast<uint64_t>(r) * a.count + marks[ci];
                    smem_copy_params fp{HostPtr(selfGva + flagOff), HostPtr(peerGva + flagOff), sizeof(uint64_t),
                                        nullptr};
                    ret = smem_bm_copy(bm, &fp, SMEMB_COPY_AUTO, 0);
                }
                const uint64_t t1 = NowUs();
                if (ret != 0) {
                    printf("cont sender abort at iter %u ret=%d\n", r, ret);
                    break;
                }
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t transport = t1 - t0; /* 传输 = 全部分块写到 staging + 逐批 flag（不含收端） */
                    sumTransportUs += transport;
                    transportCosts.push_back(transport);
                }
                RoundGap(); /* 计时区外：给收端留出本轮 scatter/校验时间，避免下一轮覆盖 staging */
            }
            for (uint32_t k = 0; k < transportCosts.size(); ++k) {
                printf("cont sender round %u transport_us=%llu\n", k,
                       static_cast<unsigned long long>(transportCosts[k]));
            }
            printf("cont sender transport_avg_us=%llu chunks=%zu (rounds=%u, no ack)\n",
                   static_cast<unsigned long long>(sumTransportUs / a.rounds), marks.size(), a.rounds);
            printf("cont sender note: end-to-end ~= transport_avg_us + receiver tail_avg_us\n");
        } else {
            PrintLabel("cont (receiver watermark scatter, no ack)");
            uint64_t sumScatterUs = 0;
            uint64_t sumTailUs = 0;
            /* staging / 离散目标地址固定，一次性解析（GVA==VA，无每轮转换开销） */
            std::vector<void *> srcVas(a.count), dstVas(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                srcVas[i] = HostPtr(selfGva + i * a.size);
                dstVas[i] = HostPtr(selfGva + dispBase + i * a.stride);
            }
            auto *flagVa = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + flagOff));
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            /* flag 是全局累计块数（跨轮不归零），本轮水位 = (r+1)*count；
               scatteredGlobal 与发端 flag 同一口径，据此把全局块号换算回本轮块号定位 staging 偏移 */
            uint64_t scatteredGlobal = 0;
            std::vector<uint64_t> scatterCosts, tailCosts; /* 跑完后统一打印 */
            std::vector<std::string> arrivalLines;         /* 各批数据可见时刻(相对本轮起点,us)，用于确认重叠 */
            std::vector<int> errs;
            std::vector<VerifyResult> stagVerifies, dispVerifies;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t roundStart = static_cast<uint64_t>(r) * a.count;
                const uint64_t roundEnd = static_cast<uint64_t>(r + 1) * a.count; /* 本轮全局水位 */
                const uint64_t tr0 = NowUs(); /* 本轮起点：开始等第一份 flag */
                uint64_t scatterUs = 0;       /* 本轮 scatter 累计耗时（只算 memcpy，不含轮询等待） */
                uint64_t tailUs = 0;          /* 最后一批(600 块中收尾那批)的 scatter 耗时 */
                std::string arrivals;
                while (scatteredGlobal < roundEnd) {
                    const uint64_t doneGlobal = *flagVa;
                    if (doneGlobal <= scatteredGlobal) {
                        continue; /* 自旋等下一批 flag 推进（小包完成很快，不用 sleep） */
                    }
                    const uint64_t uptoGlobal = std::min(doneGlobal, roundEnd);
                    const uint32_t from = static_cast<uint32_t>(scatteredGlobal - roundStart);
                    const uint32_t upto = static_cast<uint32_t>(uptoGlobal - roundStart);
                    const uint64_t tb0 = NowUs();
                    for (uint32_t i = from; i < upto; ++i) {
                        memcpy(dstVas[i], srcVas[i], a.size);
                    }
                    const uint64_t tb1 = NowUs();
                    scatterUs += tb1 - tb0;
                    tailUs = tb1 - tb0; /* 收尾批的 scatter 即发端投完之后的"尾巴" */
                    arrivals += (arrivals.empty() ? "" : ",") + std::to_string(tb0 - tr0);
                    scatteredGlobal = uptoGlobal;
                }
                /* 计时区之外：整块校验本轮数据。staging 反映"传输是否完整正确"，
                   离散目标反映"scatter 是否正确"，分开便于区分故障点。
                   每轮之间有 RoundGap 隔离，校验期间不会被下一轮覆盖。 */
                VerifyResult vStag = VerifyBlocks(srcVas, a.count, a.size, refBlock.data());
                VerifyResult vDisp = VerifyBlocks(dstVas, a.count, a.size, refBlock.data());
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    sumScatterUs += scatterUs;
                    sumTailUs += tailUs;
                    scatterCosts.push_back(scatterUs);
                    tailCosts.push_back(tailUs);
                    arrivalLines.push_back(arrivals);
                    errs.push_back((vStag.ok && vDisp.ok) ? 0 : 1);
                    stagVerifies.push_back(vStag);
                    dispVerifies.push_back(vDisp);
                }
                RoundGap();
            }
            for (uint32_t k = 0; k < scatterCosts.size(); ++k) {
                printf("cont receiver round %u err=%d scatter_us=%llu tail_us=%llu arrivals_us=[%s]\n", k, errs[k],
                       static_cast<unsigned long long>(scatterCosts[k]),
                       static_cast<unsigned long long>(tailCosts[k]), arrivalLines[k].c_str());
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
            printf("cont receiver scatter_avg_us=%llu tail_avg_us=%llu (rounds=%u, no ack) verify=%s\n",
                   static_cast<unsigned long long>(sumScatterUs / a.rounds),
                   static_cast<unsigned long long>(sumTailUs / a.rounds), a.rounds,
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
