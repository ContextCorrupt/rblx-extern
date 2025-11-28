cmake_minimum_required(VERSION 3.20)
project(cradle VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

file(GLOB_RECURSE SOURCES
    ${PROJECT_SOURCE_DIR}/src/core/*.cpp
    ${PROJECT_SOURCE_DIR}/src/overlay/*.cpp
    ${PROJECT_SOURCE_DIR}/src/evo/**/*.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/*.cpp
    ${PROJECT_SOURCE_DIR}/src/config/*.cpp
    ${PROJECT_SOURCE_DIR}/src/util/engine/instance/*.cpp
    ${PROJECT_SOURCE_DIR}/src/util/engine/datamodel/*.cpp
    ${PROJECT_SOURCE_DIR}/src/util/engine/input/*.cpp
    ${PROJECT_SOURCE_DIR}/src/util/engine/visualengine/*.cpp
    ${PROJECT_SOURCE_DIR}/src/util/engine/wallcheck/*.cpp
    ${PROJECT_SOURCE_DIR}/src/util/security/*.cpp
    ${PROJECT_SOURCE_DIR}/src/util/profiling/*.cpp
    ${PROJECT_SOURCE_DIR}/src/cache/*.cpp
    ${PROJECT_SOURCE_DIR}/src/imgui_impl_dx11.cpp
    ${PROJECT_SOURCE_DIR}/src/main.cpp
)

list(FILTER SOURCES EXCLUDE REGEX "${PROJECT_SOURCE_DIR}/src/evo/evo-core/.*")
list(FILTER SOURCES EXCLUDE REGEX "${PROJECT_SOURCE_DIR}/src/evo/thirdparty/imgui/imgui_freetype.cpp")

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src/evo/thirdparty/imgui
    ${PROJECT_SOURCE_DIR}/src/evo/thirdparty/imgui/backends
    ${PROJECT_SOURCE_DIR}/src/evo/thirdparty/imgui/freetype/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/evo
)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    spdlog::spdlog_header_only
    fmt::fmt-header-only
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    wbemuuid
    winhttp
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()

option(CRADLE_RANDOMIZE_OUTPUT "Randomize executable name after build" ON)
if(CRADLE_RANDOMIZE_OUTPUT)
    set(RANDOMIZE_SCRIPT ${PROJECT_SOURCE_DIR}/cmake/randomize_exe_name.cmake)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_PATH=$<TARGET_FILE:${PROJECT_NAME}>
            -DOUTPUT_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>
            -DNAME_LENGTH=10
            -P ${RANDOMIZE_SCRIPT}
        VERBATIM
        COMMENT "Randomizing executable name")
endif()
