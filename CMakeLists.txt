cmake_minimum_required(VERSION 3.15)
project(xgrammar LANGUAGES CXX)

if(EXISTS ${CMAKE_BINARY_DIR}/config.cmake)
  include(${CMAKE_BINARY_DIR}/config.cmake)
else()
  if(EXISTS ${CMAKE_SOURCE_DIR}/config.cmake)
    include(${CMAKE_SOURCE_DIR}/config.cmake)
  endif()
endif()

option(BUILD_PYTHON_BINDINGS "Build Python bindings" ON)
option(BUILD_GTEST "Build tests with GoogleTest" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "No build type specified; defaulting to CMAKE_BUILD_TYPE=RelWithDebugInfo.")
  set(CMAKE_BUILD_TYPE
      "RelWithDebugInfo"
      CACHE STRING "The build type" FORCE
  )
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build Python bindings: ${BUILD_PYTHON_BINDINGS}")
message(STATUS "Build C++ tests with GoogleTest: ${BUILD_GTEST}")

if(MSVC)
  set(CMAKE_CXX_FLAGS "/Wall /WX ${CMAKE_CXX_FLAGS}")
else()
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "-Ofast ${CMAKE_CXX_FLAGS}")
  endif()

  set(CMAKE_CXX_FLAGS
      "-Wall -Wextra -Werror -pedantic -Wno-unused-parameter -flto=auto ${CMAKE_CXX_FLAGS}"
  )
endif()

set(XGRAMMAR_INCLUDE_PATH
    ${PROJECT_SOURCE_DIR}/3rdparty/picojson ${PROJECT_SOURCE_DIR}/3rdparty/dlpack/include
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/cpp
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(XGRAMMAR_COMPILE_DEFINITIONS XGRAMMAR_ENABLE_LOG_DEBUG=1)
else()
  set(XGRAMMAR_COMPILE_DEFINITIONS XGRAMMAR_ENABLE_LOG_DEBUG=0)
endif()

file(GLOB_RECURSE XGRAMMAR_SOURCES_PATH "${PROJECT_SOURCE_DIR}/cpp/*.cc")
list(FILTER XGRAMMAR_SOURCES_PATH EXCLUDE REGEX "${PROJECT_SOURCE_DIR}/cpp/pybind/.*\\.cc")

add_library(xgrammar STATIC ${XGRAMMAR_SOURCES_PATH})
target_include_directories(xgrammar PUBLIC ${XGRAMMAR_INCLUDE_PATH})
target_compile_definitions(xgrammar PUBLIC ${XGRAMMAR_COMPILE_DEFINITIONS})

if(BUILD_PYTHON_BINDINGS)
  add_subdirectory(${PROJECT_SOURCE_DIR}/cpp/pybind)
  target_compile_options(xgrammar PUBLIC ${TORCH_CXX_FLAGS})
endif()

if(BUILD_GTEST)
  add_subdirectory(${PROJECT_SOURCE_DIR}/3rdparty/googletest)

  file(GLOB_RECURSE XGRAMMAR_TEST_SOURCES_PATH "${PROJECT_SOURCE_DIR}/tests/cpp/*.cc")
  enable_testing()

  add_executable(xgrammar_test ${XGRAMMAR_TEST_SOURCES_PATH})

  # libtorch requires -D_GLIBCXX_USE_CXX11_ABI=0. Fix the incompatibility issue caused by this flag.
  if(BUILD_PYTHON_BINDINGS)
    target_compile_options(gtest PUBLIC ${TORCH_CXX_FLAGS})
    target_compile_options(gtest_main PUBLIC ${TORCH_CXX_FLAGS})
  endif()

  target_link_libraries(xgrammar_test xgrammar gtest gtest_main)

  include(GoogleTest)
  gtest_discover_tests(xgrammar_test)
endif()
