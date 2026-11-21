# openDCC 的用例单独编成一个可执行文件，不并入 test_memfabric
file(GLOB_RECURSE opendcc_src
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

list(FILTER opendcc_src EXCLUDE REGEX "/mock_device_ts\\.cpp$")
add_library(opendcc_mock_hal SHARED genetty/verbs/mock_device_ts.cpp)
add_library(opendcc_mock_runtime SHARED genetty/verbs/mock_device_ts.cpp)
foreach(target opendcc_mock_hal opendcc_mock_runtime)
    target_include_directories(${target} PRIVATE ${PROJECT_OPENDCC_SRC_BASE}/genetty/csrc)
endforeach()
set_target_properties(opendcc_mock_hal PROPERTIES OUTPUT_NAME ascend_hal)
set_target_properties(opendcc_mock_runtime PROPERTIES OUTPUT_NAME runtime)

file(GLOB_RECURSE genetty_src
        ${PROJECT_OPENDCC_SRC_BASE}/genetty/csrc/*.cpp
)

ADD_UNIT_TEST(test_openDCC
        FILES ${PROJECT_OPENDCC_SRC_BASE}/tools/odcc/pingpong_genetty/odcc_pp_genetty.cpp
              ${opendcc_src} ${TEST_SRC_PATHS}/ut_main.cpp ${genetty_src}
        INCLUDE_PATHS ${TEST_INCLUDE_PATHS}
                        ${PROJECT_OPENDCC_SRC_BASE}/tools/odcc
                        ${PROJECT_OPENDCC_SRC_BASE}/genetty/csrc
                        ${PROJECT_OPENDCC_SRC_BASE}/genetty/csrc/under_api
                        ${PROJECT_OPENDCC_SRC_BASE}/genetty/include
        LINK_LIBRARIES ${TEST_DEPEND_LIBS} opendcc_mock_hal ${CMAKE_DL_LIBS}
        BUILD_FLAGS -fpermissive --coverage
)

add_dependencies(test_openDCC opendcc_mock_runtime)

# Keep hardware stubs private to this test executable, including dlopen of libruntime.so.
set_target_properties(test_openDCC PROPERTIES SKIP_BUILD_RPATH FALSE BUILD_RPATH "$<TARGET_FILE_DIR:opendcc_mock_hal>")
