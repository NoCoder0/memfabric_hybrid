set(RUN_MODE "npu" CACHE STRING "run mode: npu")

set(ZBAL_AICORE_DIR ${ZBAL_SRC_DIR}/csrc/operators/npu/aicore)

file(GLOB ZBAL_KERNEL_CLASSIC_SRCS
    ${ZBAL_AICORE_DIR}/ops/classic/*.cpp
)
source_group(TREE ${ZBAL_AICORE_DIR} PREFIX "ops/classic" FILES ${ZBAL_KERNEL_CLASSIC_SRCS})

file(GLOB ZBAL_KERNEL_DISPATCH_NORMAL_SRCS
    ${ZBAL_AICORE_DIR}/ops/dispatch_combine_normal/*.cpp
)
source_group(TREE ${ZBAL_AICORE_DIR} PREFIX "ops/dispatch_combine_normal" FILES ${ZBAL_KERNEL_DISPATCH_NORMAL_SRCS})

file(GLOB ZBAL_KERNEL_DISPATCH_LOW_LATENCY_SRCS
    ${ZBAL_AICORE_DIR}/ops/dispatch_combine_low_latency/*.cpp
)
source_group(TREE ${ZBAL_AICORE_DIR} PREFIX "ops/dispatch_combine_low_latency" FILES ${ZBAL_KERNEL_DISPATCH_LOW_LATENCY_SRCS})

if(ZBAL_ASCEND_NPU_A3)
    file(GLOB ZBAL_KERNEL_FUSED_DEEP_MOE_SRCS
            ${ZBAL_AICORE_DIR}/ops/fused_deep_moe/*.cpp)
    source_group(TREE ${ZBAL_AICORE_DIR} PREFIX "ops/fused_deep_moe" FILES ${ZBAL_KERNEL_FUSED_DEEP_MOE_SRCS})
endif()

ascendc_library(zbal_kernel STATIC
        ${ZBAL_KERNEL_CLASSIC_SRCS}
        ${ZBAL_KERNEL_DISPATCH_NORMAL_SRCS}
        ${ZBAL_KERNEL_DISPATCH_LOW_LATENCY_SRCS}
        ${ZBAL_KERNEL_FUSED_DEEP_MOE_SRCS}
)

ascendc_include_directories(zbal_kernel PRIVATE
        $ENV{SHMEM_HOME_PATH}/memfabric_hybrid/include/smem/device
        ${ASCEND_HOME_PATH}/include
        ${ASCEND_HOME_PATH}/include/aclnn
        ${ASCEND_HOME_PATH}/include/aclnn/opdev
        ${ASCEND_HOME_PATH}/asc/include
        ${ZBAL_SRC_DIR}/include
        ${ZBAL_SRC_DIR}/csrc/common
        ${ZBAL_SRC_DIR}/csrc/operators
        ${ZBAL_SRC_DIR}/csrc/operators/npu
        ${ZBAL_SRC_DIR}/csrc/operators/npu/aicore/common
)
if(ZBAL_ASCEND_NPU_A3)
    ascendc_include_directories(zbal_kernel PRIVATE
        ${catlass_SOURCE_DIR}/include
        ${ZBAL_SRC_DIR}/csrc/operators/npu/host/fused_deep_moe)
endif()

ascendc_compile_definitions(zbal_kernel PRIVATE
        ASCENDC_DUMP=0
)

# Forward chip type macro to AscendC device kernel compiler
if(ZBAL_ASCEND_NPU_A3)
    ascendc_compile_definitions(zbal_kernel PRIVATE ZBAL_ASCEND_NPU_A3)
elseif(ZBAL_ASCEND_NPU_A5)
    ascendc_compile_definitions(zbal_kernel PRIVATE ZBAL_ASCEND_NPU_A5)
endif()

option(BUILD_ZBAL_AICPU_KERNEL "Build AICPU persistent dispatcher kernel" ON)

if (BUILD_ZBAL_AICPU_KERNEL)
    if (EXISTS ${ASCEND_HOME_PATH}/include)
        add_subdirectory(aicpu)
        message(STATUS "zbal: AICPU kernel subdirectory added")
    else()
        message(WARNING "zbal: ASCEND_HOME_PATH not found, cannot build AICPU kernel")
    endif()
else()
    message(STATUS "zbal: AICPU kernel build disabled (BUILD_ZBAL_AICPU_KERNEL=OFF)")
endif()