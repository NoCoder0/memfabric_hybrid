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

# Operator build filter: comma-separated list of operator groups under
# src/csrc/operators/npu/aicore/ops/ to compile. Empty means build all groups.
set(ZBAL_OP_BUILD_FILTER "" CACHE STRING
        "Comma-separated operator groups to build (classic, dispatch_combine_normal, \
dispatch_combine_low_latency, fused_deep_moe). Empty builds all groups.")
# Canonical group names, used for validation.
set(ZBAL_OP_GROUPS classic dispatch_combine_normal dispatch_combine_low_latency fused_deep_moe)

# Accepts both short aliases (classic, dcn, dcll, fmoe) and canonical group names,
# so every entry point (build.sh, setup.py, direct -DZBAL_OP_BUILD_FILTER=...) works
# without a separate translation step. Unknown tokens pass through and are rejected below.
function(zbal_op_normalize token out_var)
    string(TOLOWER "${token}" _op)
    if (_op STREQUAL "dcn")
        set(${out_var} "dispatch_combine_normal" PARENT_SCOPE)
    elseif (_op STREQUAL "dcll")
        set(${out_var} "dispatch_combine_low_latency" PARENT_SCOPE)
    elseif (_op STREQUAL "fmoe")
        set(${out_var} "fused_deep_moe" PARENT_SCOPE)
    else ()
        set(${out_var} "${_op}" PARENT_SCOPE)
    endif ()
endfunction()

string(REPLACE "," ";" _raw_filter "${ZBAL_OP_BUILD_FILTER}")
set(ZBAL_OP_BUILD_FILTER_LIST "")
foreach (_token IN LISTS _raw_filter)
    if (_token STREQUAL "")
        continue()
    endif ()
    zbal_op_normalize("${_token}" _canonical)
    list(APPEND ZBAL_OP_BUILD_FILTER_LIST "${_canonical}")
endforeach ()
list(REMOVE_DUPLICATES ZBAL_OP_BUILD_FILTER_LIST)
foreach (_op IN LISTS ZBAL_OP_BUILD_FILTER_LIST)
    if (NOT _op IN_LIST ZBAL_OP_GROUPS)
        message(FATAL_ERROR "zbal: unknown operator group '${_op}' in ZBAL_OP_BUILD_FILTER "
                "(valid: classic,dcn,dcll,fmoe or their full names)")
    endif ()
endforeach ()

# Sets <out_var> to TRUE when the operator group is selected (or no filter given).
function(zbal_op_enabled op_name out_var)
    if (NOT ZBAL_OP_BUILD_FILTER_LIST)
        set(${out_var} TRUE PARENT_SCOPE)
    elseif (op_name IN_LIST ZBAL_OP_BUILD_FILTER_LIST)
        set(${out_var} TRUE PARENT_SCOPE)
    else ()
        set(${out_var} FALSE PARENT_SCOPE)
    endif ()
endfunction()

zbal_op_enabled("classic" ZBAL_BUILD_OP_CLASSIC)
zbal_op_enabled("dispatch_combine_normal" ZBAL_BUILD_OP_DISPATCH_COMBINE_NORMAL)
zbal_op_enabled("dispatch_combine_low_latency" ZBAL_BUILD_OP_DISPATCH_COMBINE_LOW_LATENCY)
zbal_op_enabled("fused_deep_moe" ZBAL_BUILD_OP_FUSED_DEEP_MOE)

# UT mode skips device kernel compilation, so fused_deep_moe has no launcher
# symbol to link against. Force it off to match the pre-filter UT behavior.
if (BUILD_ZBAL_MODULE_UT AND ZBAL_BUILD_OP_FUSED_DEEP_MOE)
    message(STATUS "zbal: UT build forces fused_deep_moe off (device kernel is not compiled)")
    set(ZBAL_BUILD_OP_FUSED_DEEP_MOE FALSE)
endif ()

message(STATUS "zbal: operator build filter='${ZBAL_OP_BUILD_FILTER}' "
        "(classic=${ZBAL_BUILD_OP_CLASSIC}, "
        "dispatch_combine_normal=${ZBAL_BUILD_OP_DISPATCH_COMBINE_NORMAL}, "
        "dispatch_combine_low_latency=${ZBAL_BUILD_OP_DISPATCH_COMBINE_LOW_LATENCY}, "
        "fused_deep_moe=${ZBAL_BUILD_OP_FUSED_DEEP_MOE})")

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
# ZBAL_LIB_OUTPUT_SUBDIR keeps artifacts of multi-variant builds (e.g. a3/a5 in one wheel) separate
set(ZBAL_LIB_OUTPUT_SUBDIR "" CACHE STRING "Subdirectory under output/ for library artifacts (e.g. a3, a5)")
if (ZBAL_LIB_OUTPUT_SUBDIR STREQUAL "")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ZBAL_BASE_DIR}/output)
else ()
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ZBAL_BASE_DIR}/output/${ZBAL_LIB_OUTPUT_SUBDIR})
endif ()

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
# Guard: abort configure when chip type is ambiguous (both set) or missing for NPU build, otherwise
# kernel-side #if defined(A3)||defined(A5) compiles to empty shells without any error
if (ZBAL_ASCEND_NPU_A3 AND ZBAL_ASCEND_NPU_A5)
    message(FATAL_ERROR "zbal: ZBAL_ASCEND_NPU_A3 and ZBAL_ASCEND_NPU_A5 are both set, specify exactly one chip type")
endif ()
if (ZBAL_ASCEND_NPU_A3)
    add_compile_definitions(ZBAL_ASCEND_NPU_A3)
elseif (ZBAL_ASCEND_NPU_A5)
    add_compile_definitions(ZBAL_ASCEND_NPU_A5)
elseif (XPU_TYPE STREQUAL "NPU" AND NOT BUILD_ZBAL_MODULE_UT)
    message(FATAL_ERROR "zbal: chip type not specified for NPU build, pass -DZBAL_ASCEND_NPU_A3=1 or "
                        "-DZBAL_ASCEND_NPU_A5=1, otherwise device kernels compile to empty shells")
endif ()

# Availability macros consumed by the host-side launcher call sites. A group that
# is not selected must be compiled out there, otherwise its device launcher symbols
# would be undefined and the zbal extension would fail to load.
if (ZBAL_BUILD_OP_CLASSIC)
    add_compile_definitions(ZBAL_OP_CLASSIC_ENABLED)
endif ()
if (ZBAL_BUILD_OP_DISPATCH_COMBINE_NORMAL)
    add_compile_definitions(ZBAL_OP_DISPATCH_NORMAL_ENABLED)
endif ()
if (ZBAL_BUILD_OP_DISPATCH_COMBINE_LOW_LATENCY)
    add_compile_definitions(ZBAL_OP_DISPATCH_LOW_LATENCY_ENABLED)
endif ()

if (ZBAL_ASCEND_NPU_A3 AND ZBAL_BUILD_OP_FUSED_DEEP_MOE)
    add_compile_definitions(ZBAL_FUSED_DEEP_MOE_ENABLED)
    message(STATUS "fused_deep_moe operator compilation ENABLED (selected by ZBAL_OP_BUILD_FILTER)")
endif ()

if (ZBAL_ASCEND_NPU_A3 AND ZBAL_BUILD_OP_FUSED_DEEP_MOE)
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
