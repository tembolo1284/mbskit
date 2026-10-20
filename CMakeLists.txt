cmake_minimum_required(VERSION 3.20)

project(mbskit
    VERSION 0.1.0
    DESCRIPTION "Mortgage analytics behind flat C ABIs: libprepay + libmbs"
    LANGUAGES C CXX)

# ---------------------------------------------------------------- options --

option(MBSKIT_BUILD_MBS      "Build libmbs (pools/TBA/ARM/CMO)"           ON)
option(MBSKIT_BUILD_PREPAY   "Build libprepay (prepayment models)"        OFF)  # flip on once libs/prepay is adapted to the superbuild
option(MBSKIT_BUILD_TESTS    "Build C99 and C++ smoke/unit tests"         ON)
option(MBSKIT_BUILD_EXAMPLES "Build examples/"                            OFF)
option(MBSKIT_BUILD_PYTHON   "Build Python extension modules"             OFF)
option(MBSKIT_WERROR         "Treat warnings as errors"                   ON)

# ---------------------------------------------------------------- globals --

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Hourglass rule: nothing is visible from a .so unless it carries LIB_API.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Keep build products together: build/lib/libmbs.so, build/bin/mbs_smoke_c, ...
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(GNUInstallDirs)
include(GenerateExportHeader)
include(CTest)

# Shared warning set, applied per target by mbskit_warnings().
set(MBSKIT_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(MBSKIT_WERROR)
    list(APPEND MBSKIT_WARNINGS -Werror)
endif()

function(mbskit_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 $<$<BOOL:${MBSKIT_WERROR}>:/WX>)
    else()
        target_compile_options(${target} PRIVATE ${MBSKIT_WARNINGS})
    endif()
endfunction()

# ---------------------------------------------------------------- libs -----

if(MBSKIT_BUILD_MBS)
    add_subdirectory(libs/mbs)
endif()

if(MBSKIT_BUILD_PREPAY)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/libs/prepay/CMakeLists.txt)
        add_subdirectory(libs/prepay)
    else()
        message(FATAL_ERROR "MBSKIT_BUILD_PREPAY=ON but libs/prepay/CMakeLists.txt not found")
    endif()
endif()

if(MBSKIT_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------- summary --

message(STATUS "mbskit ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  libmbs     : ${MBSKIT_BUILD_MBS}")
message(STATUS "  libprepay  : ${MBSKIT_BUILD_PREPAY}")
message(STATUS "  tests      : ${MBSKIT_BUILD_TESTS}")
message(STATUS "  examples   : ${MBSKIT_BUILD_EXAMPLES}")
message(STATUS "  python     : ${MBSKIT_BUILD_PYTHON}")
message(STATUS "  -Werror    : ${MBSKIT_WERROR}")
