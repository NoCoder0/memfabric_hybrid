#
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# ubs-comm is licensed under the Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#      http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
#
###########################################################################
## target objects
# hcc uses the socket definitions provided by acc_links
include_directories(${PROJECT_ACCLINKS_SRC_BASE}/include)

# 源文件基于本文件所在目录收集，避免父作用域变量为空或错误时 glob 静默返回空列表，
# 空列表会让 OBJECT 库退化成"没有源文件"，最终报出难以定位的 'No SOURCES given to target'
# root objects, the common types and the c api which are used by the sub directories
file(GLOB ROOT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if (NOT ROOT_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}")
endif ()
add_library(hcc_root_objects OBJECT ${ROOT_SRCS})
target_compile_options(hcc_root_objects PRIVATE -fPIC)

# common objects, currently only headers exist so the target is created on demand
file(GLOB_RECURSE COMMON_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp)
if (COMMON_SRCS)
    add_library(hcc_common_objects OBJECT ${COMMON_SRCS})
    target_compile_options(hcc_common_objects PRIVATE -fPIC)
endif ()

# cs objects
file(GLOB_RECURSE CS_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/client_server/*.cpp)
if (NOT CS_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}/client_server")
endif ()
add_library(hcc_cs_objects OBJECT ${CS_SRCS})
target_compile_options(hcc_cs_objects PRIVATE -fPIC)

# p2p objects
file(GLOB_RECURSE P2P_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/peer2peer/*.cpp)
if (NOT P2P_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}/peer2peer")
endif ()
add_library(hcc_p2p_objects OBJECT ${P2P_SRCS})
target_compile_options(hcc_p2p_objects PRIVATE -fPIC)

set(HCC_OBJECTS
        $<TARGET_OBJECTS:hcc_root_objects>
        $<TARGET_OBJECTS:hcc_cs_objects>
        $<TARGET_OBJECTS:hcc_p2p_objects>
)
if (TARGET hcc_common_objects)
    list(APPEND HCC_OBJECTS $<TARGET_OBJECTS:hcc_common_objects>)
endif ()

# append the hcc objects to the single openDCC shared library
list(APPEND OPENDCC_ALL_OBJECTS ${HCC_OBJECTS})
set(OPENDCC_ALL_OBJECTS ${OPENDCC_ALL_OBJECTS} PARENT_SCOPE)

###########################################################################
## libraries
# static library
add_library(openDCC_hcc_static STATIC ${HCC_OBJECTS})
set_target_properties(openDCC_hcc_static PROPERTIES OUTPUT_NAME "mf_opendcc_hcc")
