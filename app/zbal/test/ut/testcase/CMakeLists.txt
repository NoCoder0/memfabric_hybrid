# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# ZBAL is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

message(STATUS "build testcase is required")

include_directories(
        ${ASCEND_HOME_PATH}/include
        ${TORCH_DIR}/include/torch/csrc/api/include
        ${TORCH_DIR}/include
        ${TORCH_DIR}/include/torch/csrc/distributed
        ${TORCH_DIR}/include/torch/csrc/utils
        ${TORCH_DIR}/include/c10/util
        ${CMAKE_CURRENT_SOURCE_DIR}/../mock_overrides
        ${TORCH_NPU_DIR}/include
        ${PYBIND11_DIR}/include
        ${CPYTHON_DIR}
        ${ZBAL_BASE_DIR}/third_party/ska
        ${ZBAL_BASE_DIR}
        ${ZBAL_SRC_DIR}/include
        ${ZBAL_SRC_DIR}/csrc/adaptor/pytorch_npu
        ${ZBAL_SRC_DIR}/csrc/adaptor/deepep
        ${ZBAL_SRC_DIR}/csrc/
        ${ZBAL_SRC_DIR}/csrc/operators
        ${ZBAL_SRC_DIR}/csrc/operators/npu
        ${ZBAL_SRC_DIR}/csrc/common
        ${ZBAL_SRC_DIR}/csrc/sma
        ${ZBAL_SRC_DIR}/csrc/under_api/cann
        ${ZBAL_SRC_DIR}/csrc/under_api/memfabric
        ${ZBAL_SRC_DIR}/csrc/bootstrap
        ${ZBAL_SRC_DIR}/csrc/bootstrap/memory
        ${ZBAL_SRC_DIR}/csrc/bootstrap/memory/memfabric
        ${ZBAL_BASE_DIR}/test/ut/testcase/csrc/include/
        ${ZBAL_TEST_3RDPARTY_OUTPUT_DIR}/googletest/include
        # mock torch_npu/c10 stubs for SMA tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../mock
)

file(GLOB_RECURSE ZBAL_TEST_CASE_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/csrc/ccl/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/csrc/common/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/csrc/sma/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/csrc/adaptor/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ut_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../mock/acl_stub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../mock/zbal_npu_operators_stub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../mock/zbal_sma_device_stub.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../mock/zbal_mem_allocator_stub.cpp
        ${ZBAL_SRC_DIR}/csrc/zbal_mem_allocator.cpp
        ${ZBAL_SRC_DIR}/csrc/zbal.cpp
        ${ZBAL_SRC_DIR}/csrc/zbal_bootstrap.cpp
        ${ZBAL_SRC_DIR}/csrc/zbal_operations.cpp
        # sma source files (not in object libs when DISABLE_ALLOCATOR_COMPILE=ON)
        ${ZBAL_SRC_DIR}/csrc/sma/zbal_sma_mm_heap.cpp
        ${ZBAL_SRC_DIR}/csrc/sma/zbal_sma_config.cpp
        ${ZBAL_SRC_DIR}/csrc/sma/zbal_sma_device_pool.cpp
        ${ZBAL_SRC_DIR}/csrc/sma/zbal_sma_device_info.cpp
        ${ZBAL_SRC_DIR}/csrc/sma/zbal_sma.cpp
        # adaptor source files for ut coverage
        ${ZBAL_SRC_DIR}/csrc/adaptor/pytorch_npu/zbal_pytorch_mm_heap.cpp
        ${ZBAL_SRC_DIR}/csrc/adaptor/pytorch_npu/zbal_pytorch_c10_dma.cpp
        ${ZBAL_SRC_DIR}/csrc/adaptor/pytorch_npu/zbal_pytorch_util.cpp
        ${ZBAL_SRC_DIR}/csrc/adaptor/pytorch_npu/zbal_pytorch_process_group.cpp
        ${ZBAL_SRC_DIR}/csrc/adaptor/pytorch_npu/zbal_pytorch_process_group_impl.cpp
        ${ZBAL_SRC_DIR}/csrc/adaptor/deepep/zbal_deepep.cpp
        ${ZBAL_SRC_DIR}/csrc/adaptor/deepep/zbal_deepep_config.cpp
)

add_executable(test_zbal ${ZBAL_TEST_CASE_SRCS}
        $<TARGET_OBJECTS:common_objects>
        $<TARGET_OBJECTS:operators_objects>
        $<TARGET_OBJECTS:under_api_objects>
        $<TARGET_OBJECTS:boostrap_objects>
        )

target_compile_options(test_zbal PRIVATE -fPIC -Wno-float-equal
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/../mock/sma_preinclude.h")

# Derive Python library path from CPYTHON_DIR (e.g. /usr/local/python3.11.14/include/python3.11 -> /usr/local/python3.11.14/lib)
get_filename_component(PYTHON_LIB_DIR "${CPYTHON_DIR}/../.." REALPATH)
get_filename_component(PYTHON_LIB_DIR "${PYTHON_LIB_DIR}/lib" REALPATH)

target_link_options(test_zbal PRIVATE -pie
    "LINKER:--disable-new-dtags"
    "LINKER:-rpath-link,${PYTHON_LIB_DIR}"
    "LINKER:-rpath-link,${TORCH_DIR}/lib"
    "LINKER:-rpath-link,${TORCH_DIR}/../torch.libs"
    "LINKER:-rpath,${TORCH_DIR}/lib"
    "LINKER:-rpath,${TORCH_NPU_DIR}/lib"
    "LINKER:-rpath,${PYTHON_LIB_DIR}"
    "LINKER:-rpath,${ASCEND_HOME_PATH}/lib64"
    "LINKER:-rpath,${TORCH_DIR}/../torch.libs")

target_link_directories(test_zbal PRIVATE
    ${TORCH_DIR}/lib
    ${TORCH_NPU_DIR}/lib
    ${ASCEND_HOME_PATH}/lib64
    ${PYTHON_LIB_DIR}
)
target_link_libraries(test_zbal PUBLIC
    gtest
    zbal_kernel
    acl_shared
    nnopbase
    torch
    torch_cpu
    c10
    torch_python
    torch_npu
    python3.11
    opapi
    pthread
)

install(TARGETS test_zbal
        RUNTIME DESTINATION ${ZBAL_BASE_DIR}/output/ut
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
