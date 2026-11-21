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
# 源文件基于本文件所在目录收集，避免父作用域变量为空或错误时 glob 静默返回空列表，
# 空列表会让 OBJECT 库退化成"没有源文件"，最终报出难以定位的 'No SOURCES given to target'
# root objects, the common types and the c api which are used by the sub directories
file(GLOB ROOT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if (NOT ROOT_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}")
endif ()
add_library(root_objects OBJECT ${ROOT_SRCS})
target_compile_options(root_objects PRIVATE -fPIC)

# common objects
file(GLOB_RECURSE COMMON_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp)
if (NOT COMMON_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}/common")
endif ()
add_library(common_objects OBJECT ${COMMON_SRCS})
target_compile_options(common_objects PRIVATE -fPIC)

# under_api objects
file(GLOB_RECURSE UNDER_API_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/under_api/*.cpp)
if (NOT UNDER_API_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}/under_api")
endif ()
add_library(under_api_objects OBJECT ${UNDER_API_SRCS})
target_compile_options(under_api_objects PRIVATE -fPIC)

# urma objects
file(GLOB_RECURSE URMA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/urma/*.cpp)
if (NOT URMA_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}/urma")
endif ()
add_library(urma_objects OBJECT ${URMA_SRCS})
target_compile_options(urma_objects PRIVATE -fPIC)

# verbs objects
file(GLOB_RECURSE VERBS_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/verbs/*.cpp)
if (NOT VERBS_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}/verbs")
endif ()
add_library(verbs_objects OBJECT ${VERBS_SRCS})
target_compile_options(verbs_objects PRIVATE -fPIC)

# append the genetty objects to the single openDCC shared library
list(APPEND OPENDCC_ALL_OBJECTS
        $<TARGET_OBJECTS:root_objects>
        $<TARGET_OBJECTS:common_objects>
        $<TARGET_OBJECTS:under_api_objects>
        $<TARGET_OBJECTS:urma_objects>
        $<TARGET_OBJECTS:verbs_objects>
)
set(OPENDCC_ALL_OBJECTS ${OPENDCC_ALL_OBJECTS} PARENT_SCOPE)

###########################################################################
## libraries
# static library
add_library(openDCC_genetty_static STATIC
        $<TARGET_OBJECTS:root_objects>
        $<TARGET_OBJECTS:common_objects>
        $<TARGET_OBJECTS:under_api_objects>
        $<TARGET_OBJECTS:urma_objects>
        $<TARGET_OBJECTS:verbs_objects>
)
set_target_properties(openDCC_genetty_static PROPERTIES OUTPUT_NAME "mf_opendcc_genetty")

target_link_libraries(openDCC_genetty_static PUBLIC pthread ${CMAKE_DL_LIBS})
