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
#include "acc_offload_entry_manager.h"

namespace py = pybind11;
using ock::offload::AccOffloadEntryManager;

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
}

void DefineAccOffloadApi(py::module_ &m)
{
    m.def("initialize", &offload_init, py::call_guard<py::gil_scoped_release>(), py::arg("config"));

    m.def("uninitialize", &offload_uninit, py::call_guard<py::gil_scoped_release>());

    m.def("malloc", &offload_malloc, py::call_guard<py::gil_scoped_release>(), py::arg("size"), py::arg("flags") = 0);

    m.def("free", &offload_free, py::call_guard<py::gil_scoped_release>(), py::arg("ptr"), py::arg("flags") = 0);

    m.def("sparse_copy", &offload_sparse_copy, py::call_guard<py::gil_scoped_release>(), py::arg("srcPtrs"),
          py::arg("dstPtrs"), py::arg("lenPtrs"), py::arg("sizePtr"), py::arg("deviceId"));

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

void DefineAccOffloadInstanceCopyApi(py::class_<AccOffloadEntryManager> &cls)
{
    cls.def(
        "sparse_copy",
        [](AccOffloadEntryManager &self, uint64_t src, uint64_t dst, uint64_t len, uint64_t size, uint16_t device) {
            return self.SparseCopy(reinterpret_cast<uint64_t *>(src), reinterpret_cast<uint64_t *>(dst),
                                   reinterpret_cast<uint32_t *>(len), reinterpret_cast<uint32_t *>(size), device);
        },
        py::call_guard<py::gil_scoped_release>(), py::arg("srcPtrs"), py::arg("dstPtrs"), py::arg("lenPtrs"),
        py::arg("sizePtr"), py::arg("deviceId"));
    cls.def(
        "group_pack_copy",
        [](AccOffloadEntryManager &self, uint64_t src, uint64_t dst, uint64_t len, uint64_t count, uint64_t groups,
           uint64_t packedGroups, uint16_t device) {
            return self.GroupPackCopy(reinterpret_cast<uint64_t *>(src), reinterpret_cast<uint64_t *>(dst),
                                      reinterpret_cast<uint32_t *>(len), reinterpret_cast<uint32_t *>(count),
                                      reinterpret_cast<int64_t *>(groups), reinterpret_cast<int64_t *>(packedGroups),
                                      device);
        },
        py::call_guard<py::gil_scoped_release>(), py::arg("srcPtrs"), py::arg("dstPtrs"), py::arg("lenPtrs"),
        py::arg("numLocalExpertPtr"), py::arg("groupList"), py::arg("packedGroupList"), py::arg("deviceId"));
    cls.def(
        "kv_exchange_copy",
        [](AccOffloadEntryManager &self, uint64_t meta, uint16_t device) {
            return self.KvExchangeCopy(reinterpret_cast<uint64_t *>(meta), device);
        },
        py::call_guard<py::gil_scoped_release>(), py::arg("metaPtr"), py::arg("deviceId"));
    cls.def("register_entry_table", &AccOffloadEntryManager::RegisterEntryTable,
            py::call_guard<py::gil_scoped_release>(), py::arg("entryBytes"), py::arg("rowsPerSlot"));
    cls.def("entry_gather", &AccOffloadEntryManager::EntryGather, py::call_guard<py::gil_scoped_release>(),
            py::arg("dstPtr"), py::arg("idsPtr"), py::arg("countPtr"), py::arg("deviceId"));
}

void DefineAccOffloadInstanceApi(py::module_ &m)
{
    auto cls = py::class_<AccOffloadEntryManager>(m, "_Offload");
    cls.def(py::init<>());
    cls.def("initialize", &AccOffloadEntryManager::Initialize, py::call_guard<py::gil_scoped_release>(),
            py::arg("config"));
    cls.def("uninitialize", &AccOffloadEntryManager::UnInitialize, py::call_guard<py::gil_scoped_release>());
    cls.def("malloc", &AccOffloadEntryManager::HostMalloc, py::call_guard<py::gil_scoped_release>(), py::arg("size"),
            py::arg("flags") = 0);
    cls.def("free", &AccOffloadEntryManager::HostFree, py::call_guard<py::gil_scoped_release>(), py::arg("ptr"),
            py::arg("flags") = 0);
    cls.def(
        "get_dva",
        [](AccOffloadEntryManager &self, uint64_t ptr) -> uint64_t {
            uint64_t dva = 0;
            return self.GetDva(ptr, &dva) == 0 ? dva : 0;
        },
        py::call_guard<py::gil_scoped_release>(), py::arg("ptr"));
    cls.def("host_register", &AccOffloadEntryManager::HostRegister, py::call_guard<py::gil_scoped_release>(),
            py::arg("ptr"), "idempotent in-pool check — the whole pool was registered at initialize");
    cls.def(
        "host_get_device_pointer",
        [](AccOffloadEntryManager &self, uint64_t hostPtr) -> uint64_t {
            uint64_t dva = 0;
            if (self.HostGetDevicePtr(hostPtr, &dva) != 0) {
                return 0;
            }
            return dva;
        },
        py::call_guard<py::gil_scoped_release>(), py::arg("ptr"),
        "returns the device address of any address inside the pre-registered host pool");
    DefineAccOffloadInstanceCopyApi(cls);
}

PYBIND11_MODULE(_pymf_acc_offload, m)
{
    auto offload = m.def_submodule("offload", "Acc Offload Module.");

    DefineAccOffloadConfig(offload);
    DefineAccOffloadApi(offload);
    DefineAccOffloadInstanceApi(offload);
}

#pragma GCC diagnostic pop
