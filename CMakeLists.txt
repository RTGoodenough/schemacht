cmake_minimum_required(VERSION 3.21)

project(schemacht VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Install rules, tests and examples default on only when this is the top-level project, not when schemacht is pulled in by another one.
option(SCHEMACHT_INSTALL "Generate the install rules: the library, its headers and a CMake package (find_package(schemacht))" ${PROJECT_IS_TOP_LEVEL})
option(SCHEMACHT_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(SCHEMACHT_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
set(SCHEMACHT_BUILD_DOCS "AUTO" CACHE STRING "Add the target `docs`, the API reference built from the headers with Doxygen: AUTO (if Doxygen is found), ON or OFF")
set_property(CACHE SCHEMACHT_BUILD_DOCS PROPERTY STRINGS AUTO ON OFF)

# ==============================================================================
# Dependencies
# ==============================================================================
# libpq 17 or newer: streamed results use its chunked rows mode (PQsetChunkedRowsMode) and non-blocking cancel (PQcancelCreate).
# AsyncClient also checks the libpq it loads at run time.
find_package(PostgreSQL REQUIRED)
set(SCHEMACHT_MIN_LIBPQ 17)
if(PostgreSQL_VERSION_STRING VERSION_LESS SCHEMACHT_MIN_LIBPQ)
  message(FATAL_ERROR "schemacht needs libpq ${SCHEMACHT_MIN_LIBPQ} or newer, found ${PostgreSQL_VERSION_STRING}. "
                      "Install libpq-dev from the PostgreSQL apt repository (https://apt.postgresql.org).")
endif()
find_package(Threads REQUIRED)

# ==============================================================================
# Library
# ==============================================================================
add_library(
  schemacht
  STATIC
  src/async/thread_pool.cpp
  src/postgres/async_client.cpp
  src/postgres/binary_form.cpp
  src/postgres/chrono_codec.cpp
  src/postgres/field_codec.cpp
  src/postgres/numeric.cpp
)

# The headers are found in the source tree while building here and under the install prefix once installed.
target_include_directories(
  schemacht
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(schemacht PUBLIC PostgreSQL::PostgreSQL Threads::Threads)

# The headers use C++20, so whoever includes them compiles as C++20 or later.
target_compile_features(schemacht PUBLIC cxx_std_20)

# The name a project links, whether it builds schemacht (add_subdirectory) or finds it installed (find_package).
add_library(schemacht::schemacht ALIAS schemacht)
set_target_properties(schemacht PROPERTIES EXPORT_NAME schemacht)

# ==============================================================================
# Tests and examples
# ==============================================================================
if(SCHEMACHT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(SCHEMACHT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ==============================================================================
# Documentation: the target `docs`, if Doxygen is found
# ==============================================================================
# The API reference, from the comments in include/, with README.md as its front page and docs/ as further pages, written to
# build/docs/html. No Doxyfile is kept: CMake's FindDoxygen writes one from the DOXYGEN_ variables below. Diagrams need
# Graphviz (dot); without it the reference is built without them.
string(TOUPPER "${SCHEMACHT_BUILD_DOCS}" SCHEMACHT_BUILD_DOCS_CHOICE)
if(SCHEMACHT_BUILD_DOCS_CHOICE STREQUAL "ON")
  find_package(Doxygen REQUIRED OPTIONAL_COMPONENTS dot)
elseif(SCHEMACHT_BUILD_DOCS_CHOICE STREQUAL "AUTO")
  find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)
elseif(NOT SCHEMACHT_BUILD_DOCS_CHOICE STREQUAL "OFF")
  message(FATAL_ERROR "SCHEMACHT_BUILD_DOCS is '${SCHEMACHT_BUILD_DOCS}': AUTO, ON or OFF")
endif()

if(DOXYGEN_FOUND AND NOT SCHEMACHT_BUILD_DOCS_CHOICE STREQUAL "OFF")
  set(DOXYGEN_PROJECT_BRIEF "tables as C++ types, SQL written while compiling, an asynchronous PostgreSQL client")
  set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
  set(DOXYGEN_STRIP_FROM_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
  set(DOXYGEN_STRIP_FROM_INC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set(DOXYGEN_EXAMPLE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/examples)
  set(DOXYGEN_EXTRACT_ALL NO)
  set(DOXYGEN_EXTRACT_PRIVATE NO)
  set(DOXYGEN_HIDE_UNDOC_MEMBERS YES)
  set(DOXYGEN_HIDE_UNDOC_CLASSES YES)
  set(DOXYGEN_EXCLUDE_SYMBOLS "detail" "*::detail" "*::detail::*" "insert_detail" "*::insert_detail" "*::insert_detail::*")
  set(DOXYGEN_PREDEFINED "__cplusplus=202002L")  # the headers' own guard against an older standard
  set(DOXYGEN_MARKDOWN_ID_STYLE GITHUB)  # so that a link to a heading in docs/ resolves as it does on GitHub
  set(DOXYGEN_JAVADOC_AUTOBRIEF NO)  # every brief is an explicit @brief; a sentence end inside a code span would otherwise cut one short
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_GENERATE_TREEVIEW YES)
  set(DOXYGEN_SHOW_INCLUDE_FILES NO)
  set(DOXYGEN_SORT_MEMBER_DOCS NO)
  set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
  set(DOXYGEN_WARN_AS_ERROR NO)
  set(DOXYGEN_QUIET YES)
  doxygen_add_docs(docs ${CMAKE_CURRENT_SOURCE_DIR}/include/schemacht ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${CMAKE_CURRENT_SOURCE_DIR}/docs
                   COMMENT "The API reference, with Doxygen, into ${CMAKE_BINARY_DIR}/docs/html")
endif()

# ==============================================================================
# Install
# ==============================================================================
# `cmake --install <build> --prefix <dir>` puts under <dir>:
#   lib/libschemacht.a        the library
#   include/schemacht/...     its headers
#   lib/cmake/schemacht/      the package: find_package(schemacht) gives schemacht::schemacht
if(SCHEMACHT_INSTALL)
  set(SCHEMACHT_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/schemacht)

  install(TARGETS schemacht EXPORT schemacht-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(EXPORT schemacht-targets NAMESPACE schemacht:: DESTINATION ${SCHEMACHT_INSTALL_CMAKEDIR})
  install(DIRECTORY include/schemacht DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")

  configure_package_config_file(
    cmake/schemacht-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/schemacht-config.cmake
    INSTALL_DESTINATION ${SCHEMACHT_INSTALL_CMAKEDIR}
  )
  # 0.x: a new minor version may change anything, so only a patch release is compatible.
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/schemacht-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/schemacht-config.cmake ${CMAKE_CURRENT_BINARY_DIR}/schemacht-config-version.cmake
          DESTINATION ${SCHEMACHT_INSTALL_CMAKEDIR})
endif()
