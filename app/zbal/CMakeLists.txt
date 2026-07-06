# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# ZBAL is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(zbal LANGUAGES CXX)

option(BUILD_ZBAL_MODULE_UT "build unit test for zbal library" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_VERBOSE_MAKEFILE ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RELEASE)
endif ()

include(cmake/config_version.cmake)

if (BUILD_ZBAL_MODULE_UT)
    # UT mode: only need ASCEND_HOME_PATH for CANN headers, skip kernel cmake
    set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}" CACHE PATH "ASCEND CANN package installation directory")
else ()
    include(cmake/config_ascendc.cmake)
endif ()

include(cmake/config_pybind_torch.cmake)
include(cmake/config_git_last_commit.cmake)

set(ZBAL_BASE_DIR ${PROJECT_SOURCE_DIR})
set(ZBAL_SRC_DIR ${ZBAL_BASE_DIR}/src)

# set the library output dir to the python dir for wheel package build
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ZBAL_BASE_DIR}/output)

add_compile_options(-Wunused-value -Wcast-align -Wno-macro-redefined)
add_compile_options(-fpermissive -Wno-error)
add_compile_options(-Wcast-qual -Winvalid-pch -Wwrite-strings -Wsign-compare -Wextra -Wno-deprecated-declarations)
add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)

if (${CMAKE_BUILD_TYPE} MATCHES "RELEASE")
    add_compile_options(-O3)
    add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
    add_compile_options(-fstack-protector-strong)
    add_compile_options(-Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now -fPIE -fPIC -ftrapv -s)
    message(STATUS "build type set to RELEASE")
else ()
    add_compile_options(-g -rdynamic)
endif ()

if (BUILD_ZBAL_MODULE_UT)
    message(STATUS "test: unit test build is required")
endif ()

# forward chip type macro from setup.py / build.sh to C++ preprocessor
if (ZBAL_ASCEND_NPU_A3)
    add_compile_definitions(ZBAL_ASCEND_NPU_A3)
elseif (ZBAL_ASCEND_NPU_A5)
    add_compile_definitions(ZBAL_ASCEND_NPU_A5)
endif ()

# prepare catlass for fused_moe (A3 only)
if (ZBAL_ASCEND_NPU_A3)
    include(FetchContent)
    FetchContent_Declare(
            catlass
            GIT_REPOSITORY https://gitcode.com/cann/catlass
            GIT_TAG 404b4309c4e6366f2019fdba8feac376e0021836  # catlass-v1-stable
            GIT_SHALLOW TRUE
    )
    FetchContent_Populate(catlass)
    if (NOT catlass_POPULATED)
        FetchContent_Populate(catlass)
    endif ()
    message(STATUS "CATLASS cloned in ${catlass_SOURCE_DIR}")
endif ()

add_subdirectory(src/csrc)

if (BUILD_ZBAL_MODULE_UT)
    add_subdirectory(test)
endif ()