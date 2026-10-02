cmake_minimum_required(VERSION 3.21)

project(schemacht VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Install rules, tests and examples default on only when this is the top-level project, not when schemacht is pulled in by another one.
option(SCHEMACHT_INSTALL "Generate the install rules: the library, its headers and a CMake package (find_package(schemacht))" ${PROJECT_IS_TOP_LEVEL})
option(SCHEMACHT_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(SCHEMACHT_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})

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

# The headers use C++23, so whoever includes them compiles as C++23 too.
target_compile_features(schemacht PUBLIC cxx_std_23)

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
