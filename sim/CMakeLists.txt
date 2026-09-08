cmake_minimum_required(VERSION 3.24)
project(embersim VERSION 0.1.0 LANGUAGES CXX)

# Engine-free, dependency-free sim core (ADR 0008 / plan D1). Builds:
#   embersim_core  static library (C++ API + C API in include/embersim/embersim.h)
#   embersim       CLI (run / replay / info / models / version)
#   embersim_tests doctest suite (conformance, CA properties, suppression, formats, golden hashes)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Determinism lockdown (plan F3): the tick path is integer-only, so FP flags only matter for
# init-time precomputation, which is also kept out of the hash path. We still pin strict FP so
# nobody can "accidentally" depend on contraction: no fast-math, no FMA contraction.
if(MSVC)
  add_compile_options(/W4 /permissive- /fp:strict /utf-8 /Zc:__cplusplus /EHsc /bigobj
                      /wd4996)  # deprecated CRT names inside vendored headers
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
endif()

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
add_library(embersim_core STATIC ${CORE_SOURCES})
target_include_directories(embersim_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(embersim_core PUBLIC
  EMBERSIM_PACKS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packs")

add_executable(embersim src/main.cpp)
target_link_libraries(embersim PRIVATE embersim_core)
target_include_directories(embersim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
if(TEST_SOURCES)
  add_executable(embersim_tests ${TEST_SOURCES})
  target_link_libraries(embersim_tests PRIVATE embersim_core)
  target_include_directories(embersim_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(embersim_tests PRIVATE
    EMBERSIM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    EMBERSIM_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_test(NAME embersim_tests COMMAND embersim_tests)
endif()
