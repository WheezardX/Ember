cmake_minimum_required(VERSION 3.24)
project(emberworld VERSION 0.1.0 LANGUAGES CXX)

# Engine-free world core for the UE renderer (EPIC_5_PLAN C1/C2/C4): Terrain tile-store
# reader, minimal GeoTIFF reader, heightfield -> mesh, (later) the Epic 2 scatter port.
# The same sources compile inside the UE module EmberWorld; this build exists so CI can test
# them without an engine. Vendored headers (nlohmann/json, doctest) are shared with sim/.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/../sim/third_party)
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /wd4996)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(emberworld STATIC ${CORE_SOURCES})
target_include_directories(emberworld PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${VENDOR})

enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(emberworld_tests ${TEST_SOURCES})
target_link_libraries(emberworld_tests PRIVATE emberworld)
target_include_directories(emberworld_tests PRIVATE ${VENDOR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(emberworld_tests PRIVATE
  EMBERWORLD_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME emberworld_tests COMMAND emberworld_tests)
