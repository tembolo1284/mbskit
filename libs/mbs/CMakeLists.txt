# libmbs -- two targets, one .so:
#
#   mbskit::mbs_c   the shared library + the C header (the waist)
#   mbskit::mbs     mbs_c + the header-only C++ wrapper
#
# Anything that depends on libmbs from another library links mbs_c only.
# Only end-user C++ code links mbs.

set(MBS_VERSION 0.1.0)
set(MBS_SOVERSION 0)

# ---------------------------------------------------------------- the .so --

add_library(mbs SHARED
    src/cabi/mbs_cabi.cpp
)
add_library(mbskit::mbs_c ALIAS mbs)

set_target_properties(mbs PROPERTIES
    OUTPUT_NAME         mbs
    VERSION             ${MBS_VERSION}
    SOVERSION           ${MBS_SOVERSION}
    CXX_STANDARD        17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS      OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# mbs_export.h is generated, not hand-written. It defines MBS_API as export
# while building this target and import for consumers.
generate_export_header(mbs
    BASE_NAME        MBS
    EXPORT_MACRO_NAME MBS_API
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/mbs/mbs_export.h
)

target_include_directories(mbs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

mbskit_warnings(mbs)

# The C ABI is C99; consumers of mbs_c need nothing else. Mark the interface
# so a pure-C target can link it without pulling in C++ feature requirements.
target_compile_features(mbs PRIVATE cxx_std_17)

# ---------------------------------------------------------------- wrapper --

add_library(mbs_cxx INTERFACE)
add_library(mbskit::mbs ALIAS mbs_cxx)
target_link_libraries(mbs_cxx INTERFACE mbs)
target_compile_features(mbs_cxx INTERFACE cxx_std_17)

# ---------------------------------------------------------------- tests ----

if(MBSKIT_BUILD_TESTS)
    # C99 smoke: links the waist only. -Wpedantic -Werror in C mode is what
    # enforces "the header compiles as C99" from docs/cabi-conventions.md.
    add_executable(mbs_smoke_c tests/c/smoke.c)
    target_link_libraries(mbs_smoke_c PRIVATE mbskit::mbs_c)
    set_target_properties(mbs_smoke_c PROPERTIES
        C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    mbskit_warnings(mbs_smoke_c)
    add_test(NAME mbs.smoke.c COMMAND mbs_smoke_c)

    # C++ smoke: exercises the wrapper only; never includes src/.
    add_executable(mbs_smoke_cpp tests/cpp/smoke.cpp)
    target_link_libraries(mbs_smoke_cpp PRIVATE mbskit::mbs)
    mbskit_warnings(mbs_smoke_cpp)
    add_test(NAME mbs.smoke.cpp COMMAND mbs_smoke_cpp)
endif()

# ---------------------------------------------------------------- install --

install(TARGETS mbs mbs_cxx
    EXPORT  mbskitTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/mbs
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/mbs/mbs_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mbs)
