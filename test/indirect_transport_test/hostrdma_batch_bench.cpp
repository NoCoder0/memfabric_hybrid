/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 *
 * hostrdma_batch_bench.cpp
 *
 * 双机 host rdma 小包（默认 600x1KB）跨机拷贝时延测试（只用现有 smem_bm 接口）
 *
 * 对称内存布局（每 rank 各自贡献 localDRAMSize，GVA 空间按 rank 排列）：
 *   [0, stagingEnd)                    连续 staging（cont 的接收侧），默认 count*size
 *   [dispBase, dispBase+count*stride)  count 个离散目标/源（stride 间隔模拟离散）
 *   [scratchOff, wmOff)                进度源槽数组（发送端用；每个 flag 独占一格 8B，写入后不覆写）
 *   [wmOff, wmOff+links*8)             水位槽（每 rail 一个 8B；值 = 全局累计已完成块数，跨轮不归零）
 *   [msgOff, msgOff+msgRegionBytes)    地址消息区（见下）
 *   [doneOff, doneOff+8)               baseline 完成标志（remote 写回 local）
 *
 * 地址下发（真实 copy 接口语义）：**发起方 local 每轮把两侧地址一起交给执行端 remote**，
 * remote 不再自己按公式算地址：
 *   baseline：远端源地址 count 个（离散，不可合并）+ 本地目标地址 count 个（逐个给）
 *   cont    ：远端源地址 count 个 + 本地目标地址 ceil(count/kContDstAgg) 个描述
 *             （本地侧是连续 staging，每 kContDstAgg 个 IO 聚合一个描述）
 * 整份消息一次单边写（baseline ≈ (2*count*8+40) 字节；cont 因聚合更短），并且**计入本轮时延**。
 *
 * 场景：
 *   baseline : remote 按消息里的两侧地址，batch 直写 local 的 count 个离散目标（无 staging/scatter）
 *   cont     : remote 按消息写 local 的连续 staging，进度通知由 smem_bm_copy_batch 接口内部完成
 *              （progressSrc/progressDest/progressBase/progressInterval）：每 --chunk 个 IO 拷完，
 *              接口自动向对端水位槽单边写一次 (progressBase + 已完成个数)；每格源槽独立不复用
 *              （网卡处理 WQE 时才读源，复用会让水位提前）。local 见水位推进就增量 scatter 已到达的
 *              那一段（staging 连续，偏移 = 块号*size），使后续批次的 RDMA 写时延与本地 scatter
 *              在两端重叠。
 *
 * 测量：每轮由 local 驱动，计时在 local 侧用**单时钟**完成（起点 = 下发地址消息之前，
 *   终点 = count 块全部散完），发请求 + 传输 + scatter 全在计时区内，不需要回传信号。
 *   前 --warmup(默认100) 轮不计时，之后 --rounds(默认1000) 轮计入统计。
 *   每端每个场景只打印：avg / P50 / P95 / P99（最近秩法）各一行，cont 收端另加一行
 *   scatter / tail 的均值；其余明细一律不打印，避免影响节奏。
 *   收端校验在所有轮次跑完之后只做一次（不占每轮关键路径），只打一行 OK/FAIL。
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
 *   - --hcom-url 各填本机 RDMA 网卡 IP:port（多个 url 用 ';' 分隔 = 多条 rail）；
 *   - 数据正确性：remote 第 i 块由 FillBlock 填充（前 4B = 块号+1 小端，其余 = (i+1)&0xFF），
 *       local 端在所有轮次跑完后做整块校验（先验身份 tag 再 memcmp），能识别漏写/错位/字节错。
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

#include "smem_bm.h"
#include "smem_bm_def.h"
#include "smem.h" /* smem_set_log_level */

namespace {

constexpr uint32_t kDefaultCount = 600;
constexpr uint64_t kDefaultSize = 1024;
constexpr uint64_t kDefaultStride = 4096; /* 离散摆放间隔 */
constexpr uint64_t kDefaultDramMB = 16;   /* 每 rank 对称 host 内存，需覆盖 源区+目标区+staging+flag */
constexpr uint32_t kDefaultChunk = 128;   /* cont: 每 chunk 个小 IO 发一次 flag */
constexpr uint32_t kDefaultWarmup = 100;  /* 前 100 轮只热身，不计入统计 */
constexpr uint32_t kDefaultRounds = 1000; /* 计时轮数：1000 */
constexpr uint32_t kContDstAgg = 16; /* cont：本地侧是连续 staging，按 16 个 IO 聚合一次地址描述 */
constexpr uint64_t kSpinTimeoutUs = 5ULL * 1000 * 1000; /* 自旋等水位的上限：超过就报错退出，避免静默挂死 */

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
    bool dramMBSet = false; /* 是否显式指定 --dram-mb；未指定时按布局自动放大（count 大时默认 16MB 不够） */
    uint32_t chunk = kDefaultChunk;   /* cont: 每 chunk 个 IO 提交一批并推进一次 flag */
    uint32_t warmup = kDefaultWarmup; /* 前 warmup 轮不计时 */
    uint32_t rounds = kDefaultRounds; /* 计时轮数 */
    int32_t logLevel = -1;            /* <0 表示不改库的日志级别；0~5 见 smem_set_log_level */
};

void Usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --role=local|remote --rank=N --store-url=tcp://ip:port "
            "--hcom-url=tcp://ip:port [options]\n"
            "  --mode=all|baseline|cont   场景(默认 all)\n"
            "  --count=N                  小 IO 数(默认600)\n"
            "  --size=N                   单块字节(默认1024)\n"
            "  --stride=N                 离散摆放间隔(默认4096，必须 >= --size)\n"
            "  --dram-mb=N                每 rank 对称 host 内存 MB(不指定则按 count/size/stride 自动计算，最小16)\n"
            "  --chunk=N                  cont: 每 N 个小 IO 提交一批并发一次 flag(默认128)\n"
            "  --warmup=N                 前 N 轮不计时(默认100)\n"
            "  --rounds=N                 计时轮数(默认1000)\n"
            "  --log-level=0..5           设置库日志级别(默认不改)\n"
            "  --with-store=0|1           本进程是否内嵌启动 config store(默认0)\n"
            "  说明: 两个场景都由 local 驱动轮次并在本端用单时钟计时；\n"
            "        每轮 local 先把【两侧地址】单边下发给 remote（计时区内），remote 照此投数据；\n"
            "        本地侧聚合固定：baseline 每个目标一个描述；cont 每 %u 个 IO 一个描述；\n"
            "        baseline = 直写 local 的 count 个离散目标（不 scatter）；\n"
            "        cont     = 写 local 的连续 staging（水位推进）→ local 边收边散。\n",
            prog, kContDstAgg);
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
            a.dramMBSet = true;
        } else if (k == "--chunk") {
            a.chunk = static_cast<uint32_t>(std::stoul(v));
        } else if (k == "--warmup") {
            a.warmup = static_cast<uint32_t>(std::stoul(v));
        } else if (k == "--rounds") {
            a.rounds = static_cast<uint32_t>(std::stoul(v));
        } else if (k == "--log-level") { /* 0~5: DEBUG/INFO/WARN/ERROR/FATAL/TRACE；不传则不改库的级别 */
            a.logLevel = static_cast<int32_t>(std::stoi(v));
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
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

/* 两端就绪握手（计时区之外）：确认对端已完成 smem_bm_create 且 RDMA 链路**双向**可用。
   本端只【写对端槽】、只【读本端槽】（本端槽由对端写），所以不会把自己收到的标记覆盖掉。
   两阶段：①两端先互换 phase1，证明"我起来了、且我的写能到你"；
          ②本端见到对端 phase1 后升到 phase2，只有见到对端 phase2 才算握上。
   为什么必须两阶段：若本端写不通、只有对端写能通（对端 VA 还没登记/链路单向），只判"我收到了
   对端标记"会误判成功，带着其实没握上的状态进场景 —— 早跑的一端场景超时后直接 destroy 退出，
   晚跑的一端随即因为对端 VA 被摘掉而 direction 推导失败。
   完成本端 phase2 需要收到对端 phase2，而对端 phase2 是以"收到本端 phase2"为前提的，
   于是【本端完成 ⇒ 对端收到过本端的写 ⇒ 本端的写真的通】，两个方向都被证明过。 */
constexpr uint64_t kReadyMagicBase = 0x524541445900ULL;   /* "READY" 前缀 */
constexpr uint32_t kReadyRetryMinMs = 100;                /* 重发起始间隔（对端秒起时尽快握上） */
constexpr uint32_t kReadyRetryMaxMs = 1000;               /* 重发间隔上限（对端久等时少刷库日志） */
constexpr uint64_t kReadyTimeoutUs = 30ULL * 1000 * 1000; /* 给对端留 30s 启动时间 */

uint64_t ReadyMagic(uint32_t rank, uint32_t phase)
{
    return kReadyMagicBase + (static_cast<uint64_t>(rank) << 8) + phase;
}

bool WaitPeerReady(smem_bm_t bm, uint32_t selfRank, uint32_t peerRank, uint64_t selfGva, uint64_t peerGva,
                   uint64_t readyOff, uint64_t readyOutOff)
{
    auto *selfSlot = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + readyOff));
    auto *outSlot = reinterpret_cast<uint64_t *>(HostPtr(selfGva + readyOutOff));
    const uint64_t peerP1 = ReadyMagic(peerRank, 1);
    const uint64_t peerP2 = ReadyMagic(peerRank, 2);
    uint32_t phase = 1;
    uint64_t out = ReadyMagic(selfRank, phase);
    uint32_t retryMs = kReadyRetryMinMs;
    const uint64_t t0 = NowUs();
    printf("[bench] waiting for peer ready (handshake, up to %llus)...\n",
           static_cast<unsigned long long>(kReadyTimeoutUs / 1000000ULL));
    for (;;) {
        /* 源必须落在已注册的对称内存里：栈上变量不是 MR，SafePut 会直接拒绝 */
        *outSlot = out;
        smem_copy_params p{HostPtr(selfGva + readyOutOff), HostPtr(peerGva + readyOff), sizeof(out), nullptr};
        (void)smem_bm_copy(bm, &p, SMEMB_COPY_AUTO, 0); /* 失败就下一轮重发，不据此判成功 */
        const uint64_t rv = *selfSlot;
        if (phase == 1 && rv == peerP1) {
            phase = 2; /* 对端在且写得到我：告诉它"我也在，且我写得到你" */
            out = ReadyMagic(selfRank, phase);
        }
        if (rv == peerP2) {
            return true;
        }
        if (NowUs() - t0 > kReadyTimeoutUs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(retryMs));
        if (retryMs < kReadyRetryMaxMs) {
            retryMs = std::min(retryMs * 2, kReadyRetryMaxMs);
        }
    }
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

/* ===== 地址消息（真实 copy 接口语义）=====
   发起方（local）每次拷贝都要把【两侧地址】交给执行端（remote），remote 不再自己按公式算地址：
     - baseline：srcCount = count（远端离散源，不可合并）+ dstCount = count（本地目标逐个给）
     - cont    ：srcCount = count + dstCount = ceil(count/agg)，agg = kContDstAgg
       （本地侧是连续 staging，每个描述覆盖 agg*size 字节的连续区）
   消息布局（写方填在自己段的 msgOff，一次单边写搬到对端段的 msgOff；固定按 count 定长）：
     +0                      progressDestGva：local 的水位槽地址（remote 把进度写到这里）
     +8                      doneDestGva：local 的完成槽地址（baseline remote 写完写这里）
     +16                     agg
     +24                     dstCount（有效目标描述个数；srcCount 恒为 count）
     +32                     uint64 srcList[count]   远端源地址（离散，逐个给）
     +32 + count*8           uint64 dstList[count]   本地目标地址（前 dstCount 项有效）
     +32 + count*16          uint64 seq
   seq 放在**最后**：同一 QP 保序 ⇒ 远端看到新 seq 即整份消息（含两侧地址）已落地。
   消息长度与 seq 偏移按本场景确定的 (count, dstCount) 算（两端同一规则，不需要先读头部）。
   agg>1 时远端按下式展开第 i 个 iov 的目标：dstList[i/agg] + (i%agg)*size。 */
constexpr uint64_t kMsgHdrBytes = 32;
uint64_t MsgBytes(uint32_t count, uint32_t dstCount)
{
    return AlignUp(kMsgHdrBytes + (static_cast<uint64_t>(count) + dstCount) * sizeof(uint64_t) + sizeof(uint64_t), 64);
}
uint64_t MsgSeqOff(uint32_t count, uint32_t dstCount)
{
    return kMsgHdrBytes + (static_cast<uint64_t>(count) + dstCount) * sizeof(uint64_t);
}

/* 时延统计：平均值 + P50/P95/P99（最近秩法） */
struct LatStats {
    uint64_t avg = 0;
    uint64_t p50 = 0;
    uint64_t p95 = 0;
    uint64_t p99 = 0;
};

LatStats CalcLatStats(std::vector<uint64_t> v)
{
    LatStats s{};
    if (v.empty()) {
        return s;
    }
    uint64_t sum = 0;
    for (auto x : v) {
        sum += x;
    }
    s.avg = sum / v.size();
    std::sort(v.begin(), v.end());
    auto pick = [&v](uint32_t p) {
        size_t idx = (v.size() * p + 99) / 100; /* 最近秩：ceil(p/100 * N) */
        if (idx == 0) {
            idx = 1;
        }
        if (idx > v.size()) {
            idx = v.size();
        }
        return v[idx - 1];
    };
    s.p50 = pick(50);
    s.p95 = pick(95);
    s.p99 = pick(99);
    return s;
}

void PrintLat(const char *who, const char *what, const std::vector<uint64_t> &samples, uint32_t warmup)
{
    LatStats s = CalcLatStats(samples);
    printf("%s %s_us: avg=%llu p50=%llu p95=%llu p99=%llu (warmup=%u timed=%zu)\n", who, what,
           static_cast<unsigned long long>(s.avg), static_cast<unsigned long long>(s.p50),
           static_cast<unsigned long long>(s.p95), static_cast<unsigned long long>(s.p99), warmup, samples.size());
}

/* local：填一份地址消息（两侧地址都由发起方给）并单边写到对端 msgOff（一次写 = 一个请求） */
void SendAddrMsg(smem_bm_t bm, uint64_t selfGva, uint64_t peerGva, uint64_t msgOff, const std::vector<void *> &srcs,
                 const std::vector<void *> &dsts, uint32_t count, uint32_t dstCount, uint32_t agg,
                 uint64_t progressDest, uint64_t doneDest, uint64_t seq)
{
    auto *msg = reinterpret_cast<uint64_t *>(HostPtr(selfGva + msgOff));
    msg[0] = progressDest;
    msg[1] = doneDest;
    msg[2] = agg;
    msg[3] = dstCount;
    auto *srcList = reinterpret_cast<uint64_t *>(HostPtr(selfGva + msgOff + kMsgHdrBytes));
    auto *dstList = srcList + count;
    for (uint32_t i = 0; i < count; ++i) {
        srcList[i] = reinterpret_cast<uint64_t>(srcs[i]);
    }
    for (uint32_t j = 0; j < dstCount; ++j) {
        dstList[j] = reinterpret_cast<uint64_t>(dsts[static_cast<size_t>(j) * agg]); /* 聚合描述：段的起始地址 */
    }
    *reinterpret_cast<uint64_t *>(HostPtr(selfGva + msgOff + MsgSeqOff(count, dstCount))) = seq;
    std::atomic_thread_fence(std::memory_order_release);
    smem_copy_params mp{HostPtr(selfGva + msgOff), HostPtr(peerGva + msgOff), MsgBytes(count, dstCount), nullptr};
    (void)smem_bm_copy(bm, &mp, SMEMB_COPY_AUTO, 0);
}

/* remote：把对端下发的地址消息展开成两侧地址列表（自己不再算地址） */
void ReadAddrMsg(uint64_t selfGva, uint64_t msgOff, uint32_t count, uint32_t dstCount, uint32_t agg, uint64_t size,
                 std::vector<void *> &srcs, std::vector<void *> &dsts, uint64_t &progressDest, uint64_t &doneDest)
{
    auto *msg = reinterpret_cast<const uint64_t *>(HostPtr(selfGva + msgOff));
    progressDest = msg[0];
    doneDest = msg[1];
    const auto *srcList = reinterpret_cast<const uint64_t *>(HostPtr(selfGva + msgOff + kMsgHdrBytes));
    const auto *dstList = srcList + count;
    for (uint32_t i = 0; i < count; ++i) {
        srcs[i] = reinterpret_cast<void *>(srcList[i]);
        uint32_t j = (agg > 1) ? std::min(i / agg, dstCount - 1) : std::min(i, dstCount - 1);
        dsts[i] = reinterpret_cast<void *>(dstList[j] + (i % agg) * size);
    }
}

void PrintLabel(const char *mode)
{
    printf("\n==== mode=%s ====\n", mode);
}

/* ===== 可选的 per-rail 到达时间线（默认关闭）=====
   设 MF_BENCH_RAIL_TRACE=1 打开，只打印前 kRailTraceRounds 轮。用途：判定**两条 rail 的数据
   是否真的同时在跑** ——
     到达时刻交错（rail0/rail1 混在同一时间窗内）  → 真并行；
     rail0 的三批全部先到、rail1 的三批全部后到    → 串行（提交线程把两条 rail 排成一前一后）。
   只在开启时插 NowUs()，关闭时零开销、不影响任何时序。 */
constexpr uint32_t kRailTraceRounds = 3;
bool RailTraceEnabled()
{
    return getenv("MF_BENCH_RAIL_TRACE") != nullptr;
}

/* ===== 可选的"主线程绑核"（默认关闭）=====
   设 MF_BENCH_APP_CPU=<cpu> 就把本进程主线程钉到该核。收端的时间/轮询/散播都在这个线程上，
   而库自己的忙轮询 worker / 提交 worker 是另外的线程（通常由 MF_HYBM_HCOM_WORKER_CPU_RANGE、
   MF_HYBM_SUBMIT_CPU_RANGE 指定），把计时线程显式钉住可以避免它被调度器搬来搬去、也便于和
   库线程彻底分开 —— 与"同事的 demo 用 --app-cpu 单独指定 app 核"是同一套做法。
   ⚠ 绑核只能限制本线程，并不能独占该核：库的后台线程仍可能被调度上来。所以收益预期是个位数 µs，
   主要作用是**去掉调度抖动**（p99），不是压均值。不设该变量则完全不改行为。 */
void PinMainThreadIfRequested()
{
    const char *cpuStr = std::getenv("MF_BENCH_APP_CPU");
    if (cpuStr == nullptr || *cpuStr == '\0') {
        return;
    }
    const int cpu = std::atoi(cpuStr);
    if (cpu < 0) {
        printf("[bench] ignore invalid MF_BENCH_APP_CPU=%s\n", cpuStr);
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        printf("[bench] pin main thread to cpu %d failed (errno=%d), ignored\n", cpu, errno);
        return;
    }
    printf("[bench] main thread pinned to cpu %d\n", cpu);
}

} // namespace

int main(int argc, char *argv[])
{
    BenchArgs a;
    if (!ParseArgs(argc, argv, a)) {
        Usage(argv[0]);
        return 1;
    }
    /* chunk 非法或超过总数：退化为整批一次 flag */
    if (a.chunk == 0 || a.chunk > a.count) {
        a.chunk = a.count;
    }
    if (a.stride < a.size) {
        fprintf(stderr, "--stride 必须 >= --size（否则各块互相重叠，数据无意义）\n");
        return 1;
    }
    const bool isLocal = (a.role == "local");
    /* 本地侧目标地址描述的个数（= 发起方每轮下发的目标地址列表长度）：
       baseline 的本地目标是离散的 → 逐个给；cont 的本地侧是连续 staging → 按 kContDstAgg 聚合。 */
    const uint32_t baseDstCount = a.count;
    /* 必须向上取整：若用整除，count 不是 agg 整数倍时最后 count%agg 个 iov 会被夹到最后一个描述上，
       目标地址算错（实测：600/16=37 → 592~599 号块被写到 576 号槽，校验必然 FAIL）。 */
    const uint32_t contDstCount = (a.count + kContDstAgg - 1) / kContDstAgg;
    /* 链路档位：库侧恒为 1 条 transport 链路 —— 多网卡由 HCOM 在同一个 service 内部建多条 rail
       并自动分流（库侧 epCount_ = 1）。hcom-url 里的 url 个数只决定库用几张网卡，
       **不参与 bench 的布局计算**（但水位槽个数要按 rail 数）。 */
    uint32_t links = 1;
    for (char c : a.hcomUrl) {
        if (c == ';') {
            ++links;
        }
    }
    printf("[bench] role=%s rank=%u count=%u size=%llu stride=%llu warmup=%u rounds=%u chunk=%u "
           "baseDstCount=%u contDstCount=%u links=%u mode=%s\n",
           a.role.c_str(), a.rank, a.count, static_cast<unsigned long long>(a.size),
           static_cast<unsigned long long>(a.stride), a.warmup, a.rounds, a.chunk, baseDstCount, contDstCount, links,
           a.mode.c_str());

    /* 布局常量（两端同一公式）
       [0, stagingEnd)                    连续 staging（cont 的接收侧）
       [stagingEnd, +4096)                预留 gap
       [dispBase, dispBase+count*stride)  count 个离散目标/源（stride 间隔模拟离散）
       [scratchOff, wmOff)                进度源槽数组（发送端用；flagsPerRound*links 个 8B，写入后不覆写）
       [wmOff, wmOff+links*8)             水位槽（每条 rail 一个 8B；值 = 全局块号，跨轮单调不归零）
       [msgOff, msgOff+msgRegionBytes)    地址消息（每轮 local 单边下发，布局见 Msg* 注释）
       [doneOff, doneOff+8)               baseline 完成标志（remote 写回 local）
       [readyOutOff/readyOff, +8 各]      就绪握手槽 */
    /* DRAM 默认值按布局自动放大（仅在未显式指定 --dram-mb 时）：
       布局至少要装下 staging(count×size) + gap + 离散区(count×stride) + 地址消息区 + 槽区余量。
       否则 count 一变大就会命中下面的 "dram-mb too small" 断言
       —— 例如 count=9600、size=1KB、stride=4KB 需要约 50MB，而默认只有 16MB。 */
    if (!a.dramMBSet) {
        const uint64_t stagingNeed = AlignUp(a.count * a.size, 4096) + 4096;
        const uint64_t dispNeed = a.count * a.stride;
        const uint64_t msgNeed = AlignUp(MsgBytes(a.count, baseDstCount), 4096);
        const uint64_t slotNeed =
            AlignUp(static_cast<uint64_t>((a.count + a.chunk - 1) / a.chunk) * links * 8ULL, 64) + 4 * 8 + 4096;
        const uint64_t minBytes = stagingNeed + dispNeed + msgNeed + slotNeed;
        uint64_t needMB = (minBytes + (1ULL << 20) - 1) >> 20;
        needMB = ((needMB + 15) / 16) * 16; /* 向上取整到 16MB */
        if (needMB > a.dramMB) {
            fprintf(stderr, "[bench] --dram-mb 未显式指定，按布局自动提升: %llu -> %llu MB\n",
                    static_cast<unsigned long long>(a.dramMB), static_cast<unsigned long long>(needMB));
            a.dramMB = needMB;
        }
    }

    const uint64_t dramBytes = a.dramMB * 1024 * 1024;
    const uint64_t stagingEnd = AlignUp(a.count * a.size, 4096); /* staging 连续区尾部 */
    const uint64_t dispBase = stagingEnd + 4096;                 /* 离散源/目标区起点 */
    const uint64_t needBytes = dispBase + a.count * a.stride;
    const uint32_t flagsPerRound = (a.count + a.chunk - 1) / a.chunk; /* 每条 rail 每轮最多这么多个水位 */
    const uint64_t wmOff = dramBytes - static_cast<uint64_t>(links) * 8ULL; /* 水位槽区（每 rail 一个 8B） */
    const uint64_t flagOff = wmOff;                                         /* rail0 的水位槽 */
    const uint64_t scratchOff =
        wmOff - AlignUp(static_cast<uint64_t>(flagsPerRound) * links * 8ULL, 64); /* 进度源槽数组起点 */
    const uint64_t readyOutOff = scratchOff - 8;                                  /* 握手发送槽：本端写 */
    const uint64_t readyOff = readyOutOff - 8;                                    /* 握手接收槽：对端写 */
    /* 地址消息区：放在"离散区之后、段尾控制槽之前"的空隙里。
       ⚠ 绝不能放进 staging 区：baseline 发端的数据源就在 [0, count*stride)，把控制区放那里会污染源块。
       固定按"两侧各 count 个地址"预留整块空间，本轮实际有效项数由消息头的 dstCount/agg 决定。 */
    const uint64_t msgOff = AlignUp(needBytes, 4096);
    /* 消息区按两个场景里较大的那份预留（baseline 的列表最长 = count + count 项） */
    const uint64_t msgRegionBytes = MsgBytes(a.count, baseDstCount);
    const uint64_t doneOff = msgOff + msgRegionBytes;
    if (doneOff + 8 > readyOff) {
        fprintf(stderr,
                "dram-mb too small: need >= %llu bytes (count=%u size=%llu stride=%llu)，请加 --dram-mb=%llu\n",
                static_cast<unsigned long long>(needBytes + msgRegionBytes + 4096), a.count,
                static_cast<unsigned long long>(a.size), static_cast<unsigned long long>(a.stride),
                static_cast<unsigned long long>(((needBytes + msgRegionBytes + 8192) + (1ULL << 20) - 1) >> 20));
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
    if (a.logLevel >= 0) {
        /* 提库的日志级别：HCOM 的日志经 HcomExternalLoggerAdapter 转发进本仓 logger，
           而默认级别是 WARN，会挡掉库的 INFO（例如 MultiRail 的 "create driver xxx_0/_1"）。 */
        (void)smem_set_log_level(a.logLevel);
        printf("[bench] smem log level -> %d\n", a.logLevel);
    }
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
    printf("[bench] selfGva=0x%llx peerGva=0x%llx stagingEnd=%llu flagOff=%llu scratchOff=%llu dispBase=%llu\n",
           static_cast<unsigned long long>(selfGva), static_cast<unsigned long long>(peerGva),
           static_cast<unsigned long long>(stagingEnd), static_cast<unsigned long long>(flagOff),
           static_cast<unsigned long long>(scratchOff), static_cast<unsigned long long>(dispBase));
    if (selfGva == 0 || peerGva == 0) {
        fprintf(stderr, "get gva failed\n");
        return 1;
    }

    /* 56-bit GVA 在本路径恒为关闭（见 HostPtr 注释），GVA == VA，无任何转换，故这里不做自检 */
    printf("[bench] 56-bit GVA off (hardcoded in smem_bm_create): GVA == VA, no conversion\n");

    /* 3. 初始化数据：remote 源填值 i；local 的 staging/目标清 0；控制槽清 0 */
    {
        for (uint32_t e = 0; e < links; ++e) { /* 每条 rail 一个水位槽 */
            *reinterpret_cast<uint64_t *>(HostPtr(selfGva + wmOff + static_cast<uint64_t>(e) * 8ULL)) = 0;
        }
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + doneOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + readyOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + readyOutOff)) = 0;
        /* 两个场景各有一套消息布局，两份 seq 都清零 */
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + msgOff + MsgSeqOff(a.count, baseDstCount))) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + msgOff + MsgSeqOff(a.count, contDstCount))) = 0;
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
    /* 每场景的轮数；seq 用【每场景独立的号段】而不是都从 1 开始 ——
       避免 next 场景的 remote 读到上一场景残留的消息就当成"本轮请求"（地址语义完全不同）。
       baseline: [1, R]；cont: [R+1, 2R]（只跑 cont 时 R=0，即从 1 开始）。 */
    const uint64_t kRoundsPerScenario = static_cast<uint64_t>(a.warmup) + a.rounds;

    /* 就绪握手：链路没建好就进场景，早起的一端会在等 flag 时超时并提前退出，
       晚起的一端随即因为对端 VA 被摘掉而 direction 推导失败（见 WaitPeerReady 注释）。
       两端都过了这一步才开始跑，因此两个进程按任意顺序、隔几秒启动都行。 */
    if (!WaitPeerReady(bm, a.rank, peerRank, selfGva, peerGva, readyOff, readyOutOff)) {
        fprintf(stderr, "[bench] wait peer ready TIMEOUT: 对端未启动或链路未建立"
                        "（确认两端 --hcom-url / --mode 一致，store 无残留进程）\n");
        smem_bm_destroy(bm);
        return 1;
    }
    printf("[bench] peer ready handshake OK\n");

    /* ⚠ 绑核必须放在这里：**要在建链/握手完成之后**。
       新建线程会继承创建线程的 affinity，若在 smem_bm_init 之前就把主线程钉到一个核，
       之后库内部创建的线程（store/acc/重连…）会一起继承成"只有一个核"，反而害了它自己。 */
    PinMainThreadIfRequested();

    /* cont 的驱动方向：**每轮由 local 发起**（local 发消息 → remote 写 → local 边收边散），
       因此整个流程的计时在 local 侧用单时钟完成（见 receiver 的 cont 段）。
       请求里带 staging 基址（跨轮不变）与轮次号；remote 只按收到的东西投数据，不参与计时。 */

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
            std::vector<uint64_t> costs; /* 热循环不打日志，跑完后统一打印 */
            const uint32_t kTotal = a.warmup + a.rounds;
            /* 轮次由 local 驱动：等对端消息的 seq 推进（同 QP 保序 ⇒ 整份消息已落地），
               然后按消息里的**两侧地址**投数据 —— 自己不再按公式算地址。 */
            auto *seqVa =
                reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + msgOff + MsgSeqOff(a.count, baseDstCount)));
            uint64_t lastSeq = 0;
            bool aborted = false;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t w0 = NowUs();
                while (*seqVa == lastSeq) {
                    if (NowUs() - w0 > kSpinTimeoutUs) {
                        printf("baseline sender TIMEOUT at iter %u: 等不到 local 的地址消息\n", r);
                        aborted = true;
                        break;
                    }
                }
                if (aborted) {
                    break;
                }
                lastSeq = *seqVa;
                uint64_t progressDest = 0;
                uint64_t doneDest = 0;
                ReadAddrMsg(selfGva, msgOff, a.count, baseDstCount, 1, a.size, srcs, dsts, progressDest, doneDest);
                const uint64_t t0 = NowUs(); /* sender 时延 = 一次 batch 从调用到返回 */
                smem_batch_copy_params p{};
                p.sources = srcs.data();
                p.destinations = dsts.data();
                p.dataSizes = sizes.data();
                p.batchSize = a.count;
                int32_t ret = smem_bm_copy_batch(bm, &p, SMEMB_COPY_AUTO, 0);
                const uint64_t t1 = NowUs();
                if (ret != 0) {
                    printf("baseline sender abort at iter %u ret=%d\n", r, ret);
                    break;
                }
                /* 600 块写完之后，在**同一条 channel** 上把完成标志写到 local（地址由消息给出）：
                   QP 内保序 ⇒ 标志到 = 前面 600 块已全部落地，local 才能用它做单时钟端到端计时。 */
                *reinterpret_cast<uint64_t *>(HostPtr(selfGva + doneOff)) = lastSeq;
                std::atomic_thread_fence(std::memory_order_release);
                smem_copy_params dp{HostPtr(selfGva + doneOff), reinterpret_cast<void *>(doneDest), sizeof(uint64_t),
                                    nullptr};
                ret = smem_bm_copy(bm, &dp, SMEMB_COPY_AUTO, 0);
                if (ret != 0) {
                    printf("baseline sender done-flag write failed at iter %u ret=%d\n", r, ret);
                    break;
                }
                if (r >= a.warmup) {
                    costs.push_back(t1 - t0);
                }
            }
            PrintLat("baseline sender", "transport", costs, a.warmup);
        } else {
            PrintLabel("baseline (observer)");
            /* 发起方（local）手里有两侧地址：远端源地址 + 本地目标地址，每轮一起下发 */
            std::vector<void *> dstVas(a.count);
            std::vector<void *> peerSrcVas(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                dstVas[i] = HostPtr(selfGva + dispBase + i * a.stride);
                peerSrcVas[i] = HostPtr(peerGva + i * a.stride);
            }
            auto *doneVa = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + doneOff));
            const uint32_t kTotal = a.warmup + a.rounds;
            uint64_t expect = 1;
            uint32_t timedOutAt = UINT32_MAX;
            std::vector<uint64_t> e2eCosts;
            uint32_t verifyFail = 0;
            VerifyResult firstFail{};
            for (uint32_t r = 0; r < kTotal; ++r) {
                /* 本端**驱动轮次并单时钟计时**：① 下发地址消息（两侧地址，计时区内）
                   → ② remote 按消息直写 600 块 → ③ remote 把完成标志写回本端 done 槽。
                   baseline 不做 scatter（数据直写 600 个离散目标），所以这个 e2e 就是纯传输时长。 */
                const uint64_t t0 = NowUs();
                SendAddrMsg(bm, selfGva, peerGva, msgOff, peerSrcVas, dstVas, a.count, baseDstCount, 1,
                            selfGva + flagOff, selfGva + doneOff, expect);
                bool timedOut = false;
                const uint64_t tw0 = NowUs();
                while (*doneVa < expect) { /* 用 < 而非 == ，避免对端领先时两端失步死等 */
                    if (NowUs() - tw0 > kSpinTimeoutUs) {
                        timedOut = true;
                        break;
                    }
                }
                const uint64_t t1 = NowUs();
                if (timedOut) {
                    timedOutAt = r;
                    break;
                }
                if (r >= a.warmup) {
                    e2eCosts.push_back(t1 - t0);
                }
                /* 计时区之外：整块校验本轮数据（身份 tag + memcmp 内容） */
                VerifyResult vr = VerifyBlocks(dstVas, a.count, a.size, refBlock.data());
                if (!vr.ok) {
                    ++verifyFail;
                    if (verifyFail == 1) {
                        firstFail = vr;
                    }
                }
                ++expect;
            }
            if (timedOutAt != UINT32_MAX) {
                printf("baseline observer TIMEOUT at iter %u: done=%llu expect=%llu —— 对端没在推进完成标志\n",
                       timedOutAt, static_cast<unsigned long long>(*doneVa), static_cast<unsigned long long>(expect));
            }
            PrintLat("baseline observer", "e2e", e2eCosts, a.warmup);
            if (verifyFail != 0) {
                PrintVerifyFail("baseline observer", 0, firstFail);
            }
            printf("baseline observer verify=%s (timed=%zu)\n", verifyFail == 0 ? "OK" : "FAIL", e2eCosts.size());
        }
    }

    /* 场景 2：cont —— 进度通知由 smem_bm_copy_batch **接口内部**完成：每 a.chunk 个 IO 拷完，
       接口自动向对端水位槽单边写一次 (progressBase + 已完成个数)；local 见水位推进就增量 scatter。
       应用只填参数、只调一次。
       目的地址由 local 在**每轮请求里**显式给出；整轮流程（local 发请求 → remote 写 → local 边收边散）
       全在计时区内，计时在 local 侧用单时钟完成，不需要任何回传信号。 */
    if (runCont) {
        if (!isLocal) {
            PrintLabel("cont (sender)");
            std::vector<void *> srcs(a.count), dsts(a.count);
            std::vector<uint64_t> transportCosts;
            const uint32_t kTotal = a.warmup + a.rounds;
            /* 本轮是否开始由 local 决定：轮询对端消息的 seq，看到推进就按消息里的两侧地址投数据
               （远端离散源 + local 的连续 staging 目标，都由发起方给出）。发端不参与计时。
               起始 seq 用 cont 号段（只跑 cont 时为 0），避免把上一场景残留的消息当成本轮请求。 */
            auto *seqVa =
                reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + msgOff + MsgSeqOff(a.count, contDstCount)));
            uint64_t lastSeq = runBase ? kRoundsPerScenario : 0;
            bool aborted = false;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t w0 = NowUs();
                while (*seqVa == lastSeq) {
                    if (NowUs() - w0 > kSpinTimeoutUs) {
                        printf("cont sender TIMEOUT at iter %u: 等不到 local 的地址消息\n", r);
                        aborted = true;
                        break;
                    }
                }
                if (aborted) {
                    break;
                }
                lastSeq = *seqVa;
                uint64_t progressDest = 0;
                uint64_t doneDest = 0;
                ReadAddrMsg(selfGva, msgOff, a.count, contDstCount, kContDstAgg, a.size, srcs, dsts, progressDest,
                            doneDest);
                smem_batch_copy_params p{};
                p.sources = srcs.data();
                p.destinations = dsts.data();
                p.dataSizes = sizes.data();
                p.batchSize = a.count;
                p.progressSrc = HostPtr(selfGva + scratchOff); /* 本端进度源槽数组（每个 flag 一格） */
                p.progressDest = reinterpret_cast<void *>(progressDest); /* 对端水位槽地址（由消息给出） */
                p.progressBase = static_cast<uint64_t>(r) * a.count;     /* 跨轮单调、不归零 */
                p.progressInterval = a.chunk;
                const uint64_t t0 = NowUs();
                const int32_t ret = smem_bm_copy_batch(bm, &p, SMEMB_COPY_AUTO, 0);
                const uint64_t tW = NowUs();
                if (ret != 0) {
                    printf("cont sender abort at iter %u ret=%d\n", r, ret);
                    aborted = true;
                    break;
                }
                if (r >= a.warmup) {
                    transportCosts.push_back(tW - t0);
                }
            }
            PrintLat("cont sender", "transport", transportCosts, a.warmup);
        } else {
            PrintLabel("cont (receiver)");
            /* 两侧地址都由本端（发起方）指定：远端离散源 + 本地连续 staging 目标；
               staging / scatter 目标地址固定，一次性解析（GVA==VA，无每轮转换开销）。 */
            std::vector<void *> srcVas(a.count), dstVas(a.count), peerSrcVas(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                srcVas[i] = HostPtr(selfGva + i * a.size);              /* staging 第 i 块的落地位置 */
                dstVas[i] = HostPtr(selfGva + dispBase + i * a.stride); /* scatter 目标 */
                peerSrcVas[i] = HostPtr(peerGva + i * a.stride);        /* 远端源地址（发起方指定） */
            }
            const uint32_t kTotal = a.warmup + a.rounds;
            /* 每条 rail 一段【连续】iov + 一条水位槽，分区规则与库侧一致
               （base = count/K, rem = count%K, rail e 拿 base + (e<rem?1:0) 个块），
               水位值是【全局块号】，收端按 rail 各自增量 scatter，全部 rail 到位才算本轮结束。 */
            std::vector<volatile uint64_t *> wmVas(links);
            for (uint32_t e = 0; e < links; ++e) {
                wmVas[e] =
                    reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + wmOff + static_cast<uint64_t>(e) * 8ULL));
            }
            std::vector<uint64_t> donePerEp(links, 0);
            std::vector<uint64_t> endPerEp(links, 0);
            /* cont 的 seq 用独立号段（跑了 baseline 就从 kRoundsPerScenario+1 起），
               这样两端对"本轮 seq 是多少"的预期完全确定，不需要额外屏障。 */
            uint64_t expect = (runBase ? kRoundsPerScenario : 0) + 1;
            std::vector<uint64_t> e2eCosts;
            uint64_t sumScatterUs = 0; /* 本地 scatter 累计（只算 memcpy，不含轮询等待） */
            uint64_t sumTailUs = 0;    /* 收尾那批 scatter（发端投完之后的"尾巴"） */
            bool aborted = false;
            const bool railTrace = RailTraceEnabled();
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t roundStart = static_cast<uint64_t>(r) * a.count;
                { /* 本轮每条 rail 负责的块区间（全局块号） */
                    const uint32_t per = a.count / links;
                    const uint32_t rem = a.count % links;
                    uint32_t cur = 0;
                    for (uint32_t e = 0; e < links; ++e) {
                        const uint32_t len = per + (e < rem ? 1U : 0U);
                        donePerEp[e] = roundStart + cur;
                        endPerEp[e] = roundStart + cur + len;
                        cur += len;
                    }
                }
                /* 本轮由 local 发起：① 下发地址消息（两侧地址，计时区内）→ ② remote 写 staging 并推进水位
                   → ③ local 边到边散。计时 = 从下发消息之前开始，到 600 块全部散完结束（单时钟）。
                   校验不在这里做，改为所有轮次跑完后只做一次（见循环之后）。 */
                const uint64_t tRoundStart = NowUs();
                SendAddrMsg(bm, selfGva, peerGva, msgOff, peerSrcVas, srcVas, a.count, contDstCount, kContDstAgg,
                            selfGva + flagOff, selfGva + doneOff, expect);
                bool timedOut = false;
                uint64_t scatterUs = 0;
                uint64_t tailUs = 0;
                std::string railTraceLine; /* 开启 rail-trace 时记录"哪条 rail 在第几 us 到货" */
                const uint64_t tw0 = NowUs();
                for (;;) {
                    if (NowUs() - tw0 > kSpinTimeoutUs) {
                        timedOut = true;
                        break;
                    }
                    bool allDone = true;
                    for (uint32_t e = 0; e < links; ++e) {
                        const uint64_t w = *wmVas[e];
                        const uint64_t uptoGlobal = std::min(w, endPerEp[e]);
                        if (uptoGlobal > donePerEp[e]) {
                            const uint32_t from = static_cast<uint32_t>(donePerEp[e] - roundStart);
                            const uint32_t upto = static_cast<uint32_t>(uptoGlobal - roundStart);
                            const uint64_t tb0 = NowUs();
                            for (uint32_t i = from; i < upto; ++i) {
                                memcpy(dstVas[i], srcVas[i], a.size);
                            }
                            const uint64_t tb1 = NowUs();
                            scatterUs += tb1 - tb0;
                            tailUs = tb1 - tb0;
                            if (railTrace && r < kRailTraceRounds) {
                                railTraceLine += " rail" + std::to_string(e) + ":blk" +
                                                 std::to_string(uptoGlobal - roundStart) + "@" +
                                                 std::to_string(tb0 - tRoundStart) + "us";
                            }
                            donePerEp[e] = uptoGlobal;
                        }
                        if (donePerEp[e] < endPerEp[e]) {
                            allDone = false;
                        }
                    }
                    if (allDone) {
                        break; /* 自旋等水位推进（小包完成很快，不用 sleep） */
                    }
                }
                if (timedOut) {
                    printf("cont receiver TIMEOUT at iter %u: 对端没在推进水位。水位实测:", r);
                    for (uint32_t e = 0; e < links; ++e) {
                        printf(" wm[%u]=%llu(需达到 %llu)", e, static_cast<unsigned long long>(*wmVas[e]),
                               static_cast<unsigned long long>(endPerEp[e]));
                    }
                    printf("\n");
                    aborted = true;
                    break;
                }
                if (railTrace && r < kRailTraceRounds) { /* per-rail 到达时间线（判定是否真并行） */
                    printf("[rail-trace] round=%u%s\n", r, railTraceLine.c_str());
                }
                if (r >= a.warmup) {
                    e2eCosts.push_back(NowUs() - tRoundStart);
                    sumScatterUs += scatterUs;
                    sumTailUs += tailUs;
                }
                ++expect;
            }
            PrintLat("cont receiver", "e2e", e2eCosts, a.warmup);
            if (!e2eCosts.empty()) {
                printf("cont receiver scatter_us: avg=%llu tail_avg=%llu (timed=%zu)\n",
                       static_cast<unsigned long long>(sumScatterUs / e2eCosts.size()),
                       static_cast<unsigned long long>(sumTailUs / e2eCosts.size()), e2eCosts.size());
            }
            /* 校验只做一次（所有轮次跑完之后）：此时没有任何后续写入，staging 与离散目标都是末轮真实内容，
               且不再占用每轮关键路径。 */
            if (!aborted) {
                VerifyResult vStag = VerifyBlocks(srcVas, a.count, a.size, refBlock.data());
                VerifyResult vDisp = VerifyBlocks(dstVas, a.count, a.size, refBlock.data());
                if (!vStag.ok) {
                    PrintVerifyFail("cont receiver staging", 0, vStag);
                }
                if (!vDisp.ok) {
                    PrintVerifyFail("cont receiver scattered", 0, vDisp);
                }
                printf("cont receiver verify=%s (staging+scattered)\n", (vStag.ok && vDisp.ok) ? "OK" : "FAIL");
            }
        }
    }

    smem_bm_leave(bm, 0);
    smem_bm_destroy(bm);
    smem_bm_uninit(0);
    printf("[bench] done\n");
    return 0;
}
