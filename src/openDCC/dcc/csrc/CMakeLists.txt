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
file(GLOB ROOT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if (NOT ROOT_SRCS)
    message(FATAL_ERROR "No cpp source found under ${CMAKE_CURRENT_SOURCE_DIR}")
endif ()
add_library(dcc_root_objects OBJECT ${ROOT_SRCS})
target_compile_options(dcc_root_objects PRIVATE -fPIC)

# append the dcc objects to the single openDCC shared library
list(APPEND OPENDCC_ALL_OBJECTS $<TARGET_OBJECTS:dcc_root_objects>)
set(OPENDCC_ALL_OBJECTS ${OPENDCC_ALL_OBJECTS} PARENT_SCOPE)

###########################################################################
## libraries
# static library
add_library(openDCC_dcc_static STATIC $<TARGET_OBJECTS:dcc_root_objects>)
set_target_properties(openDCC_dcc_static PROPERTIES OUTPUT_NAME "mf_opendcc_dcc")

##########################################################################
