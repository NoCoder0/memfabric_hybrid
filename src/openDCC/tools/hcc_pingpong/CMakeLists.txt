add_executable(hcc_pingpong hcc_pingpong.cpp)
target_include_directories(hcc_pingpong PRIVATE ${PROJECT_OPENDCC_SRC_BASE}/tools/odcc)
target_link_libraries(hcc_pingpong PRIVATE openDCC_shared)
install(TARGETS hcc_pingpong RUNTIME DESTINATION ${PROJECT_OPENDCC_OUTPUT}/bin)
