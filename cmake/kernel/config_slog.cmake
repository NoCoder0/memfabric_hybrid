# config_slog.cmake - Configure slog (dlog_pub.h / libascendalog.so) for AICPU kernel build
#
# This module finds the CANN slog library needed for kernel logging via dlog_pub.h.
# It sets the following variables:
#   ASCEND_SLOG_LIB_DIR  - Directory containing libascendalog.so (aarch64)
#
# Prerequisite: ASCEND_HOME_PATH must be set to the CANN installation directory.

if(NOT DEFINED ASCEND_HOME_PATH)
    message(STATUS "[config_slog] ASCEND_HOME_PATH not set, skipping slog configuration")
    return()
endif()

if(NOT DEFINED ASCEND_SLOG_LIB_DIR)
    find_library(_CONFIG_SLOG_LIBRARY
        NAMES ascendalog
        PATHS
            "${ASCEND_HOME_PATH}/aarch64-linux/lib64"
            "${ASCEND_HOME_PATH}/lib64"
        NO_CMAKE_SYSTEM_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(_CONFIG_SLOG_LIBRARY)
        get_filename_component(ASCEND_SLOG_LIB_DIR "${_CONFIG_SLOG_LIBRARY}" DIRECTORY)
        message(STATUS "[config_slog] Found slog library: ${_CONFIG_SLOG_LIBRARY}")
    else()
        message(WARNING "[config_slog] libascendalog.so not found in ASCEND_HOME_PATH. "
                        "Kernel logging via dlog_pub.h may not link. "
                        "Set ASCEND_SLOG_LIB_DIR manually to specify the library directory.")
    endif()
    unset(_CONFIG_SLOG_LIBRARY CACHE)
endif()

if(DEFINED ASCEND_SLOG_LIB_DIR)
    message(STATUS "[config_slog] ASCEND_SLOG_LIB_DIR = ${ASCEND_SLOG_LIB_DIR}")
endif()
