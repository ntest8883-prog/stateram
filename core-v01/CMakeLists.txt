cmake_minimum_required(VERSION 3.16)
project(StateRAMCoreV01 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(stateram_core SHARED
    src/stateram_core.cpp
    src/platform.cpp
)

target_include_directories(stateram_core
    PUBLIC include
    PRIVATE src
)

target_compile_definitions(stateram_core PRIVATE STATERAM_CORE_BUILD)

if (MSVC)
    target_compile_options(stateram_core PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(stateram_core PRIVATE Cabinet)
else()
    target_compile_options(stateram_core PRIVATE -Wall -Wextra -Wpedantic)
    find_package(ZLIB REQUIRED)
    target_link_libraries(stateram_core PRIVATE ZLIB::ZLIB pthread)
endif()

add_executable(stateram_core_selfcheck examples/core_selfcheck.cpp)
target_link_libraries(stateram_core_selfcheck PRIVATE stateram_core)

if (WIN32)
    add_executable(stateram_workbench WIN32 workbench/workbench_win32.cpp)
    set_target_properties(stateram_workbench PROPERTIES OUTPUT_NAME "StateRAM_Workbench")
    target_link_libraries(stateram_workbench PRIVATE stateram_core user32 gdi32)

    add_executable(stateram_workbench_safe workbench/workbench_launcher.cpp)
    set_target_properties(stateram_workbench_safe PROPERTIES OUTPUT_NAME "StateRAM_Workbench_Safe")
    target_link_libraries(stateram_workbench_safe PRIVATE user32 advapi32)

    add_executable(stateram_4to8_evaluator evaluation/evaluator_win32.cpp)
    set_target_properties(stateram_4to8_evaluator PROPERTIES OUTPUT_NAME "StateRAM_4to8_Evaluator")
    target_link_libraries(stateram_4to8_evaluator PRIVATE stateram_core Psapi)

    add_executable(stateram_4to8_evaluator_safe evaluation/evaluator_launcher.cpp)
    set_target_properties(stateram_4to8_evaluator_safe PROPERTIES OUTPUT_NAME "StateRAM_4to8_Evaluator_Safe")
    target_link_libraries(stateram_4to8_evaluator_safe PRIVATE user32 advapi32)
endif()
