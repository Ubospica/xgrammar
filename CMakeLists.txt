cmake_minimum_required(VERSION 3.15)
project(xgrammar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    add_compile_options(/Wall /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -pedantic -Wno-unused-parameter)
endif()


# Set default build type if not set
if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type specified; defaulting to CMAKE_BUILD_TYPE=Debug.")
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "The build type" FORCE)
endif()

# Set pybind11
find_package(Python COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

add_library(xgrammar_interface INTERFACE)

set(XGRAMMAR_INCLUDE
    ${PROJECT_SOURCE_DIR}/3rdparty/picojson
    ${PROJECT_SOURCE_DIR}/3rdparty/dlpack/include
    ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(xgrammar_interface INTERFACE ${XGRAMMAR_INCLUDE})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(xgrammar_interface INTERFACE XGRAMMAR_ENABLE_LOG_DEBUG=1)
else()
    target_compile_definitions(xgrammar_interface INTERFACE XGRAMMAR_ENABLE_LOG_DEBUG=0)
endif()

# Source files list
file(GLOB_RECURSE SOURCES "${PROJECT_SOURCE_DIR}/cpp/*.cc")
add_library(xgrammar_objs OBJECT ${SOURCES})
target_link_libraries(xgrammar_objs PUBLIC xgrammar_interface)

add_library(xgrammar_lib $<TARGET_OBJECTS:xgrammar_objs>)
target_link_libraries(xgrammar_lib PUBLIC xgrammar_interface)

# Create python module
pybind11_add_module(xgrammar_bindings $<TARGET_OBJECTS:xgrammar_objs>)
# Install target
install(TARGETS xgrammar_bindings DESTINATION .)
