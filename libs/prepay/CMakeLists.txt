cmake_minimum_required(VERSION 3.20)
project(prepay VERSION 0.1.0 LANGUAGES CXX)

option(PREPAY_BUILD_PYTHON "Build the nanobind Python extension" OFF)
option(PREPAY_BUILD_TESTS  "Build native (CTest) tests"          OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export nothing by default; only the prepay_* C ABI carries
# visibility("default") via PREPAY_BUILD_SHARED in the public header. This
# keeps std:: and internal prepay::* symbols out of the dynamic table.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(prepay SHARED
  src/prepay_abi.cpp
  src/prepay_version.cpp
  src/prepay_error.cpp
  src/model.cpp
  src/engine.cpp
)

target_include_directories(prepay
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Marks the prepay_* entry points for export in the public header.
target_compile_definitions(prepay PRIVATE PREPAY_BUILD_SHARED)

set_target_properties(prepay PROPERTIES
  VERSION   ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(PREPAY_BUILD_PYTHON)
  add_subdirectory(python)
endif()

# tests/ currently holds smoke.c and the pytest suite, driven by build.sh
# rather than CTest. Only descend if a CMakeLists is actually there.
if(PREPAY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  enable_testing()
  add_subdirectory(tests)
endif()
