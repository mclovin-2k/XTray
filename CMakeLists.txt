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

add_library(XTray.Core STATIC
    src/instance.cpp
    src/tray.cpp
    src/language.cpp
    src/theme.cpp
    src/logs.cpp
    src/xray_process.cpp
    src/settings.cpp
)
target_include_directories(XTray.Core PUBLIC src)
# These are Windows system libraries; no additional runtime is distributed.
target_link_libraries(XTray.Core PUBLIC advapi32 bcrypt windowsapp)

add_executable(XTray WIN32
    src/main.cpp
    src/log_view.cpp
    src/theme_window.cpp
    src/settings_dialog.cpp
    resources/XTray.rc
)
target_include_directories(XTray PRIVATE resources)
target_link_libraries(XTray PRIVATE XTray.Core user32 gdi32 shell32 ole32 uuid comctl32 uxtheme dwmapi)
# The resource script embeds the manifest, so disable the linker's generated one.
target_link_options(XTray PRIVATE /MANIFEST:NO)

# Test helpers are built only for validation and are never included in the artifact.
add_executable(XTray.Tests tests/core_tests.cpp)
target_link_libraries(XTray.Tests PRIVATE XTray.Core shell32 user32)

foreach(target XTray.Core XTray XTray.Tests)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
    )
endforeach()

message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "MSVC version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Windows SDK: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/xtray-compiler-path.txt"
    CONTENT "${CMAKE_CXX_COMPILER}\n")

enable_testing()
add_test(NAME XTray.Startup COMMAND XTray --smoke-test)
set_tests_properties(XTray.Startup PROPERTIES TIMEOUT 15)
add_test(NAME XTray.Core COMMAND XTray.Tests)
set_tests_properties(XTray.Core PROPERTIES TIMEOUT 90)
