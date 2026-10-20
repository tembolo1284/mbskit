# Configured only when PREPAY_BUILD_PYTHON=ON (set by pyproject/scikit-build-core).

find_package(Python 3.9 REQUIRED
  COMPONENTS Interpreter Development.Module
  OPTIONAL_COMPONENTS Development.SABIModule)

# Locate the nanobind CMake package that ships inside the pip/conda install.
execute_process(
  COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
find_package(nanobind CONFIG REQUIRED)

# The compiled extension. Underscore-prefixed; the pure-Python prepay/__init__.py
# re-exports a clean API on top of it.
nanobind_add_module(_prepay
  STABLE_ABI          # single wheel for CPython 3.12+
  NB_STATIC           # fold libnanobind into the extension
  ext.cpp
)

# Bind against the C ABI target from the top-level CMakeLists.
target_link_libraries(_prepay PRIVATE prepay)

# So the extension finds libprepay.so sitting next to it in the installed package.
set_target_properties(_prepay PROPERTIES INSTALL_RPATH "$ORIGIN")

# Everything lands under prepay/ in the wheel: the extension, the shared lib,
# and the pure-Python package files.
install(TARGETS _prepay LIBRARY DESTINATION prepay)
install(TARGETS prepay  LIBRARY DESTINATION prepay)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/prepay/ DESTINATION prepay)
