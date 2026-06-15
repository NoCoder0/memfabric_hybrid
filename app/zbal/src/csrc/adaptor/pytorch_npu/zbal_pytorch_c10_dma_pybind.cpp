/*
* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
* ZBAL is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
* See the Mulan PSL v2 for more details.
*/

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "zbal_pytorch_c10_dma.h"
#include "zbal_pytorch_c10_dma_pybind.h"

namespace py = pybind11;

py::dict dma_dump_snapshot()
{
    using c10_npu::dma::BlockInfo;
    using c10_npu::dma::SegmentInfo;

    py::str device_s = "device";
    py::str address_s = "address";
    py::str total_size_s = "total_size";
    py::str allocated_size_s = "allocated_size";
    py::str active_size_s = "active_size";
    py::str requested_size_s = "requested_size";
    py::str stream_s = "stream";
    py::str segment_type_s = "segment_type";
    py::str segment_pool_id = "segment_pool_id";
    py::str large_s = "large";
    py::str small_s = "small";
    py::str size_s = "size";
    py::str state_s = "state";
    py::str active_allocated_s = "active_allocated";
    py::str active_pending_free_s = "active_pending_free";
    py::str inactive_s = "inactive";
    py::str addr_s = "addr";
    py::str cpp_frames_s = "cpp_frames";
    py::str blocks_s = "blocks";
    py::str is_expandable_s = "is_expandable";
    py::str frames_s = "frames";

    py::list empty_frames;

    const auto segmentInfoToDict = [&](const SegmentInfo &segmentInfo) {
        py::dict segmentDict;
        segmentDict[device_s] = segmentInfo.device;
        segmentDict[address_s] = segmentInfo.address;
        segmentDict[total_size_s] = segmentInfo.total_size;
        segmentDict[allocated_size_s] = segmentInfo.allocated_size;
        segmentDict[active_size_s] = segmentInfo.active_size;
        segmentDict[requested_size_s] = segmentInfo.requested_size;
        segmentDict[stream_s] = int64_t(segmentInfo.stream);
        segmentDict[segment_pool_id] = segmentInfo.owner_private_pool_id;
        segmentDict[segment_type_s] = (segmentInfo.is_large ? large_s : small_s);
        segmentDict[is_expandable_s] = segmentInfo.is_expandable;
        segmentDict[frames_s] = empty_frames;

        auto address = segmentInfo.address;
        py::list blocks;
        for (const auto &blockInfo : segmentInfo.blocks) {
            py::dict blockDict;
            blockDict[address_s] = address;
            blockDict[size_s] = blockInfo.size;
            blockDict[requested_size_s] = blockInfo.requested_size;
            blockDict[state_s] =
                (blockInfo.allocated ? active_allocated_s : (blockInfo.active ? active_pending_free_s : inactive_s));
            blockDict[frames_s] = empty_frames;
            blocks.append(blockDict);
            address += blockInfo.size;
        }
        segmentDict[blocks_s] = blocks;

        return segmentDict;
    };

    auto snapshot = c10_npu::dma::getCachingAllocator().snapshot();
    py::list segments;

    for (const auto &segmentInfo : snapshot.segments) {
        segments.append(segmentInfoToDict(segmentInfo));
    }

    py::list traces;
    py::str action_s = "action";
    py::str alloc_s = "alloc";
    py::str free_requested_s = "free_requested";
    py::str free_completed_s = "free_completed";
    py::str segment_alloc_s = "segment_alloc";
    py::str segment_free_s = "segment_free";
    py::str segment_map_s = "segment_map";
    py::str segment_unmap_s = "segment_unmap";
    py::str empty_cache_s = "empty_cache";

    py::str snapshot_s = "snapshot";
    py::str workspace_snapshot_s = "workspace_snapshot";
    py::str oom_s = "oom";
    py::str device_free_s = "device_free";

    using namespace c10_npu::dma;

    auto action_to_str = [&](TraceEntry::Action action) {
        switch (action) {
            case TraceEntry::ALLOC:
                return alloc_s;
            case TraceEntry::FREE_REQUESTED:
                return free_requested_s;
            case TraceEntry::FREE_COMPLETED:
                return free_completed_s;
            case TraceEntry::SEGMENT_ALLOC:
                return segment_alloc_s;
            case TraceEntry::SEGMENT_FREE:
                return segment_free_s;
            case TraceEntry::OOM:
                return oom_s;
            case TraceEntry::SNAPSHOT:
                return snapshot_s;
            case TraceEntry::WORKSPACE_SNAPSHOT:
                return workspace_snapshot_s;
            case TraceEntry::SEGMENT_UNMAP:
                return segment_unmap_s;
            case TraceEntry::SEGMENT_MAP:
                return segment_map_s;
            case TraceEntry::EMPTY_CACHE:
                return empty_cache_s;
            default:
                TORCH_INTERNAL_ASSERT(false, "[E]symm allocator snapshot invalid TraceEntry action");
        }
    };

    for (const auto &traceInfo : snapshot.device_traces) {
        py::list trace;
        for (const auto &te : traceInfo) {
            py::dict trace_entry;
            trace_entry[action_s] = action_to_str(te.action_);
            trace_entry[te.action_ == TraceEntry::OOM ? device_free_s : addr_s] = te.addr_;
            trace_entry[size_s] = te.size_;
            trace_entry[stream_s] = int64_t(te.stream_);
            trace.append(trace_entry);
        }
        traces.append(trace);
    }

    py::dict result;
    result["segments"] = segments;
    result["device_traces"] = traces;

    return result;
}