cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(mf_accoffload_kernel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RELEASE)
endif ()

if (NOT SOC_VERSION)
    message(FATAL_ERROR "SOC_VERSION must be set, e.g. -DSOC_VERSION=ascend910_9382 (A3) or ascend950pr_9599 (A5)")
endif ()
message(STATUS "acc_offload: SOC_VERSION = ${SOC_VERSION}")

include(${CMAKE_CURRENT_SOURCE_DIR}/config_ascendc.cmake)

set(KERNEL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_entry_gather_descriptors.cpp)
if (NOT SOC_VERSION MATCHES "^(Ascend910B|ascend910b)")
    list(APPEND KERNEL_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_sparse_copy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_group_pack_copy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_kv_exchange.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_entry_gather.cpp
    )
endif ()
ascendc_library(mf_accoffload_kernel STATIC ${KERNEL_SOURCES})

ascendc_include_directories(mf_accoffload_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Map SOC_VERSION to the ACC_SOC_VERSION_Ax macros the kernels branch on
# (same naming as the install-time g++ build of acc_offload_operators_launch.cpp).
if (SOC_VERSION MATCHES "^(Ascend910B|ascend910b)")
    ascendc_compile_definitions(mf_accoffload_kernel PRIVATE ACC_SOC_VERSION_A2=1)
elseif (SOC_VERSION MATCHES "^ascend950")
    ascendc_compile_definitions(mf_accoffload_kernel PRIVATE ACC_SOC_VERSION_A5=1)
elseif (SOC_VERSION MATCHES "^ascend910")
    ascendc_compile_definitions(mf_accoffload_kernel PRIVATE ACC_SOC_VERSION_A3=1)
    find_program(ACC_OFFLOAD_AICPU_COMPILER bisheng PATHS "${ASCEND_HOME_PATH}/bin")
    if (NOT ACC_OFFLOAD_AICPU_COMPILER)
        message(FATAL_ERROR "acc_offload: bisheng not found in PATH or CANN toolkit")
    endif ()
    set(AICPU_INIT_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/acc_offload_aicpu_initialize.o")
    set(AICPU_INIT_LAUNCH_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/acc_offload_aicpu_initialize_launch.o")
    add_custom_command(
        OUTPUT "${AICPU_INIT_OBJECT}" "${AICPU_INIT_LAUNCH_OBJECT}"
        COMMAND "${ACC_OFFLOAD_AICPU_COMPILER}" -c -O2 -fPIC -x aicpu --cce-aicpu-no-firmware
            "${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_aicpu_initialize.cc" -o "${AICPU_INIT_OBJECT}"
        COMMAND "${ACC_OFFLOAD_AICPU_COMPILER}" -c -O2 -fPIC -x asc --npu-arch=dav-2201
            "${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_aicpu_initialize.cpp" -o "${AICPU_INIT_LAUNCH_OBJECT}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_aicpu_initialize.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_aicpu_initialize.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/acc_offload_operators.h"
        VERBATIM
    )
    set_source_files_properties("${AICPU_INIT_OBJECT}" "${AICPU_INIT_LAUNCH_OBJECT}"
        PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    target_sources(mf_accoffload_kernel PRIVATE "${AICPU_INIT_OBJECT}" "${AICPU_INIT_LAUNCH_OBJECT}")
else ()
    message(FATAL_ERROR "acc_offload: unsupported SOC_VERSION '${SOC_VERSION}' for SoC-dependent kernel code")
endif ()

ascendc_compile_definitions(mf_accoffload_kernel PRIVATE ASCENDC_DUMP=0)
