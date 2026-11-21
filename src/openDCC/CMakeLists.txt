set(OPENDCC_GENETTY_CSRC_DIR ${PROJECT_OPENDCC_SRC_BASE}/genetty/csrc)
set(OPENDCC_GENETTY_INCLUDE_DIR ${PROJECT_OPENDCC_SRC_BASE}/genetty/include)
set(OPENDCC_GENETTY_OUTPUT_DIR ${PROJECT_OPENDCC_OUTPUT})

set(OPENDCC_HCC_CSRC_DIR ${PROJECT_OPENDCC_SRC_BASE}/hcc/csrc)
set(OPENDCC_HCC_INCLUDE_DIR ${PROJECT_OPENDCC_SRC_BASE}/hcc/include)

set(OPENDCC_DCC_CSRC_DIR ${PROJECT_OPENDCC_SRC_BASE}/dcc/csrc)
set(OPENDCC_DCC_INCLUDE_DIR ${PROJECT_OPENDCC_SRC_BASE}/dcc/include)

include_directories(
        ${OPENDCC_GENETTY_INCLUDE_DIR}
        ${OPENDCC_GENETTY_CSRC_DIR}
        ${OPENDCC_HCC_CSRC_DIR}
        ${OPENDCC_HCC_INCLUDE_DIR}
        ${OPENDCC_DCC_CSRC_DIR}
        ${OPENDCC_DCC_INCLUDE_DIR}
)

set(OPENDCC_ALL_OBJECTS "")

add_subdirectory(genetty/csrc)
add_subdirectory(hcc/csrc)
add_subdirectory(dcc/csrc)
add_subdirectory(tools/odcc)

###########################################################################
## single openDCC shared library, the objects are appended by each module
add_library(openDCC_shared SHARED ${OPENDCC_ALL_OBJECTS})
set_target_properties(openDCC_shared PROPERTIES OUTPUT_NAME "mf_opendcc" LINKER_LANGUAGE CXX)
target_link_libraries(openDCC_shared PUBLIC pthread ${CMAKE_DL_LIBS})

install(TARGETS openDCC_shared
        LIBRARY DESTINATION ${PROJECT_OPENDCC_OUTPUT}/lib64
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
)