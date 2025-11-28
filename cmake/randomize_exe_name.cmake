cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED INPUT_PATH)
    message(FATAL_ERROR "randomize_exe_name.cmake: INPUT_PATH is not set")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "randomize_exe_name.cmake: OUTPUT_DIR is not set")
endif()
if(NOT EXISTS "${INPUT_PATH}")
    message(WARNING "randomize_exe_name.cmake: input executable '${INPUT_PATH}' does not exist; skipping rename")
    return()
endif()
if(NOT EXISTS "${OUTPUT_DIR}")
    file(MAKE_DIRECTORY "${OUTPUT_DIR}")
endif()

if(NOT DEFINED NAME_LENGTH OR NAME_LENGTH LESS 4)
    set(NAME_LENGTH 8)
endif()

set(ALPHABET "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
get_filename_component(INPUT_EXT "${INPUT_PATH}" EXT)
if(INPUT_EXT STREQUAL "")
    set(INPUT_EXT ".exe")
endif()

set(MAX_TRIES 128)
set(__tries 0)
set(target_path "")
set(random_name "")
while(__tries LESS MAX_TRIES)
    string(RANDOM LENGTH ${NAME_LENGTH} ALPHABET ${ALPHABET} RAND_TOKEN)
    string(TOLOWER "${RAND_TOKEN}" RAND_LOWER)
    set(candidate "${RAND_LOWER}")
    set(target_candidate "${OUTPUT_DIR}/${candidate}${INPUT_EXT}")
    if(NOT EXISTS "${target_candidate}")
        set(target_path "${target_candidate}")
        set(random_name "${candidate}")
        break()
    endif()
    math(EXPR __tries "${__tries} + 1")
endwhile()

if(target_path STREQUAL "")
    message(FATAL_ERROR "randomize_exe_name.cmake: failed to generate unique random name after ${MAX_TRIES} attempts")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${INPUT_PATH}" "${target_path}"
    RESULT_VARIABLE COPY_RESULT
)
if(NOT COPY_RESULT EQUAL 0)
    message(FATAL_ERROR "randomize_exe_name.cmake: failed to copy executable to '${target_path}' (code ${COPY_RESULT})")
endif()

file(REMOVE "${INPUT_PATH}")

set(log_path "${OUTPUT_DIR}/latest_build_name.txt")
file(WRITE "${log_path}" "${random_name}${INPUT_EXT}")

message(STATUS "Executable randomized to ${random_name}${INPUT_EXT}")
