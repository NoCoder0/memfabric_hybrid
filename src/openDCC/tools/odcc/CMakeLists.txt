#
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#      http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
#
###########################################################################
## odcc command line tool
file(GLOB_RECURSE ODCC_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

message(STATUS "openDCC: odcc tool sources = ${ODCC_SRCS}")

add_executable(odcc ${ODCC_SRCS})
target_include_directories(odcc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(odcc PRIVATE -fPIC)
target_link_libraries(odcc PUBLIC openDCC_genetty_static)

install(TARGETS odcc
        RUNTIME DESTINATION ${PROJECT_OPENDCC_OUTPUT}/bin
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
