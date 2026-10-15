cmake_minimum_required(VERSION 4.2)

project(XTray LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "XTray requires Windows and the Microsoft MSVC toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "XTray supports x64 builds only.")
endif()

if(CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    message(FATAL_ERROR "Select the x64 target platform.")
endif()

add_executable(XTray WIN32
    src/main.cpp
    resources/XTray.rc
)

set_target_properties(XTray PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_definitions(XTray PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(XTray PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
)

target_include_directories(XTray PRIVATE resources)
target_link_libraries(XTray PRIVATE user32)
# The resource script embeds the manifest, so disable the linker's generated one.
target_link_options(XTray PRIVATE /MANIFEST:NO)

message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "MSVC version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Windows SDK: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/xtray-compiler-path.txt"
    CONTENT "${CMAKE_CXX_COMPILER}\n")

enable_testing()
add_test(NAME XTray.Startup COMMAND XTray --smoke-test)
set_tests_properties(XTray.Startup PROPERTIES TIMEOUT 15)
