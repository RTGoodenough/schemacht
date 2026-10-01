cmake_minimum_required(VERSION 3.21)

project(schemacht VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# The install rules default on only when this is the top-level project, not when schemacht is pulled in by another one.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(SCHEMACHT_IS_TOP_LEVEL ON)
else()
  set(SCHEMACHT_IS_TOP_LEVEL OFF)
endif()

option(SCHEMACHT_INSTALL
  "Generate the install rules: the library, its headers, a CMake package (find_package(schemacht)) and libpqxx, whose headers it needs"
  ${SCHEMACHT_IS_TOP_LEVEL})

# ==============================================================================
# Dependencies
# ==============================================================================
include(FetchContent)

# --- libpqxx: fetched and built from source. The system libpqxx-dev package ships headers whose std::source_location-gated
# exception constructors don't match what its pre-built .so exports (an Ubuntu packaging inconsistency), so it is built here with
# our own compiler. Its install rules run with ours (an installed schemacht is no use without the libpqxx headers it was built
# against), but not when schemacht is not being installed: a fetched project that is EXCLUDE_FROM_ALL has its install rules
# skipped (the keyword needs CMake 3.28; before that they run regardless). ---
set(SCHEMACHT_PQXX_SCOPE "")
if(NOT SCHEMACHT_INSTALL AND NOT CMAKE_VERSION VERSION_LESS 3.28)
  set(SCHEMACHT_PQXX_SCOPE EXCLUDE_FROM_ALL)
endif()
FetchContent_Declare(
  pqxx
  GIT_REPOSITORY https://github.com/jtv/libpqxx.git
  GIT_TAG 7.10.7  # postgres/connection.hpp checks its headers are this minor version: change both together
  GIT_SHALLOW TRUE
  ${SCHEMACHT_PQXX_SCOPE}
)
set(PQXX_LIBTYPE STATIC CACHE STRING "" FORCE)
set(SKIP_BUILD_TEST ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pqxx)

# After libpqxx, which includes it too and derives its documentation directory from its own project name; included first,
# libpqxx's documents would be installed as schemacht's.
include(GNUInstallDirs)

# --- libpq: used directly by postgres::Async_Client for non-blocking IO (libpqxx is blocking-only for queries). Streamed results
# use libpq 17's chunked rows mode (PQsetChunkedRowsMode) and non-blocking cancel (PQcancelCreate); Async_Client checks the libpq
# it loads at run time too. ---
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
  src/field_codec.cpp
  src/postgres/async_client.cpp
  src/postgres/sql_utils.cpp
)

# The headers are found in the source tree while building here and under the install prefix once installed.
target_include_directories(
  schemacht
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(schemacht PUBLIC pqxx PostgreSQL::PostgreSQL Threads::Threads)

# The headers use C++23 (concepts, std::move_only_function, ...), so whoever includes them compiles as C++23 too.
target_compile_features(schemacht PUBLIC cxx_std_23)

# The name a project links, whether it builds schemacht (add_subdirectory) or finds it installed (find_package).
add_library(schemacht::schemacht ALIAS schemacht)
set_target_properties(schemacht PROPERTIES EXPORT_NAME schemacht)

# ==============================================================================
# Install
# ==============================================================================
# `cmake --install <build> --prefix <dir>` puts under <dir>:
#   lib/libschemacht.a                   the library
#   include/schemacht/...                its headers, and include/pqxx/ (libpqxx's)
#   lib/cmake/schemacht/                 the package: find_package(schemacht) gives schemacht::schemacht
#   lib/cmake/libpqxx/, lib/libpqxx*.a   libpqxx, built for this compiler, which the package finds beside itself
# Everything the package refers to is relative to where its files are, so an installed tree can be moved.
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
