// SPDX-License-Identifier: MulanPSL-2.0
#include "hostrdma_trace.h"
#include "hcom_rdma_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <time.h>

namespace mf_trace {
namespace {
using Event = ock::hcom::UBSHcomRdmaTraceEvent;
using Kind = ock::hcom::UBSHcomRdmaTraceKind;
using Hooks = ock::hcom::UBSHcomRdmaTraceHooks;
using Configure = int (*)(const Hooks *, uint32_t) noexcept;
constexpr size_t kThreads = 16;
constexpr size_t kMaxCapacity = 65536;
struct alignas(128) Buffer {
    std::unique_ptr<Event[]> events;
    size_t size = 0;
    size_t dropped = 0;
};
struct AppEvent {
    const char *name;
    uint64_t timestamp, round, value;
    int32_t rail, status;
    uint32_t from, upto;
};
std::array<Buffer, kThreads> buffers;
std::unique_ptr<AppEvent[]> appEvents;
std::atomic<uint64_t> epoch{0};
std::atomic<size_t> nextThread{0}, unregistered{0};
size_t capacity = 0, appSize = 0, appDropped = 0;
uint32_t warmupRounds = 0, measuredRounds = 0;
uint64_t payloadBytes = 0, peer = 0, watermark = 0, message = 0;
uint32_t railCount = 0;
bool enabled = false, failed = false, remote = false;
std::string role;
void *library = nullptr;
Configure configure = nullptr;

uint64_t Now()
{
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

uint64_t Epoch() { return epoch.load(std::memory_order_acquire); }

void Record(const Event &event)
{
    // Only the first event of each thread claims a slot. No allocations/printing
    // or shared write counters on the steady-state path.
    thread_local const size_t slot = nextThread.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kThreads) {
        unregistered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto &buffer = buffers[slot];
    if (buffer.size == capacity) {
        ++buffer.dropped;
        return;
    }
    buffer.events[buffer.size++] = event;
}
const Hooks hooks{Epoch, Record};

const char *Name(Kind kind)
{
    switch (kind) {
        case Kind::POST_BEGIN: return "verbs_post_begin";
        case Kind::POST_END: return "verbs_post_end";
        case Kind::CQ_POLL_BATCH: return "cq_poll_batch";
        case Kind::CQE_OBSERVED: return "cqe_observed";
        case Kind::CQ_DISPATCH_BEGIN: return "cq_dispatch_begin";
        case Kind::CQ_DISPATCH_END: return "cq_dispatch_end";
        default: return "other";
    }
}

const char *Transfer(const Event &e)
{
    if (e.kind != Kind::POST_BEGIN && e.kind != Kind::POST_END) return "unknown";
    if (remote && e.remoteAddress >= peer && e.remoteAddress - peer < payloadBytes) return "data";
    if (remote && e.remoteAddress >= watermark && e.remoteAddress - watermark < railCount * 8ULL)
        return "watermark";
    if (!remote && e.remoteAddress == message) return "request";
    return "other";
}

void PrintEvent(const Event &e, size_t thread)
{
    std::cout << "{\"record_type\":\"hcom_trace\",\"trace_schema\":\"mf-rdma-completion-v1\",\"host_role\":\""
              << role << "\",\"event\":\"" << Name(e.kind) << "\",\"timestamp_ns\":" << e.timestampNs
              << ",\"capture_round\":" << e.epoch << ",\"thread_slot\":" << thread
              << ",\"transfer_kind\":\"" << Transfer(e) << "\",\"wr_id\":\"0x" << std::hex << e.wrId
              << "\",\"cq_id\":\"0x" << e.cqId << "\",\"local_address\":\"0x" << e.localAddress
              << "\",\"remote_address\":\"0x" << e.remoteAddress << std::dec << "\",\"qp_num\":" << e.qpNum
              << ",\"batch_id\":" << e.batchId << ",\"opcode\":" << e.opcode << ",\"status\":" << e.status
              << ",\"count\":" << e.count << ",\"sge_count\":" << e.sgeCount << ",\"bytes\":" << e.bytes
              << ",\"poll_begin_ns\":" << e.pollBeginNs << ",\"previous_poll_end_ns\":" << e.previousPollEndNs
              << ",\"empty_polls\":" << e.emptyPolls << ",\"max_poll_gap_ns\":" << e.maxPollGapNs
              << ",\"max_poll_call_ns\":" << e.maxPollCallNs << "}\n";
}

void PrintApp(const AppEvent &e)
{
    std::cout << "{\"record_type\":\"mf_app_trace\",\"host_role\":\"" << role << "\",\"event\":\"" << e.name
              << "\",\"timestamp_ns\":" << e.timestamp << ",\"capture_round\":" << e.round
              << ",\"rail\":" << e.rail << ",\"from\":" << e.from << ",\"upto\":" << e.upto
              << ",\"value\":" << e.value << ",\"status\":" << e.status << "}\n";
}

bool LoadHooks()
{
    // Same library name and namespace used by MF's DlHcomApi. Do this BEFORE MF
    // starts workers, and retain the handle until all workers have joined.
    library = dlopen("libhcom.so", RTLD_NOW | RTLD_NODELETE);
    if (library == nullptr) {
        fprintf(stderr, "ERROR: trace cannot load libhcom.so: %s\n", dlerror());
        return false;
    }
    configure = reinterpret_cast<Configure>(dlsym(library, "UBSHcomRdmaTraceConfigureV1"));
    if (configure == nullptr) {
        fprintf(stderr, "ERROR: loaded libhcom.so lacks UBSHcomRdmaTraceConfigureV1; rebuild patched ubs-comm\n");
        return false;
    }
    Dl_info info{};
    dladdr(reinterpret_cast<void *>(configure), &info);
    std::cout << "{\"record_type\":\"mf_trace_library\",\"path\":"
              << std::quoted(info.dli_fname ? info.dli_fname : "unknown") << "}\n";
    return configure(&hooks, sizeof(Event)) == 0;
}
} // namespace

bool Initialize(bool requested, const char *hostRole, uint32_t count, uint64_t size,
                uint32_t rounds, uint32_t warmup, uint32_t chunk, uint64_t stride)
{
    if (!requested) return true;
    role = hostRole;
    remote = role == "remote";
    warmupRounds = warmup;
    measuredRounds = rounds;
    payloadBytes = count * size;
    // Worst-case estimate uses one WR per block; overflow is explicit, never a
    // silently truncated successful trace. Memory is touched before workers run.
    capacity = static_cast<size_t>(std::min<uint64_t>(kMaxCapacity,
        std::max<uint64_t>(4096, static_cast<uint64_t>(count) * rounds * 12 + 1024)));
    try {
        for (auto &buffer : buffers) buffer.events.reset(new Event[capacity]());
        appEvents.reset(new AppEvent[capacity]());
    } catch (const std::bad_alloc &) {
        fprintf(stderr, "ERROR: trace buffer allocation failed, capacity=%zu threads=%zu\n", capacity, kThreads);
        return false;
    }
    if (!LoadHooks()) return false;
    enabled = true;
    std::cout << "{\"record_type\":\"mf_trace_config\",\"host_role\":\"" << role << "\",\"count\":" << count
              << ",\"size\":" << size << ",\"stride\":" << stride << ",\"chunk\":" << chunk
              << ",\"warmup\":" << warmup << ",\"rounds\":" << rounds << ",\"clock\":\"CLOCK_MONOTONIC_RAW\""
              << ",\"cqe_time_basis\":\"poll-observation-not-hardware-completion\"}\n";
    return true;
}

void SetLayout(uint64_t peerBase, uint64_t watermarkOffset, uint64_t messageOffset, uint32_t rails)
{
    peer = peerBase;
    watermark = peerBase + watermarkOffset;
    message = peerBase + messageOffset;
    railCount = rails;
}

void BeginRound(uint32_t round)
{
    if (enabled && round >= warmupRounds) epoch.store(static_cast<uint64_t>(round) + 1, std::memory_order_release);
}

void EndRound() { epoch.store(0, std::memory_order_release); }

void Mark(const char *event, int32_t rail, uint32_t from, uint32_t upto, uint64_t value, int32_t status)
{
    const uint64_t round = Epoch();
    if (round == 0) return;
    if (appSize == capacity) {
        ++appDropped;
        return;
    }
    appEvents[appSize++] = {event, Now(), round, value, rail, status, from, upto};
}

void Fail() { failed = true; }

bool Finish()
{
    if (!enabled) return true;
    EndRound();
    configure(nullptr, sizeof(Event)); // caller has stopped/joined HCOM workers
    size_t dropped = unregistered.load() + appDropped, clocks = 0, errors = 0, posts = 0, cqes = 0;
    size_t finishedRounds = 0;
    uint64_t dataBytes = 0;
    for (size_t t = 0; t < kThreads; ++t) {
        const auto &buffer = buffers[t];
        dropped += buffer.dropped;
        for (size_t i = 0; i < buffer.size; ++i) {
            const auto &e = buffer.events[i];
            clocks += e.timestampNs == 0;
            errors += e.status != 0;
            posts += e.kind == Kind::POST_END;
            cqes += e.kind == Kind::CQE_OBSERVED;
            if (e.kind == Kind::POST_END && std::string(Transfer(e)) == "data") dataBytes += e.bytes;
            PrintEvent(e, t);
        }
    }
    for (size_t i = 0; i < appSize; ++i) {
        clocks += appEvents[i].timestamp == 0;
        errors += appEvents[i].status != 0;
        finishedRounds += std::string(appEvents[i].name) == (remote ? "copy_batch_end" : "local_round_end");
        PrintApp(appEvents[i]);
    }
    const bool ok = !failed && dropped == 0 && clocks == 0 && errors == 0 && posts != 0 && cqes != 0 &&
                    finishedRounds == measuredRounds && (!remote || dataBytes == payloadBytes * measuredRounds);
    std::cout << "{\"record_type\":\"mf_trace_summary\",\"status\":\"" << (ok ? "ok" : "incomplete")
              << "\",\"dropped\":" << dropped << ",\"clock_errors\":" << clocks << ",\"event_errors\":" << errors
              << ",\"post_records\":" << posts << ",\"cqe_records\":" << cqes << ",\"data_bytes\":" << dataBytes
              << ",\"thread_count\":" << nextThread.load() << ",\"capacity_per_thread\":" << capacity << "}\n";
    if (!ok) {
        fprintf(stderr, "ERROR: incomplete MF trace: dropped=%zu clock_errors=%zu event_errors=%zu rounds=%zu/%u\n",
                dropped, clocks, errors, finishedRounds, measuredRounds);
    }
    dlclose(library);
    return ok;
}
} // namespace mf_trace
