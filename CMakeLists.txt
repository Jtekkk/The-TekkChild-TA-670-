cmake_minimum_required(VERSION 3.22)

project(ta670
    VERSION 0.1.0
    DESCRIPTION "Tekkchild Model 670 vari-mu compressor - DSP core"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Portable, framework-free DSP core (header-only). Zero dependencies. (§12.1)
# ---------------------------------------------------------------------------
add_library(ta670_dsp INTERFACE)
target_include_directories(ta670_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Warnings-as-errors per the coding standard (§12.5).
add_library(ta670_warnings INTERFACE)
if(MSVC)
    target_compile_options(ta670_warnings INTERFACE /W4 /WX /permissive-)
else()
    target_compile_options(ta670_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow)
endif()

# ---------------------------------------------------------------------------
# Tests (§14.2) — dependency-free harness.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(ta670_tests tests/test_dsp.cpp)
target_link_libraries(ta670_tests PRIVATE ta670_dsp ta670_warnings)
add_test(NAME dsp_unit COMMAND ta670_tests)
