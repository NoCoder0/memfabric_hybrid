set(INI_FILE "${CMAKE_CURRENT_LIST_DIR}/libzbal_aicpu_kernel.ini")

if(NOT EXISTS "${INI_FILE}")
    message(FATAL_ERROR "INI file not found: ${INI_FILE}")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT must be defined (path to JSON file)")
endif()

file(READ "${INI_FILE}" INI_CONTENT)

# Build JSON manually from the well-known INI structure
# Expected format:
#   [OperatorName]
#   opInfo.opKernelLib=CUSTAICPUKernel
#   opInfo.kernelSo=libzbal_aicpu_kernel.so
#   opInfo.functionName=OperatorName

set(JSON_CONTENT "{\n")
set(OP_COUNT 0)

# Parse INI: split by lines, track current section
string(REPLACE "\n" ";" INI_LINES "${INI_CONTENT}")
set(CURRENT_OP "")
set(CURRENT_OPINFO "")

foreach(LINE ${INI_LINES})
    string(STRIP "${LINE}" LINE)
    # Skip empty lines and comments
    if(LINE STREQUAL "" OR LINE MATCHES "^[;#]")
        continue()
    endif()

    # Section header: [OperatorName]
    if(LINE MATCHES "^\\[(.+)\\]$")
        # Write previous operator
        if(NOT CURRENT_OP STREQUAL "")
            math(EXPR OP_COUNT "${OP_COUNT} + 1")
            if(OP_COUNT GREATER 1)
                set(JSON_CONTENT "${JSON_CONTENT},\n")
            endif()
            set(JSON_CONTENT "${JSON_CONTENT}    \"${CURRENT_OP}\": {\n        \"opInfo\": {\n${CURRENT_OPINFO}        }\n    }")
        endif()
        set(CURRENT_OP "${CMAKE_MATCH_1}")
        set(CURRENT_OPINFO "")
        continue()
    endif()

    # opInfo.key=value
    if(LINE MATCHES "^opInfo\\.([^=]+)=(.*)$")
        set(KEY "${CMAKE_MATCH_1}")
        set(VALUE "${CMAKE_MATCH_2}")
        if(NOT CURRENT_OPINFO STREQUAL "")
            set(CURRENT_OPINFO "${CURRENT_OPINFO},\n")
        endif()
        set(CURRENT_OPINFO "${CURRENT_OPINFO}            \"${KEY}\": \"${VALUE}\"")
        continue()
    endif()
endforeach()

# Write last operator
if(NOT CURRENT_OP STREQUAL "")
    math(EXPR OP_COUNT "${OP_COUNT} + 1")
    if(OP_COUNT GREATER 1)
        set(JSON_CONTENT "${JSON_CONTENT},\n")
    endif()
    set(JSON_CONTENT "${JSON_CONTENT}    \"${CURRENT_OP}\": {\n        \"opInfo\": {\n${CURRENT_OPINFO}        }\n    }")
endif()

set(JSON_CONTENT "${JSON_CONTENT}\n}\n")

# Validate we generated something
if(OP_COUNT EQUAL 0)
    message(FATAL_ERROR "No operators found in INI file: ${INI_FILE}")
endif()

file(WRITE "${OUTPUT}" "${JSON_CONTENT}")
message(STATUS "Generated AICPU JSON: ${OUTPUT} (${OP_COUNT} operators)")
