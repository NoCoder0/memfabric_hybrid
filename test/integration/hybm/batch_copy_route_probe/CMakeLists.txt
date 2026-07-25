# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.

if (NOT XPU_TYPE STREQUAL "NPU")
    message(STATUS "Skip batch_copy_route_probe because XPU_TYPE is not NPU")
    return()
endif ()

set(PROBE_ASCEND_HOME_PATH $ENV{ASCEND_HOME_PATH})
if ("${PROBE_ASCEND_HOME_PATH}" STREQUAL "")
    message(WARNING "Skip batch_copy_route_probe because ASCEND_HOME_PATH is not set")
    return()
endif ()

set(PROBE_AICPU_COMPILER
        "${PROBE_ASCEND_HOME_PATH}/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++")
set(PROBE_AICPU_SO "${CMAKE_CURRENT_BINARY_DIR}/libcann_hybm_probe_kernel.so")
set(PROBE_AICPU_JSON "${CMAKE_CURRENT_BINARY_DIR}/libcann_hybm_probe_kernel.json")
set(PROBE_AICPU_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/cann-hybm-probe.tar.gz")
set(PROBE_AICPU_STAGING "${CMAKE_CURRENT_BINARY_DIR}/aicpu_staging/aicpu_kernels_device")

set(PROBE_AICPU_INCLUDE_DIRS
        ${PROJECT_HYBM_SRC_BASE}/include
        ${PROJECT_HYBM_SRC_BASE}/csrc/common
        ${PROJECT_HYBM_SRC_BASE}/csrc/under_api
        ${PROJECT_HYBM_SRC_BASE}/ops/hybm_kernel
        ${PROBE_ASCEND_HOME_PATH}/aarch64-linux/pkg_inc/base
        ${CMAKE_CURRENT_SOURCE_DIR}
)
set(PROBE_AICPU_INCLUDE_FLAGS)
foreach (INCLUDE_DIR ${PROBE_AICPU_INCLUDE_DIRS})
    list(APPEND PROBE_AICPU_INCLUDE_FLAGS "-I${INCLUDE_DIR}")
endforeach ()

set(PROBE_AICPU_SOURCES
        ${PROJECT_HYBM_SRC_BASE}/ops/hybm_kernel/hybm_batch_transfer.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/hybm_batch_copy_probe.cc
)
set(PROBE_AICPU_DEPS
        ${PROJECT_HYBM_SRC_BASE}/ops/hybm_kernel/hybm_batch_transfer.h
        ${PROJECT_HYBM_SRC_BASE}/ops/hybm_kernel/hybm_kernel_log.h
        ${PROJECT_HYBM_SRC_BASE}/include/hybm_def.h
        ${PROJECT_HYBM_SRC_BASE}/csrc/common/hybm_batch_copy_route.h
        ${CMAKE_CURRENT_SOURCE_DIR}/hybm_batch_copy_probe.h
)

add_custom_command(
        OUTPUT ${PROBE_AICPU_SO}
        COMMAND ${PROBE_AICPU_COMPILER}
                -shared -O2 -std=c++17 -fPIC
                ${PROBE_AICPU_INCLUDE_FLAGS}
                -o ${PROBE_AICPU_SO}
                ${PROBE_AICPU_SOURCES}
        DEPENDS ${PROBE_AICPU_SOURCES} ${PROBE_AICPU_DEPS}
        COMMENT "Compiling test-only HybmBatchCopyProbe AICPU kernel"
)

add_custom_command(
        OUTPUT ${PROBE_AICPU_JSON}
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_CURRENT_SOURCE_DIR}/libcann_hybm_probe_kernel.json
                ${PROBE_AICPU_JSON}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libcann_hybm_probe_kernel.json
)

add_custom_command(
        OUTPUT ${PROBE_AICPU_PACKAGE}
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/aicpu_staging"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROBE_AICPU_STAGING}"
        COMMAND ${CMAKE_COMMAND} -E copy "${PROBE_AICPU_SO}" "${PROBE_AICPU_STAGING}/"
        COMMAND tar czf "${PROBE_AICPU_PACKAGE}" -C "${CMAKE_CURRENT_BINARY_DIR}/aicpu_staging"
                "aicpu_kernels_device"
        DEPENDS ${PROBE_AICPU_SO}
        COMMENT "Packaging test-only HybmBatchCopyProbe AICPU kernel"
)

add_custom_target(cann_hybm_probe_kernel ALL
        DEPENDS ${PROBE_AICPU_SO} ${PROBE_AICPU_JSON} ${PROBE_AICPU_PACKAGE})

add_executable(batch_copy_route_probe batch_copy_route_probe_main.cpp)
target_compile_definitions(batch_copy_route_probe PRIVATE MF_BUILD_TEST)
target_compile_options(batch_copy_route_probe PRIVATE -Wno-stack-usage)
target_include_directories(batch_copy_route_probe PRIVATE
        ${PROJECT_HYBM_SRC_BASE}/include
        ${PROJECT_HYBM_SRC_BASE}/csrc/common
        ${PROJECT_HYBM_SRC_BASE}/csrc/driver
        ${PROJECT_HYBM_SRC_BASE}/csrc/transport
        ${PROJECT_HYBM_SRC_BASE}/csrc/transport/device
        ${PROJECT_HYBM_SRC_BASE}/csrc/transport/urma
        ${PROJECT_HYBM_SRC_BASE}/csrc/ts_engine
        ${PROJECT_HYBM_SRC_BASE}/csrc/under_api
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(batch_copy_route_probe PRIVATE hybmm_shared pthread dl)
add_dependencies(batch_copy_route_probe cann_hybm_probe_kernel)

set(PROBE_INSTALL_DIR ${TARGET_INSTALL_DIR}/hybm/batch_copy_route_probe)
install(TARGETS batch_copy_route_probe
        RUNTIME DESTINATION ${PROBE_INSTALL_DIR}
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
install(FILES ${PROBE_AICPU_SO} ${PROBE_AICPU_JSON} ${PROBE_AICPU_PACKAGE}
        DESTINATION ${PROBE_INSTALL_DIR}
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/install_probe.sh
        DESTINATION ${PROBE_INSTALL_DIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        DESTINATION ${PROBE_INSTALL_DIR})
