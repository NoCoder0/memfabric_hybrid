set(AICPU_KERNEL_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/zbal_aicpu_kernel.cpp
    ${CMAKE_SOURCE_DIR}/src/csrc/under_api/cann/dl_hal_api.cpp
)

set(AICPU_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_SOURCE_DIR}/src/csrc/under_api/cann
    ${CMAKE_SOURCE_DIR}/src/csrc/operators
    ${CMAKE_SOURCE_DIR}/src/csrc/
)

set(AICPU_COMPILER "${ASCEND_HOME_PATH}/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++")

set(AICPU_OUTPUT_DIR "${CMAKE_BINARY_DIR}/aicpu_kernels")
file(MAKE_DIRECTORY ${AICPU_OUTPUT_DIR})
set(AICPU_SO_OUTPUT "${AICPU_OUTPUT_DIR}/libzbal_aicpu_kernel.so")
set(AICPU_JSON_OUTPUT "${AICPU_OUTPUT_DIR}/libzbal_aicpu_kernel.json")

# Build include flags
set(AICPU_INC_FLAGS)
foreach(INC_DIR ${AICPU_INCLUDE_DIRS})
    list(APPEND AICPU_INC_FLAGS "-I${INC_DIR}")
endforeach()

add_custom_command(
    OUTPUT ${AICPU_JSON_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${AICPU_JSON_OUTPUT} -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_aicpu_json.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libzbal_aicpu_kernel.ini
    COMMENT "Generating AICPU kernel JSON descriptor"
)

add_custom_command(
    OUTPUT ${AICPU_SO_OUTPUT}
    COMMAND ${AICPU_COMPILER}
        -shared -O2 -std=c++17 -fPIC
        ${AICPU_INC_FLAGS}
        -o ${AICPU_SO_OUTPUT}
        ${AICPU_KERNEL_SRC}
    DEPENDS ${AICPU_KERNEL_SRC}
    COMMENT "Compiling AICPU kernel shared library with aarch64 cross-compiler"
)

# Package .so into tar.gz for AICPUKernel mode
# Result: libzbal_aicpu_kernel.tar.gz containing aicpu_kernels_device/libzbal_aicpu_kernel.so
set(AICPU_TAR_ROOT "aicpu_kernels_device")
set(AICPU_TAR_OUTPUT "${AICPU_OUTPUT_DIR}/libzbal_aicpu_kernel.tar.gz")
set(AICPU_STAGING_DIR "${AICPU_OUTPUT_DIR}/tar_staging")
file(MAKE_DIRECTORY ${AICPU_STAGING_DIR})

add_custom_command(
    OUTPUT ${AICPU_TAR_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AICPU_STAGING_DIR}/${AICPU_TAR_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy "${AICPU_SO_OUTPUT}" "${AICPU_STAGING_DIR}/${AICPU_TAR_ROOT}/"
    COMMAND ${CMAKE_COMMAND} -E tar czf "${AICPU_TAR_OUTPUT}" "${AICPU_TAR_ROOT}"
    WORKING_DIRECTORY "${AICPU_STAGING_DIR}"
    DEPENDS ${AICPU_SO_OUTPUT}
    COMMENT "Packaging AICPU kernel into tar.gz (TAR_ROOT_DIR=${AICPU_TAR_ROOT})"
)

add_custom_target(zbal_aicpu_kernel ALL DEPENDS ${AICPU_SO_OUTPUT} ${AICPU_JSON_OUTPUT} ${AICPU_TAR_OUTPUT})

set(CANN_AICPU_KERNEL_DIR "opp/vendors/cust/aicpu/kernel")
set(CANN_AICPU_CONFIG_DIR "opp/vendors/cust/aicpu/config")

install(FILES ${AICPU_TAR_OUTPUT}
    DESTINATION ${CANN_AICPU_KERNEL_DIR}
    COMPONENT zbal)

install(FILES ${AICPU_JSON_OUTPUT}
    DESTINATION ${CANN_AICPU_CONFIG_DIR}
    COMPONENT zbal)

message(STATUS "zbal AICPU kernel source: ${AICPU_KERNEL_SRC}")
message(STATUS "zbal AICPU compiler: ${AICPU_COMPILER}")
message(STATUS "zbal AICPU .so:     ${AICPU_SO_OUTPUT}")
message(STATUS "zbal AICPU JSON:    ${AICPU_JSON_OUTPUT}")
message(STATUS "zbal AICPU tar.gz:  ${AICPU_TAR_OUTPUT}")
