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

#include <pybind11/functional.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include "zbal_defines.h"
#include "zbal_deepep.h"
#include "zbal_deepep_pybind.h"

namespace py = pybind11;
using namespace zbal;

void pybind11_deepep_adaptor(pybind11::module_ &m)
{
    m.doc() = "DeepEP: an efficient expert-parallel communication library";

    pybind11::class_<zbal::adaptor::deep_ep::Config>(m, "Config")
        .def(pybind11::init<int, int, int, int, int>(), py::arg("num_sms") = ZBAL_CONST_20,
             py::arg("num_max_nvl_chunked_send_tokens") = ZBAL_CONST_6,
             py::arg("num_max_nvl_chunked_recv_tokens") = ZBAL_CONST_256,
             py::arg("num_max_rdma_chunked_send_tokens") = ZBAL_CONST_6,
             py::arg("num_max_rdma_chunked_recv_tokens") = ZBAL_CONST_256)
        .def("get_nvl_buffer_size_hint", &zbal::adaptor::deep_ep::Config::get_nvl_buffer_size_hint)
        .def("get_rdma_buffer_size_hint", &zbal::adaptor::deep_ep::Config::get_rdma_buffer_size_hint);
    m.def("get_low_latency_rdma_size_hint", &zbal::adaptor::deep_ep::get_low_latency_rdma_size_hint);

    pybind11::class_<zbal::adaptor::deep_ep::EventHandle>(m, "EventHandle")
        .def(pybind11::init<>())
        .def("current_stream_wait", &zbal::adaptor::deep_ep::EventHandle::current_stream_wait);

    pybind11::class_<zbal::adaptor::deep_ep::Buffer>(m, "Buffer")
        .def(pybind11::init<int, int, int64_t, int64_t, bool, std::string>())
        .def("is_available", &zbal::adaptor::deep_ep::Buffer::is_available)
        .def("get_num_rdma_ranks", &zbal::adaptor::deep_ep::Buffer::get_num_rdma_ranks)
        .def("get_rdma_rank", &zbal::adaptor::deep_ep::Buffer::get_rdma_rank)
        .def("get_dispatch_layout", &zbal::adaptor::deep_ep::Buffer::get_dispatch_layout)
        .def("get_send_token_idx", &zbal::adaptor::deep_ep::Buffer::get_send_token_idx)
        .def("intranode_dispatch", &zbal::adaptor::deep_ep::Buffer::intranode_dispatch)
        .def("intranode_combine", &zbal::adaptor::deep_ep::Buffer::intranode_combine)
        .def("low_latency_dispatch", &zbal::adaptor::deep_ep::Buffer::low_latency_dispatch)
        .def("low_latency_combine", &zbal::adaptor::deep_ep::Buffer::low_latency_combine)
        .def("clean_low_latency_buffer", &zbal::adaptor::deep_ep::Buffer::clean_low_latency_buffer)
#if defined(ZBAL_ASCEND_NPU_A3) && defined(ZBAL_FUSED_DEEP_MOE_ENABLED)
        .def("fused_deep_moe", &zbal::adaptor::deep_ep::Buffer::fused_deep_moe)
#endif
        ;
}