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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include <string>

#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "acc_offload.h"

namespace py = pybind11;

void DefineAccOffloadConfig(py::module_ &m)
{
    py::enum_<offload_scene_t>(m, "Scene")
        .value("LOCAL", OFFLOAD_SCENE_LOCAL)
        .value("SHARED", OFFLOAD_SCENE_SHARED)
        .export_values();

    py::class_<offload_config_t>(m, "OffloadConfig")
        .def(py::init<>())
        .def_readwrite("device_id", &offload_config_t::deviceId)
        .def_readwrite("reserve_size", &offload_config_t::reserveSize,
                       "Reserved DRAM pool size in bytes, will be aligned up to GB. SHARED: must be identical on "
                       "every rank (pool-wide slot stride)")
        .def_readwrite("alloc_size", &offload_config_t::allocSize,
                       "Allocated local physical DRAM size in bytes, will be aligned up to GB. LOCAL: must equal "
                       "reserve_size; SHARED: this rank's own contribution (<= reserve_size). Ranks MAY pass "
                       "different values; scenarios that need equal allocations must pass equal values themselves — "
                       "equality is caller-guaranteed, not validated")
        .def_readwrite("world_size", &offload_config_t::worldSize,
                       "number of ranks in the group (multi-card shared mode): the GLOBAL rank total across all "
                       "machines of the pool")
        .def_readwrite("rank_id", &offload_config_t::rankId,
                       "GLOBAL rank id in the group, 0 is the server (multi-card shared mode); slot = global rank")
        .def_readwrite("scene", &offload_config_t::scene,
                       "memory pool scene: LOCAL=single-card, SHARED=multi-card shared")
        .def_property(
            "store_url", [](const offload_config_t &config) { return std::string(config.storeUrl); },
            [](offload_config_t &config, const std::string &url) {
                if (url.size() >= sizeof(config.storeUrl)) {
                    throw py::value_error("store_url too long, max 63 bytes, url: " + url);
                }
                url.copy(config.storeUrl, url.size());
                config.storeUrl[url.size()] = '\0';
            },
            "Explicit config store url of the shared pool (e.g. tcp://127.0.0.1:8500, etcd://..., reg://...), "
            "identical across ranks of one group; resolution order: this field > ASCEND_MF_STORE_URL env > derived "
            "port 8500 + device_id // local_world_size (single machine only, requires contiguous device ids aligned "
            "to world_size)")
        .def_readwrite("local_world_size", &offload_config_t::localWorldSize,
                       "ranks of this machine within the pool; 0 = world_size (single machine, backward compatible). "
                       "Must divide world_size; machine_rank = rank_id // local_world_size. A multi-machine pool "
                       "(< world_size) currently accepts A3 (Ascend910C) nodes only")
        .def_readwrite("flags", &offload_config_t::flags, "optional flags, see OFFLOAD_FLAG_xxx");
    m.attr("OFFLOAD_FLAG_GIANT_PAGE") = py::int_(OFFLOAD_FLAG_GIANT_PAGE);
    ;
}

void DefineAccOffloadApi(py::module_ &m)
{
    m.def("initialize", &offload_init, py::call_guard<py::gil_scoped_release>(), py::arg("config"));

    m.def("uninitialize", &offload_uninit, py::call_guard<py::gil_scoped_release>());

    m.def("malloc", &offload_malloc, py::call_guard<py::gil_scoped_release>(), py::arg("size"), py::arg("flags") = 0);

    m.def("free", &offload_free, py::call_guard<py::gil_scoped_release>(), py::arg("ptr"), py::arg("flags") = 0);

    m.def("sparse_copy", &offload_sparse_copy, py::call_guard<py::gil_scoped_release>(), py::arg("srcPtrs"),
          py::arg("dstPtrs"), py::arg("lenPtrs"), py::arg("sizePtr"), py::arg("deviceId"));

    m.def("sparse_copy_host_rdma",
          [](const py::object &handle, const std::vector<uint64_t> &sources,
             const std::vector<uint64_t> &destinations, uint64_t bytes) {
              if (sources.empty() || sources.size() != destinations.size() || sources.size() > UINT32_MAX) {
                  throw py::value_error("source/destination counts must match and be in [1, UINT32_MAX]");
              }
              // Keep the Python BM owner alive, but never depend on its private C++ class ABI.
              py::capsule capsule = handle.attr("_native_handle");
              void *native = PyCapsule_GetPointer(capsule.ptr(), "memfabric.smem_bm_t");
              if (native == nullptr) { throw py::error_already_set(); }
              py::gil_scoped_release release;
              return offload_sparse_copy_host_rdma(native, sources.data(), destinations.data(),
                                                   static_cast<uint32_t>(sources.size()), bytes);
          }, py::arg("handle"), py::arg("src_addrs"), py::arg("dst_addrs"), py::arg("block_bytes"),
          R"(Synchronous CPU sparse copy, implemented by acc_offload. Sources are rank 1 BM DRAM GVAs;
targets are rank 0 BM DRAM GVAs. Call handle.prepare_host_rdma_sparse on both ranks first.
Rank 1 must call handle.poll_host_rdma_sparse; preparation creates no polling thread.
Returns 0 on success. No offload.initialize or NPU required.)");

    m.def("host_rdma_sparse_last_timing", [](const py::object &handle) {
        py::capsule capsule = handle.attr("_native_handle");
        void *native = PyCapsule_GetPointer(capsule.ptr(), "memfabric.smem_bm_t");
        if (native == nullptr) { throw py::error_already_set(); }
        offload_host_rdma_sparse_timing_v2_t timing{};
        int32_t ret;
        {
            py::gil_scoped_release release;
            ret = offload_host_rdma_sparse_last_timing_v2(native, &timing);
        }
        if (ret != 0) { throw std::runtime_error("no successful HOST_RDMA sparse timing, ret=" + std::to_string(ret)); }
        py::dict result;
        result["sequence"] = timing.sequence;
        result["request_ns"] = timing.requestNs;
        result["gather_ns"] = timing.gatherNs;
        result["write_ns"] = timing.writeNs;
        result["scatter_ns"] = timing.scatterNs;
        result["wait_remote_ns"] = timing.waitRemoteNs;
        result["receive_scatter_ns"] = timing.receiveScatterNs;
        result["gather_write_ns"] = timing.gatherWriteNs;
        return result;
    }, py::arg("handle"), "Local stage times (ns) of the last successful copy/poll; read before the next request.");

    m.def("group_pack_copy", &offload_group_pack_copy, py::call_guard<py::gil_scoped_release>(), py::arg("srcPtrs"),
          py::arg("dstPtrs"), py::arg("lenPtrs"), py::arg("numLocalExpertPtr"), py::arg("groupList"),
          py::arg("packedGroupList"), py::arg("deviceId"));

    m.def("kv_exchange_copy", &offload_kv_exchange_copy, py::call_guard<py::gil_scoped_release>(), py::arg("metaPtr"),
          py::arg("deviceId"));

    m.def(
        "get_dva",
        [](uint64_t hostPtr) -> uint64_t {
            uint64_t dva = 0;
            if (offload_get_dva(hostPtr, &dva) != 0) {
                return 0;
            }
            return dva;
        },
        py::call_guard<py::gil_scoped_release>(), py::arg("ptr"),
        "returns the device virtual address (DVA) of an offload malloc address, 0 on failure");

    m.def(
        "register_entry_table",
        [](uint32_t entryBytes, uint32_t rowsPerSlot) -> int32_t {
            return offload_register_entry_table(entryBytes, rowsPerSlot);
        },
        py::call_guard<py::gil_scoped_release>(), py::arg("entryBytes"), py::arg("rowsPerSlot"),
        "register the pool's single uniform row-grid layout (row pitch / rows per slot across all segments; callers "
        "with multiple same-width tables fold their per-segment offsets into the ids); returns 0 on success, negative "
        "on failure");

    m.def(
        "entry_gather",
        [](uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint16_t deviceId) {
            return offload_entry_gather(dstPtr, idsPtr, countPtr, deviceId);
        },
        py::call_guard<py::gil_scoped_release>(), py::arg("dstPtr"), py::arg("idsPtr"), py::arg("countPtr"),
        py::arg("deviceId"),
        "fused random-row gather over the registered uniform grid: GLOBAL row ids -> pool GVA conversion in-kernel "
        "(tail gaps strided over), results packed at dst + i*entryBytes; the entry count is read from device memory at "
        "countPtr (graph-capture safe)");
}

PYBIND11_MODULE(_pymf_acc_offload, m)
{
    auto offload = m.def_submodule("offload", "Acc Offload Module.");

    DefineAccOffloadConfig(offload);
    DefineAccOffloadApi(offload);
}

#pragma GCC diagnostic pop
