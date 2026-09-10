file(GLOB_RECURSE memfabric_src
        ${CMAKE_CURRENT_SOURCE_DIR}/hybm/*
        ${CMAKE_CURRENT_SOURCE_DIR}/smem/*
        ${CMAKE_CURRENT_SOURCE_DIR}/utils/*
        ${CMAKE_CURRENT_SOURCE_DIR}/acc_links/*
        ${CMAKE_CURRENT_SOURCE_DIR}/acc_offload/*
)
# helper 仅编入 acc_offload_launch_ut_helper so，避免重复编入测试主二进制
list(FILTER memfabric_src EXCLUDE REGEX "acc_offload/acc_offload_launch_helper\\.cpp$")

# helper so：载体为共享库时验证 GetSelfLibDir 的生产链路（dladdr 解析 so 自身路径）
add_library(acc_offload_launch_ut_helper SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/acc_offload/acc_offload_launch_helper.cpp
        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/csrc/launch/acc_offload_launch.cpp
)
set_target_properties(acc_offload_launch_ut_helper PROPERTIES
        OUTPUT_NAME "acc_offload_launch_ut_helper"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        CXX_STANDARD 17
        POSITION_INDEPENDENT_CODE ON
)
target_include_directories(acc_offload_launch_ut_helper PRIVATE
        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/include/host
        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/csrc
        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/csrc/common
        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/csrc/launch
        ${PROJECT_UTIL_SRC_BASE}
)

ADD_UNIT_TEST(test_memfabric
        FILES ${memfabric_src} ${TEST_SRC_PATHS}/ut_main.cpp
                ${PROJECT_SMEM_SRC_BASE}/csrc/python_wrapper/mk_transfer_adapter/csrc/transfer_util.cpp
                ${PROJECT_ACC_OFFLOAD_SRC_BASE}/csrc/launch/acc_offload_launch.cpp
        INCLUDE_PATHS ${TEST_INCLUDE_PATHS} ${PROJECT_SMEM_SRC_BASE}/csrc/python_wrapper/mk_transfer_adapter/include
                        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/include/host
                        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/csrc
                        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/csrc/common
                        ${PROJECT_ACC_OFFLOAD_SRC_BASE}/csrc/launch
        LINK_LIBRARIES ${TEST_DEPEND_LIBS}
        BUILD_FLAGS -fpermissive --coverage
)
target_compile_definitions(test_memfabric PRIVATE
        ACC_OFFLOAD_UT_HELPER_SO="$<TARGET_FILE:acc_offload_launch_ut_helper>"
)
add_dependencies(test_memfabric acc_offload_launch_ut_helper)
