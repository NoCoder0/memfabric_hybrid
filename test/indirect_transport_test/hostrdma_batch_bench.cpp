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
 *   [scratchOff, flagOff)          进度源槽数组（发送端用；每个 flag 独占一格 8B，写入后不覆写）
 *   [dramBytes-8, dramBytes)       水位/完成槽（段尾 8B；值=全局累计已完成块数，跨轮单调不归零）
 *   remote(rank1) 的 600 源放在自己段的 0..count*stride（离散）；
 *   local(rank0)  的 staging/flag/目标放在自己段内。
 *
 * 场景：
 *   baseline : remote 600x1KB batch 直写 local 600 个离散目标（现有实现，无 staging/scatter）
 *   cont     : remote 600x1KB 写 local 连续 staging，进度通知由 smem_bm_copy_batch 接口内部完成
 *              （progressSrc/progressDest/progressBase/progressInterval）：每 --chunk(默认128) 个 IO
 *              拷完，接口自动向对端水位槽单边写一次 (progressBase + 已完成个数)；每格的源槽独立
 *              不复用（网卡处理 WQE 时才读源，复用会让水位提前）。local 见到水位推进就增量 scatter
 *              已到达的那一段（staging 连续，偏移 = 块号*size）。这样后续批次的 RDMA 写时延与本地
 *              scatter 时延在两端重叠，降低端到端时延。
 *
 * 轮次同步与测量（cont）：**每轮由 local（接收端）发起** —— local 把 [staging 基址, 轮次号] 单边写给
 *   remote，remote 按收到的地址投本轮数据，local 边收边散，600 块散完即本轮结束。
 *   计时在 local 侧用**单时钟**完成：起点=发请求之前，终点=散完那一刻，整段（发请求 + 传输 + scatter）
 *   都在计时区内，不需要任何回传信号/ack。
 *   两端每轮跑完另各空转 RoundGap(--gap-ms，**默认 0**，计时区外)。
 *   收端校验在所有轮次跑完之后只做一次，避免占用每轮关键路径。
 *
 * 每个场景跑 --rounds 轮，只打印**最后一轮**和**平均值**（避免多轮刷屏）。
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
#include "smem.h" /* smem_set_log_level */

namespace {

constexpr uint32_t kDefaultCount = 600;
constexpr uint64_t kDefaultSize = 1024;
constexpr uint64_t kDefaultStride = 4096; /* 离散摆放间隔 */
constexpr uint64_t kDefaultDramMB = 16;   /* 每 rank 对称 host 内存，需覆盖 源区+目标区+staging+flag */
constexpr uint32_t kDefaultChunk = 128;   /* cont: 每 chunk 个小 IO 发一次 flag */
constexpr uint32_t kDefaultGapMs = 0;     /* 每轮结束后的空转(ms)，默认 0：轮次隔离已由 ack 保证。
                                             空转会让收端的发送路径长时间静默，下一轮那次 8 字节 ack 从几十 us
                                             变成 ~4ms（实测），从而把 e2e 抬高好几倍。需要隔轮校验时才调大。 */
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
    uint32_t chunk = kDefaultChunk; /* cont: 每 chunk 个 IO 提交一批并推进一次 flag */
    uint32_t gapMs = kDefaultGapMs; /* 每轮之后的空转(ms)，计时区外 */
    uint32_t rounds = 5;
    int32_t logLevel = -1; /* <0 表示不改库的日志级别；0~5 见 smem_set_log_level */
    bool old = false; /* --old=1 老版口径(如 e30bf29，无收端聚合): 只跑直写 baseline、只用单 url、无 ack/scatter */
    bool passAddrs = true; /* --pass-addrs: baseline 由 local 显式把 600 个目标地址传给 remote（默认开） */
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
            "  --gap-ms=N                 每轮之后的空转毫秒数，计时区外(默认0；调大会拖慢收端 ack，只在需要隔轮校验时用)\n"
            "  --rounds=N                 每场景轮数(默认5)\n"
            "  --old=1                    老版口径(默认0，跑 e30bf29 等无聚合 MF 用):\n"
            "                             只跑直写 baseline、只用单 url、无 staging/scatter\n"
            "                             不传 --old = 最新版: baseline 直写 + cont(收端聚合)\n"
            "  --pass-addrs=0|1           baseline: local 把自己 600 个目标地址显式传给 remote(默认1)，\n"
            "                             =0 则回退到\"双方按同一公式算出相同目标地址\"\n"
            "  --with-store=0|1           本进程是否内嵌启动 config store(默认0)\n"
            "  说明: 两个场景都由 local 驱动轮次并在本端用单时钟计时；\n"
            "        baseline = 传地址 → 远端单边直写 600 块 → local 收到（不 scatter）；\n"
            "        cont     = 传地址 → 远端单边写 → local 边收边 scatter。\n",
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
        } else if (k == "--gap-ms") {
            a.gapMs = static_cast<uint32_t>(std::stoul(v));
        } else if (k == "--rounds") {
            a.rounds = static_cast<uint32_t>(std::stoul(v));
        } else if (k == "--log-level") { /* 0~5: DEBUG/INFO/WARN/ERROR/FATAL/TRACE；不传则不改库的级别 */
            a.logLevel = static_cast<int32_t>(std::stoi(v));
        } else if (k == "--pass-addrs") {
            a.passAddrs = (v != "0");
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

/* 每轮结束后两端各自空转（计时区之外）。默认 0：轮次隔离由 ack 保证（发端收不到 ack 不会开下一轮），
   不需要空转。空转会拖慢收端下一轮那次 8 字节 ack（发送路径长时间静默，实测几十 us → ~4ms），
   把发端测到的 e2e 抬高好几倍，所以只有需要隔轮校验时才调大。 */
void RoundGap(uint32_t gapMs)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
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
    /* 链路档位：库侧**恒为 1 条 transport 链路** —— 多网卡由 HCOM 在同一个 service 内部
       MultiRail 建多条 rail 并自动分流（库侧 epCount_ = 1）。所以 bench 的 links 也必须恒为 1：
       水位槽个数、每链路负责的块区间都要和库一致，否则两端布局错位、收端永远等不到水位。
       hcom-url 里的 url 个数只决定库用几张网卡，**不参与 bench 的布局计算**。 */
    uint32_t urlCount = 1;
    for (char c : a.hcomUrl) {
        if (c == ';') {
            ++urlCount;
        }
    }
    /* links = 建链用的 url(网卡) 数：传 1 个 url 就是单连接，传多个就是双/多连接。
       MF 侧在多个 url 时会按 rail 前后切分，并按 rail 维护独立水位（每 rail 一个水位槽）。 */
    const uint32_t links = urlCount;
    printf("[bench] link-mode=%s url-count=%u links=%u hcom-url=%s\n",
           urlCount > 1 ? "multi-nic (K channels)" : "single-nic", urlCount, links, a.hcomUrl.c_str());
    printf("[bench] target-addrs=%s\n", a.passAddrs ? "由 local 显式下发 600 个地址" : "双方按同一公式各自算");

    /* 布局常量（两端同一公式）
       [0, stagingEnd)               连续 staging（接收侧）
       [stagingEnd, +4096)           预留 gap
       [dispBase, dispBase+count*stride)  600 个离散目标/源（stride 间隔模拟离散）
       [scratchOff, wmOff)            进度源槽数组（发送端用；flagsPerRound*links 个 8B，写入后不覆写）
       [ackOff, ackOff+8)            完成 ack 槽（收端全部散完后写回发端，用于单时钟测端到端）
       [readyOutOff, readyOff+8)     就绪握手槽 16B（本端写自己的 readyOutOff 再单边写到对端的 readyOff；
                                     本端只读自己的 readyOff，两边互不覆盖）
       [addrMsgOff, addrMsgOff+16)   地址传递槽 16B：[0]=staging 起始 GVA，[8]=完成标记槽 GVA。
                                     由 local（接收端）在运行时显式单边写给 remote，remote 照此写数据/取标记
       [wmOff, wmOff+links*8)         水位槽（每条 link 一个 8B；cont 值 = 全局块号，跨轮单调不归零） */
    const uint64_t dramBytes = a.dramMB * 1024 * 1024;
    const uint64_t stagingEnd = AlignUp(a.count * a.size, 4096); /* staging 连续区尾部 */
    const uint64_t dispBase = stagingEnd + 4096;                 /* 离散源/目标区起点 */
    const uint64_t needBytes = dispBase + a.count * a.stride;
    const uint32_t flagsPerRound = (a.count + a.chunk - 1) / a.chunk; /* 每条 link 每轮最多这么多个水位 */
    const uint64_t wmOff = dramBytes - static_cast<uint64_t>(links) * 8ULL; /* 水位槽区（每 link 一个 8B） */
    const uint64_t flagOff = wmOff;                                         /* 兼容旧代码：ep0 的水位槽 */
    const uint64_t scratchOff =
        wmOff - AlignUp(static_cast<uint64_t>(flagsPerRound) * links * 8ULL, 64); /* 进度源槽数组起点 */
    const uint64_t ackOff = scratchOff - 8;                                       /* 完成 ack 槽：收端写回 */
    const uint64_t readyOutOff = ackOff - 8;                                      /* 握手发送槽：本端写 */
    const uint64_t readyOff = readyOutOff - 8;                                    /* 握手接收槽：对端写 */
    /* 请求槽 16B（每轮 local 用）：[0]=staging 基址（跨轮不变），[8]=轮次号。
       local 写自己的 reqOutOff 再单边写到对端的 reqOff；对端只读自己的 reqOff。
       轮次号写在最后 8 字节，且 staging 基址跨轮不变 —— 即使 16B 写被拆成两次落地，
       也不会读到错的地址。 */
    const uint64_t reqOutOff = readyOff - 16; /* local 的请求发送槽 */
    const uint64_t reqOff = reqOutOff - 16;   /* remote 的请求接收槽 */
    /* baseline 的每轮握手槽（放在 staging 区尾部；baseline 不用 staging 传数据）。
       不复用 cont 的 reqOff/reqOutOff —— mode=all 下 baseline 先跑，残留的轮次号会让 cont 发端误判轮次：
       [baseReqOff, +8)     remote 侧：本轮请求序号（local 单边写过来）
       [baseReqOutOff, +8)  local 侧：本轮请求源（本端写好再单边写到对端的 baseReqOff）
       [baseDoneOff, +8)    local 侧：完成标志（remote 写完 600 块后单边写过来；同一 channel 保序
                            ⇒ 标志到 = 600 块已全部落地，local 才能用它做单时钟端到端计时） */
    const uint64_t baseReqOff = stagingEnd - 24;
    const uint64_t baseReqOutOff = stagingEnd - 16;
    const uint64_t baseDoneOff = stagingEnd - 8;
    /* --pass-addrs（默认开）：地址列表复用 staging 区头部（baseline 不使用 staging）。
       local 把 600 个离散目标地址单边写到对端这里，remote 从本端内存读出来直接当 dsts 用。 */
    const uint64_t addrListOff = 0;
    if (needBytes > reqOff) {
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

    /* 3. 初始化数据：remote 源填值 i；local 的 staging/目标清 0；双方 flag/ack 槽清 0 */
    {
        for (uint32_t e = 0; e < links; ++e) { /* 每条 link 一个水位槽 */
            *reinterpret_cast<uint64_t *>(HostPtr(selfGva + wmOff + static_cast<uint64_t>(e) * 8ULL)) = 0;
        }
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + ackOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + readyOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + readyOutOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + reqOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + reqOff + 8ULL)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + reqOutOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + reqOutOff + 8ULL)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + baseReqOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + baseReqOutOff)) = 0;
        *reinterpret_cast<uint64_t *>(HostPtr(selfGva + baseDoneOff)) = 0;
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

    /* 就绪握手：链路没建好就进场景，早起的一端会在等 flag 时超时并提前退出，
       晚起的一端随即因为对端 VA 被摘掉而 direction 推导失败（见 WaitPeerReady 注释）。
       两端都过了这一步才开始跑，因此两个进程按任意顺序、隔几秒启动都行。 */
    if (!WaitPeerReady(bm, a.rank, peerRank, selfGva, peerGva, readyOff, readyOutOff)) {
        fprintf(stderr,
                "[bench] wait peer ready TIMEOUT: 对端未启动或链路未建立"
                "（确认两端 --hcom-url / --mode 一致，store 无残留进程）\n");
        smem_bm_destroy(bm);
        return 1;
    }
    printf("[bench] peer ready handshake OK\n");

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
            for (uint32_t i = 0; i < a.count; ++i) {
                srcs[i] = HostPtr(selfGva + i * a.stride);
            }
            if (a.passAddrs) {
                /* 等 local 把 600 个目标地址传过来：local 先写列表、再往本端 ackOff 槽写序号，
                   同一 QP 保序 → 看到序号即列表已落地。然后用收到的地址，不套用本端公式。 */
                auto *readyVa = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + ackOff));
                const uint64_t rw0 = NowUs();
                while (*readyVa == 0) {
                    if (NowUs() - rw0 > kSpinTimeoutUs) {
                        printf("[bench] pass-addrs: 等 local 的地址列表 TIMEOUT\n");
                        smem_bm_destroy(bm);
                        return 1;
                    }
                }
                const auto *list = reinterpret_cast<const uint64_t *>(HostPtr(selfGva + addrListOff));
                for (uint32_t i = 0; i < a.count; ++i) {
                    dsts[i] = reinterpret_cast<void *>(list[i]);
                }
                printf("[bench] pass-addrs: 已从 local 收到 %u 个目标地址, 首个=0x%llx\n", a.count,
                       static_cast<unsigned long long>(list[0]));
            } else {
                for (uint32_t i = 0; i < a.count; ++i) {
                    dsts[i] = HostPtr(peerGva + dispBase + i * a.stride);
                }
            }
            uint64_t sumUs = 0;
            std::vector<uint64_t> costs; /* 热循环不打日志，跑完后统一打印，避免日志影响计时/节奏 */
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            /* 轮次由 local 驱动：等本端 baseReqOff 的序号推进 → 写本轮 600 块 → 把完成标志单边写回 local。
               两端时序由这个握手对齐，所以不需要 RoundGap 做轮次隔离。 */
            auto *reqVa = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + baseReqOff));
            uint64_t lastSeq = 0;
            bool aborted = false;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t w0 = NowUs();
                while (*reqVa == lastSeq) {
                    if (NowUs() - w0 > kSpinTimeoutUs) {
                        printf("baseline sender TIMEOUT at iter %u: 等不到 local 的本轮请求\n", r);
                        aborted = true;
                        break;
                    }
                }
                if (aborted) {
                    break;
                }
                lastSeq = *reqVa;
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
                /* 600 块写完之后，在**同一条 channel** 上把完成标志写到 local：
                   QP 内保序 ⇒ 标志到 = 前面 600 块已全部落地，local 才能用它做单时钟端到端计时。 */
                *reinterpret_cast<uint64_t *>(HostPtr(selfGva + baseDoneOff)) = lastSeq;
                std::atomic_thread_fence(std::memory_order_release);
                smem_copy_params dp{HostPtr(selfGva + baseDoneOff), HostPtr(peerGva + baseDoneOff), sizeof(uint64_t),
                                    nullptr};
                ret = smem_bm_copy(bm, &dp, SMEMB_COPY_AUTO, 0);
                if (ret != 0) {
                    printf("baseline sender done-flag write failed at iter %u ret=%d\n", r, ret);
                    break;
                }
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t cost = t1 - t0;
                    sumUs += cost;
                    costs.push_back(cost);
                }
            }
            if (!costs.empty()) { /* 只打最后一轮 + 平均值，避免 100 轮刷屏 */
                printf("baseline sender last_round cost_us=%llu\n", static_cast<unsigned long long>(costs.back()));
            }
            printf("baseline sender avg_us=%llu (rounds=%u)\n",
                   static_cast<unsigned long long>(sumUs / a.rounds), a.rounds);
        } else {
            PrintLabel("baseline (observer)");
            std::vector<int> errs;
            std::vector<VerifyResult> verifies;
            /* 600 个目标地址固定，一次性解析供每轮的整块校验使用（GVA==VA，无转换开销） */
            std::vector<void *> dstVas(a.count, nullptr);
            for (uint32_t i = 0; i < a.count; ++i) {
                dstVas[i] = HostPtr(selfGva + dispBase + i * a.stride);
            }
            if (a.passAddrs) {
                /* 把这 600 个目标地址显式下发给 remote（不再依赖"双方按同一公式算出相同地址"）：
                   先写列表，再往本端 ackOff 槽写序号（同一 QP 保序），对端看到序号即列表可用。 */
                auto *list = reinterpret_cast<uint64_t *>(HostPtr(selfGva + addrListOff));
                for (uint32_t i = 0; i < a.count; ++i) {
                    list[i] = reinterpret_cast<uint64_t>(dstVas[i]);
                }
                std::atomic_thread_fence(std::memory_order_release);
                smem_copy_params lp{HostPtr(selfGva + addrListOff), HostPtr(peerGva + addrListOff),
                                    static_cast<size_t>(a.count) * sizeof(uint64_t), nullptr};
                int32_t lret = smem_bm_copy(bm, &lp, SMEMB_COPY_AUTO, 0);
                if (lret == 0) {
                    *reinterpret_cast<uint64_t *>(HostPtr(selfGva + ackOff)) = 1;
                    std::atomic_thread_fence(std::memory_order_release);
                    smem_copy_params sp{HostPtr(selfGva + ackOff), HostPtr(peerGva + ackOff), sizeof(uint64_t), nullptr};
                    lret = smem_bm_copy(bm, &sp, SMEMB_COPY_AUTO, 0);
                }
                if (lret != 0) {
                    printf("[bench] pass-addrs: 目标地址列表下发失败 ret=%d\n", lret);
                    smem_bm_destroy(bm);
                    return 1;
                }
                printf("[bench] pass-addrs: 已向 remote 下发 %u 个目标地址\n", a.count);
            }
            auto *doneVa = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + baseDoneOff));
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时） */
            uint64_t expect = 1;
            uint32_t timedOutAt = UINT32_MAX;
            uint64_t sumE2eUs = 0;
            std::vector<uint64_t> e2eCosts;
            for (uint32_t r = 0; r < kTotal; ++r) {
                /* 本端**驱动轮次并单时钟计时**：写本轮请求(推进 baseReqOff) → remote 写 600 块 →
                   remote 把完成标志写回本端 baseDoneOff。计时区 = 从写请求到看到完成标志。
                   baseline 不做 scatter（数据直写 600 个离散目标），所以这个 e2e 就是纯传输时长。 */
                const uint64_t t0 = NowUs();
                {
                    *reinterpret_cast<uint64_t *>(HostPtr(selfGva + baseReqOutOff)) = expect;
                    std::atomic_thread_fence(std::memory_order_release);
                    smem_copy_params rp{HostPtr(selfGva + baseReqOutOff), HostPtr(peerGva + baseReqOff),
                                        sizeof(uint64_t), nullptr};
                    const int32_t rret = smem_bm_copy(bm, &rp, SMEMB_COPY_AUTO, 0);
                    if (rret != 0) {
                        printf("baseline observer req write failed at iter %u ret=%d\n", r, rret);
                        timedOutAt = r;
                        break;
                    }
                }
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
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t e2eUs = t1 - t0;
                    sumE2eUs += e2eUs;
                    e2eCosts.push_back(e2eUs);
                }
                /* 计时区之外：整块校验本轮数据（身份 tag + memcmp 内容）。
                   轮次由握手对齐，校验期间对端不会开写下一轮，结果可信。 */
                VerifyResult vr = VerifyBlocks(dstVas, a.count, a.size, refBlock.data());
                if (r >= 1) {
                    errs.push_back(vr.ok ? 0 : 1);
                    verifies.push_back(vr);
                }
                ++expect;
            }
            if (timedOutAt != UINT32_MAX) {
                printf("baseline observer TIMEOUT at iter %u: done=%llu expect=%llu —— 对端没在推进完成标志"
                       "（两端 mode / 地址列表是否一致？）\n",
                       timedOutAt, static_cast<unsigned long long>(*doneVa), static_cast<unsigned long long>(expect));
            }
            for (uint32_t k = 0; k < verifies.size(); ++k) {
                if (!verifies[k].ok) {
                    PrintVerifyFail("baseline observer", k, verifies[k]);
                    break;
                }
            }
            if (!e2eCosts.empty()) { /* 只打最后一轮 + 平均值，避免 100 轮刷屏 */
                printf("baseline observer last_round e2e_us=%llu\n",
                       static_cast<unsigned long long>(e2eCosts.back()));
            }
            printf("baseline observer e2e_avg_us=%llu (rounds=%u)\n", static_cast<unsigned long long>(sumE2eUs / a.rounds),
                   a.rounds);
            printf("baseline observer verify=%s (rounds=%u, err 末轮=%d)\n",
                   std::all_of(verifies.begin(), verifies.end(), [](const VerifyResult &v) { return v.ok; }) ? "OK"
                                                                                                            : "FAIL",
                   a.rounds, errs.empty() ? 0 : errs.back());
        }
    }

    /* 场景 2：cont —— 进度通知由 smem_bm_copy_batch **接口内部**完成：每 a.chunk 个 IO 拷完，
       接口自动向对端水位槽单边写一次 (progressBase + 已完成个数)；local 见水位推进就增量 scatter。
       应用只填参数、只调一次。
       目的地址由 local 在**每轮请求里**显式给出；整轮流程（local 发请求 → remote 写 → local 边收边散）
       全在计时区内，计时在 local 侧用单时钟完成，不需要任何回传信号。 */
    if (runCont) {
        if (!isLocal) {
            PrintLabel("cont (sender: 等 local 请求 → 投数据；不计时)");
            std::vector<void *> srcs(a.count), dsts(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                srcs[i] = HostPtr(selfGva + i * a.stride); /* 离散源 */
            }
            const uint32_t chunks = (a.count + a.chunk - 1) / a.chunk;
            uint64_t sumTransportUs = 0;
            std::vector<uint64_t> transportCosts;
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时） */
            /* 本轮是否开始由 local 决定：轮询请求槽的轮次号（[8]），看到推进就按 [0] 给的
               staging 基址投本轮数据。发端不参与计时（计时在 local 侧单时钟完成）。 */
            auto *reqSeqVa = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + reqOff + 8ULL));
            auto *reqStagingVa = reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + reqOff));
            uint64_t lastSeq = 0;
            bool aborted = false;
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t w0 = NowUs();
                while (*reqSeqVa == lastSeq) {
                    if (NowUs() - w0 > kSpinTimeoutUs) {
                        printf("cont sender TIMEOUT at iter %u: 等不到 local 的请求\n", r);
                        aborted = true;
                        break;
                    }
                }
                if (aborted) {
                    break;
                }
                lastSeq = *reqSeqVa;
                const uint64_t stagingBase = *reqStagingVa; /* local 给的 staging 基址 */
                for (uint32_t i = 0; i < a.count; ++i) {
                    dsts[i] = HostPtr(stagingBase + i * a.size);
                }
                smem_batch_copy_params p{};
                p.sources = srcs.data();
                p.destinations = dsts.data();
                p.dataSizes = sizes.data();
                p.batchSize = a.count;
                p.progressSrc = HostPtr(selfGva + scratchOff);       /* 本端进度源槽数组（每个 flag 一格） */
                p.progressDest = HostPtr(peerGva + flagOff);         /* 对端水位槽（收端轮询的那个 8B） */
                p.progressBase = static_cast<uint64_t>(r) * a.count; /* 跨轮单调、不归零 */
                p.progressInterval = a.chunk;
                const uint64_t t0 = NowUs();
                const int32_t ret = smem_bm_copy_batch(bm, &p, SMEMB_COPY_AUTO, 0);
                const uint64_t tW = NowUs();
                if (ret != 0) {
                    printf("cont sender abort at iter %u ret=%d\n", r, ret);
                    aborted = true;
                    break;
                }
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    sumTransportUs += tW - t0;
                    transportCosts.push_back(tW - t0);
                }
                RoundGap(a.gapMs);
            }
            if (!transportCosts.empty()) { /* 只打最后一轮 + 平均值，避免 100 轮刷屏 */
                printf("cont sender last_round transport_us=%llu\n",
                       static_cast<unsigned long long>(transportCosts.back()));
            }
            printf("cont sender transport_avg_us=%llu chunks=%u interval=%u (rounds=%u)\n",
                   static_cast<unsigned long long>(sumTransportUs / a.rounds), chunks, a.chunk, a.rounds);
            printf("cont sender note: 这里只是投递段耗时；完整端到端由 local 侧单时钟测（见 receiver）\n");
        } else {
            PrintLabel("cont (receiver: 发请求 → 等水位 scatter；本轮 e2e 在此单时钟计)");
            uint64_t sumScatterUs = 0;
            uint64_t sumTailUs = 0;
            uint64_t sumE2eUs = 0; /* 本轮完整端到端：从"发出请求"到"600 块全部散完" */
            uint64_t sumReqUs = 0; /* 其中"发出请求"这一段（local 的一次单边写）耗时 */
            /* staging / 离散目标地址固定，一次性解析（GVA==VA，无每轮转换开销） */
            std::vector<void *> srcVas(a.count), dstVas(a.count);
            for (uint32_t i = 0; i < a.count; ++i) {
                srcVas[i] = HostPtr(selfGva + i * a.size);
                dstVas[i] = HostPtr(selfGva + dispBase + i * a.stride);
            }
            const uint32_t kTotal = a.rounds + 1; /* 第 1 轮 warmup（不计时），后 a.rounds 轮计时 */
            /* 多链路：每条 link 一段【连续】iov + 一条水位槽，分区规则与库侧一致
               （base = count/K, rem = count%K, link e 拿 base + (e<rem?1:0) 个块），
               水位值是【全局块号】，收端按 link 各自增量 scatter，全部 link 到位才算本轮结束。 */
            std::vector<volatile uint64_t *> wmVas(links);
            for (uint32_t e = 0; e < links; ++e) {
                wmVas[e] =
                    reinterpret_cast<volatile uint64_t *>(HostPtr(selfGva + wmOff + static_cast<uint64_t>(e) * 8ULL));
            }
            std::vector<uint64_t> donePerEp(links, 0);
            std::vector<uint64_t> endPerEp(links, 0);
            uint64_t expect = 1; /* 与发端 seq 对齐，用于 ack 值 */
            std::vector<uint64_t> scatterCosts, tailCosts; /* 跑完后统一打印 */
            std::vector<uint64_t> e2eCosts, reqCosts;
            std::vector<std::string> arrivalLines;         /* 各批数据可见时刻(相对本轮起点,us)，用于确认重叠 */
            bool aborted = false;                          /* 中途超时/出错则不再校验 */
            for (uint32_t r = 0; r < kTotal; ++r) {
                const uint64_t roundStart = static_cast<uint64_t>(r) * a.count;
                { /* 本轮每条 link 负责的块区间（全局块号） */
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
                /* 本轮由 local 发起：先"发消息"（把自己的 staging 基址 + 轮次号单边写给 remote），
                   然后等水位、边到边散。计时从**发请求之前**开始，到**600 块全部散完**结束 ——
                   即你说的完整流程（local 发消息 → remote 写 → local scatter）都在计时范围内，
                   而且是 local 侧单时钟，不需要任何回传信号。 */
                const uint64_t tRoundStart = NowUs();
                {
                    auto *out = reinterpret_cast<uint64_t *>(HostPtr(selfGva + reqOutOff));
                    out[0] = selfGva;  /* staging 基址（跨轮不变） */
                    out[1] = expect;   /* 轮次号：写在最后，对端以它判断"新的一轮来了" */
                    std::atomic_thread_fence(std::memory_order_release);
                    smem_copy_params rq{HostPtr(selfGva + reqOutOff), HostPtr(peerGva + reqOff),
                                        sizeof(uint64_t) * 2, nullptr};
                    const int32_t reqRet = smem_bm_copy(bm, &rq, SMEMB_COPY_AUTO, 0);
                    const uint64_t tReqDone = NowUs();
                    if (reqRet != 0) {
                        printf("cont receiver req write failed at iter %u ret=%d\n", r, reqRet);
                        aborted = true;
                        break;
                    }
                    if (r >= 1) {
                        sumReqUs += tReqDone - tRoundStart;
                        reqCosts.push_back(tReqDone - tRoundStart);
                    }
                }
                const uint64_t tr0 = NowUs(); /* 本轮起点：开始等第一份水位 */
                uint64_t scatterUs = 0;       /* 本轮 scatter 累计耗时（只算 memcpy，不含轮询等待） */
                uint64_t tailUs = 0;          /* 最后一批的 scatter 耗时 */
                std::string arrivals;
                bool timedOut = false;
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
                            tailUs = tb1 - tb0; /* 收尾批的 scatter 即发端投完之后的"尾巴" */
                            arrivals += (arrivals.empty() ? "" : ",") + std::to_string(tb0 - tr0);
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
                    printf("cont receiver TIMEOUT at iter %u: 对端没在推进水位（检查两端 links 数/mode 是否一致、swap 是否已关）\n",
                           r);
                    aborted = true;
                    break;
                }
                /* 600 块全部散完 → 本轮结束、停表。发请求 + 传输 + scatter 全在计时区内，
                   不需要任何回传信号（这就是"完整流程都计时"）。
                   校验不在这里做，改为所有轮次跑完后只做一次（见循环之后）。 */
                if (r >= 1) { /* r==0 为 warmup，不计入 */
                    const uint64_t e2eUs = NowUs() - tRoundStart;
                    sumE2eUs += e2eUs;
                    e2eCosts.push_back(e2eUs);
                    sumScatterUs += scatterUs;
                    sumTailUs += tailUs;
                    scatterCosts.push_back(scatterUs);
                    tailCosts.push_back(tailUs);
                    arrivalLines.push_back(arrivals);
                }
                RoundGap(a.gapMs);
                ++expect;
            }
            if (!scatterCosts.empty()) { /* 只打最后一轮 + 平均值，避免 100 轮刷屏 */
                printf("cont receiver last_round e2e_us=%llu req_us=%llu scatter_us=%llu tail_us=%llu arrivals_us=[%s]\n",
                       static_cast<unsigned long long>(e2eCosts.back()),
                       static_cast<unsigned long long>(reqCosts.back()),
                       static_cast<unsigned long long>(scatterCosts.back()),
                       static_cast<unsigned long long>(tailCosts.back()), arrivalLines.back().c_str());
            }
            /* 校验只做一次（所有轮次跑完之后）：此时没有任何后续写入，staging 与离散目标都是最后一轮的
               真实内容，结果可信；而它不再占用每轮关键路径，不影响 e2e 口径。 */
            if (!aborted) {
                const uint64_t tv0 = NowUs();
                VerifyResult vStag = VerifyBlocks(srcVas, a.count, a.size, refBlock.data());
                VerifyResult vDisp = VerifyBlocks(dstVas, a.count, a.size, refBlock.data());
                const uint64_t verifyUs = NowUs() - tv0;
                if (!vStag.ok) {
                    PrintVerifyFail("cont receiver staging", 0, vStag);
                }
                if (!vDisp.ok) {
                    PrintVerifyFail("cont receiver scattered", 0, vDisp);
                }
                printf("cont receiver one-shot verify=%s (staging+scattered, 末轮, %lluus)\n",
                       (vStag.ok && vDisp.ok) ? "OK" : "FAIL", static_cast<unsigned long long>(verifyUs));
            }
            printf("cont receiver e2e_avg_us=%llu req_avg_us=%llu scatter_avg_us=%llu tail_avg_us=%llu (rounds=%u)\n",
                   static_cast<unsigned long long>(sumE2eUs / a.rounds),
                   static_cast<unsigned long long>(sumReqUs / a.rounds),
                   static_cast<unsigned long long>(sumScatterUs / a.rounds),
                   static_cast<unsigned long long>(sumTailUs / a.rounds), a.rounds);
        }
    }

    smem_bm_leave(bm, 0);
    smem_bm_destroy(bm);
    smem_bm_uninit(0);
    printf("[bench] done\n");
    return 0;
}
