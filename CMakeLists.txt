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
# CLAP plugin (§12.1) — vendored header-only SDK in third_party/clap (MIT).
# ---------------------------------------------------------------------------
add_library(ta670_clap MODULE src/plugin/ClapPlugin.cpp)
target_link_libraries(ta670_clap PRIVATE ta670_dsp ta670_warnings)
target_include_directories(ta670_clap SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include)
set_target_properties(ta670_clap PROPERTIES
    PREFIX "" OUTPUT_NAME "TA-670" SUFFIX ".clap"
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
# Tests (§14.2, §14.5) — dependency-free harness + miniature CLAP host.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(ta670_tests tests/test_dsp.cpp)
target_link_libraries(ta670_tests PRIVATE ta670_dsp ta670_warnings)
add_test(NAME dsp_unit COMMAND ta670_tests)

add_executable(ta670_clap_host tests/test_clap_host.cpp)
target_link_libraries(ta670_clap_host PRIVATE ta670_warnings
    ${CMAKE_DL_LIBS})
target_include_directories(ta670_clap_host SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include)
add_test(NAME clap_smoke
    COMMAND ta670_clap_host $<TARGET_FILE:ta670_clap>)
