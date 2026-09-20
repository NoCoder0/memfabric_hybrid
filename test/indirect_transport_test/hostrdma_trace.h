// SPDX-License-Identifier: MulanPSL-2.0
#ifndef MF_HOSTRDMA_TRACE_H
#define MF_HOSTRDMA_TRACE_H

#include <cstdint>

namespace mf_trace {
// Configure before smem initialization; Finish only after smem uninitialization.
bool Initialize(bool enabled, const char *role, uint32_t count, uint64_t size,
                uint32_t rounds, uint32_t warmup, uint32_t chunk, uint64_t stride);
void SetLayout(uint64_t peerBase, uint64_t watermarkOffset, uint64_t messageOffset, uint32_t rails);
void BeginRound(uint32_t round);
void EndRound();
void Mark(const char *event, int32_t rail = -1, uint32_t from = 0, uint32_t upto = 0,
          uint64_t value = 0, int32_t status = 0);
void Fail();
bool Finish();
} // namespace mf_trace
#endif
