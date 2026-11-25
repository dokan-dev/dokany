# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dokan Project Contributors

cmake_minimum_required(VERSION 3.22)

project(dokan_fstest LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR
      "dokan_fstest targets Windows NTFS/Win32 APIs and requires WIN32.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
  # Static CRT (/MT in Release, /MTd in Debug) so the produced executable has
  # zero VC++ redistributable dependencies across Windows Server 2016 through
  # 2025. Guarded by PROJECT_IS_TOP_LEVEL so embedders (e.g. default /MD) do
  # not hit LNK2038 or have their cache mutated.
  if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  # Default single-config generators (e.g. Ninja) to Release when unspecified.
  get_property(_fstest_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(NOT _fstest_is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build configuration" FORCE)
  endif()

  # Uniform output directory across Ninja (single-config) and Visual Studio
  # (multi-config): <build>/bin/<Config>/dokan_fstest.exe.
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
endif()

# ---------------------------------------------------------------------------
# Dependency Pins (Single Source of Truth: deps.json)
# ---------------------------------------------------------------------------
set(_fstest_deps_json_path "${CMAKE_CURRENT_SOURCE_DIR}/deps.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${_fstest_deps_json_path}")
file(READ "${_fstest_deps_json_path}" _fstest_deps_json)
string(JSON _fstest_gtest_version GET "${_fstest_deps_json}" googletest version)
string(JSON _fstest_gtest_url_default GET "${_fstest_deps_json}" googletest url)
string(JSON _fstest_gtest_sha256 GET "${_fstest_deps_json}" googletest sha256)
string(JSON _fstest_json_version GET "${_fstest_deps_json}" nlohmann_json version)
string(JSON _fstest_json_url_default GET "${_fstest_deps_json}" nlohmann_json url)
string(JSON _fstest_json_sha256 GET "${_fstest_deps_json}" nlohmann_json sha256)

# ---------------------------------------------------------------------------
# Build Options
# ---------------------------------------------------------------------------
option(FSTEST_WERROR "Treat compiler warnings as errors" OFF)
option(FSTEST_BUILD_CLI "Build dokan_fstest CLI and unit self-tests" ON)

option(FSTEST_USE_SYSTEM_GTEST
       "Use find_package(GTest REQUIRED) instead of FetchContent" OFF)
set(FSTEST_GTEST_SOURCE_DIR "" CACHE PATH
    "Optional local path to GoogleTest source tree")
set(FSTEST_GTEST_URL
    "${_fstest_gtest_url_default}"
    CACHE STRING "GoogleTest v${_fstest_gtest_version} archive URL or local file path")

option(FSTEST_USE_SYSTEM_JSON
       "Use find_package(nlohmann_json REQUIRED) instead of FetchContent" OFF)
set(FSTEST_JSON_SOURCE_DIR "" CACHE PATH
    "Optional local path to nlohmann/json include or root directory")
set(FSTEST_JSON_URL
    "${_fstest_json_url_default}"
    CACHE STRING "nlohmann/json v${_fstest_json_version} include.zip URL or local file path")

# Portability / embedder compile-check knobs.
set(FSTEST_WIN32_WINNT "0x0A00" CACHE STRING
    "Target _WIN32_WINNT macro value for core and suite")
set(FSTEST_NTDDI_VERSION "0x0A000002" CACHE STRING
    "Target NTDDI_VERSION macro value (empty string to omit)")
option(FSTEST_DEFINE_UNDERSCORE_UNICODE
       "Define _UNICODE in addition to UNICODE on core and suite" ON)
option(FSTEST_CORE_NO_EXCEPTIONS
       "Compile core, suite, and in-tree gtest with exceptions disabled" OFF)

if(FSTEST_CORE_NO_EXCEPTIONS AND FSTEST_BUILD_CLI)
  message(FATAL_ERROR
      "FSTEST_CORE_NO_EXCEPTIONS=ON requires -DFSTEST_BUILD_CLI=OFF "
      "(CLI targets require C++ exceptions for JSON serialization and error handling)")
endif()

set(_fstest_fetch_ts_arg)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
  set(_fstest_fetch_ts_arg DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# ---------------------------------------------------------------------------
# 3-Tier GoogleTest v1.17.0 Acquisition
# ---------------------------------------------------------------------------
if(FSTEST_CORE_NO_EXCEPTIONS)
  # Strip CMake's default /EHsc and /GR before acquiring GoogleTest so in-tree
  # gtest targets do not inherit conflicting exception or RTTI flags.
  string(REGEX REPLACE "[ \t]+" " " CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} ")
  string(REGEX REPLACE " [/-]EH[a-zA-Z0-9-]* " " " CMAKE_CXX_FLAGS
                       "${CMAKE_CXX_FLAGS}")
  string(REGEX REPLACE " [/-]GR-? " " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(STRIP "${CMAKE_CXX_FLAGS}" CMAKE_CXX_FLAGS)
endif()

set(_fstest_acquired_gtest FALSE)
if(TARGET GTest::gtest)
  if(FSTEST_BUILD_CLI AND NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR
        "TARGET GTest::gtest already exists, but FSTEST_BUILD_CLI=ON also "
        "requires TARGET GTest::gtest_main.")
  endif()
else()
  set(_fstest_acquired_gtest TRUE)
  # Ensure GoogleTest's option() calls honor our non-cache normal variables
  # (CMP0077) without polluting an embedding project's CMakeCache.
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  if(NOT DEFINED gtest_force_shared_crt)
    if(PROJECT_IS_TOP_LEVEL)
      set(gtest_force_shared_crt OFF)
    elseif(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY OR
           CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
      set(gtest_force_shared_crt ON)
    else()
      set(gtest_force_shared_crt OFF)
    endif()
  endif()
  if(NOT DEFINED BUILD_GMOCK)
    set(BUILD_GMOCK OFF)
  endif()
  if(NOT DEFINED INSTALL_GTEST)
    set(INSTALL_GTEST OFF)
  endif()

  set(_fstest_local_gtest_dir "${FSTEST_GTEST_SOURCE_DIR}")
  if(NOT _fstest_local_gtest_dir AND
     EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
    set(_fstest_local_gtest_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest")
  endif()

  if(FSTEST_USE_SYSTEM_GTEST)
    find_package(GTest REQUIRED)
  elseif(_fstest_local_gtest_dir)
    add_subdirectory("${_fstest_local_gtest_dir}"
                     "${CMAKE_BINARY_DIR}/_deps/googletest-build"
                     EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL "${FSTEST_GTEST_URL}"
        URL_HASH "SHA256=${_fstest_gtest_sha256}"
        ${_fstest_fetch_ts_arg})
    FetchContent_MakeAvailable(googletest)
  endif()
endif()

if(_fstest_acquired_gtest)
  foreach(_gt_target IN ITEMS gtest gtest_main)
    if(TARGET ${_gt_target})
      get_target_property(_gt_imported ${_gt_target} IMPORTED)
      if(NOT _gt_imported)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.25")
          set_target_properties(${_gt_target} PROPERTIES SYSTEM TRUE)
        endif()
        get_target_property(_gt_flags ${_gt_target} COMPILE_FLAGS)
        if(_gt_flags)
          string(REGEX REPLACE "[ \t]+" " " _gt_flags " ${_gt_flags} ")
          # Strip third-party -WX so clang-cl / newer MSVC warnings inside
          # GoogleTest do not break builds (our own targets keep /WX).
          string(REGEX REPLACE " [/-]WX " " " _gt_flags "${_gt_flags}")
          if(FSTEST_CORE_NO_EXCEPTIONS)
            # Strip GoogleTest's default -EHsc / -D_HAS_EXCEPTIONS=1 / -GR so
            # no-exceptions compiles without macro-redefinition or override errors.
            string(REGEX REPLACE " [/-]EH[a-zA-Z0-9-]* " " " _gt_flags
                                 "${_gt_flags}")
            string(REGEX REPLACE " [/-]D_HAS_EXCEPTIONS=[01] " " " _gt_flags
                                 "${_gt_flags}")
            string(REGEX REPLACE " [/-]GR-? " " " _gt_flags "${_gt_flags}")
          endif()
          string(STRIP "${_gt_flags}" _gt_flags)
          set_target_properties(${_gt_target} PROPERTIES
                                COMPILE_FLAGS "${_gt_flags}")
        endif()
        if(FSTEST_CORE_NO_EXCEPTIONS)
          if(MSVC)
            target_compile_options(${_gt_target} PRIVATE /EHs-c- /GR-)
          else()
            target_compile_options(${_gt_target} PRIVATE -fno-exceptions -fno-rtti)
          endif()
          if(_gt_target STREQUAL "gtest")
            target_compile_definitions(gtest PUBLIC _HAS_EXCEPTIONS=0
                                                    GTEST_HAS_EXCEPTIONS=0)
          endif()
        endif()
      endif()
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# 3-Tier nlohmann/json Acquisition (CLI and unit_tests ONLY)
# ---------------------------------------------------------------------------
if(FSTEST_BUILD_CLI)
  add_library(fstest_nlohmann_json INTERFACE)
  if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(fstest_nlohmann_json INTERFACE
                          nlohmann_json::nlohmann_json)
  elseif(FSTEST_USE_SYSTEM_JSON)
    find_package(nlohmann_json REQUIRED)
    target_link_libraries(fstest_nlohmann_json INTERFACE
                          nlohmann_json::nlohmann_json)
  elseif(FSTEST_JSON_SOURCE_DIR)
    if(EXISTS "${FSTEST_JSON_SOURCE_DIR}/include/nlohmann/json.hpp")
      target_include_directories(
          fstest_nlohmann_json SYSTEM
          INTERFACE "${FSTEST_JSON_SOURCE_DIR}/include")
    elseif(EXISTS "${FSTEST_JSON_SOURCE_DIR}/nlohmann/json.hpp")
      target_include_directories(
          fstest_nlohmann_json SYSTEM INTERFACE "${FSTEST_JSON_SOURCE_DIR}")
    else()
      message(FATAL_ERROR
          "FSTEST_JSON_SOURCE_DIR ('${FSTEST_JSON_SOURCE_DIR}') does not "
          "contain nlohmann/json.hpp or include/nlohmann/json.hpp")
    endif()
  else()
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL "${FSTEST_JSON_URL}"
        URL_HASH "SHA256=${_fstest_json_sha256}"
        ${_fstest_fetch_ts_arg})
    FetchContent_MakeAvailable(nlohmann_json)
    if(EXISTS "${nlohmann_json_SOURCE_DIR}/include/nlohmann/json.hpp")
      target_include_directories(
          fstest_nlohmann_json SYSTEM
          INTERFACE "${nlohmann_json_SOURCE_DIR}/include")
    elseif(EXISTS "${nlohmann_json_SOURCE_DIR}/nlohmann/json.hpp")
      target_include_directories(
          fstest_nlohmann_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}")
    else()
      message(FATAL_ERROR
          "Could not locate nlohmann/json.hpp inside '${nlohmann_json_SOURCE_DIR}'")
    endif()
  endif()
endif()
# ---------------------------------------------------------------------------
# Common Compiler Flags and Compile Definitions
# ---------------------------------------------------------------------------
function(fstest_apply_compiler_flags target_name)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
    # In clang-cl, bare -Wall maps to MSVC /Wall (-Weverything), so pass
    # Clang warnings explicitly via /clang: prefix.
    target_compile_options(${target_name} PRIVATE
        /W4 /utf-8 /clang:-Wall /clang:-Wextra)
    if(FSTEST_WERROR)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  elseif(MSVC)
    target_compile_options(${target_name} PRIVATE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
    if(FSTEST_WERROR)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE
        -Wall -Wextra -finput-charset=UTF-8)
    if(FSTEST_WERROR)
      target_compile_options(${target_name} PRIVATE -Werror)
    endif()
    if(FSTEST_CORE_NO_EXCEPTIONS)
      target_compile_options(${target_name} PRIVATE -fno-exceptions -fno-rtti)
    endif()
  endif()
endfunction()

set(_fstest_core_defs
    UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    "_WIN32_WINNT=${FSTEST_WIN32_WINNT}")
if(FSTEST_DEFINE_UNDERSCORE_UNICODE)
  list(APPEND _fstest_core_defs _UNICODE)
endif()
if(FSTEST_NTDDI_VERSION)
  list(APPEND _fstest_core_defs "NTDDI_VERSION=${FSTEST_NTDDI_VERSION}")
endif()

# ---------------------------------------------------------------------------
# Layer 1: ntfs_helpers (Pure C++20 + Win32 + dynamic ntdll, NO GTest/JSON)
# ---------------------------------------------------------------------------
add_library(ntfs_helpers STATIC
    core/src/ntdll.cpp
    core/src/status.cpp
    core/src/path.cpp
    core/src/file_ops.cpp
    core/src/file_ops_reparse.cpp
    core/src/file_ops_streams_ea.cpp
    core/src/security.cpp
    core/src/os_info.cpp
    core/src/capabilities.cpp
    core/src/temp_dir.cpp)

target_include_directories(ntfs_helpers PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core/include")
target_compile_definitions(ntfs_helpers PUBLIC ${_fstest_core_defs})
target_link_libraries(ntfs_helpers PUBLIC advapi32)
fstest_apply_compiler_flags(ntfs_helpers)
if(FSTEST_CORE_NO_EXCEPTIONS)
  if(MSVC)
    target_compile_options(ntfs_helpers PRIVATE /EHs-c- /GR-)
  endif()
  target_compile_definitions(ntfs_helpers PUBLIC _HAS_EXCEPTIONS=0)
endif()

# ---------------------------------------------------------------------------
# Layer 2a: ntfs_suite_base (Fixture, config, metadata — NO JSON, NO CLI)
# ---------------------------------------------------------------------------
add_library(ntfs_suite_base STATIC
    suite/src/test_config.cpp
    suite/src/test_fixture.cpp
    suite/src/test_metadata.cpp)

target_include_directories(ntfs_suite_base
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/suite/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/suite/src")
target_link_libraries(ntfs_suite_base PUBLIC ntfs_helpers GTest::gtest)
fstest_apply_compiler_flags(ntfs_suite_base)
if(FSTEST_CORE_NO_EXCEPTIONS)
  if(MSVC)
    target_compile_options(ntfs_suite_base PRIVATE /EHs-c- /GR-)
  endif()
  target_compile_definitions(ntfs_suite_base PUBLIC _HAS_EXCEPTIONS=0
                                                    GTEST_HAS_EXCEPTIONS=0)
endif()

# ---------------------------------------------------------------------------
# Layer 2b: ntfs_suite (All 12 NTFS conformance domains + registry anchor)
# ---------------------------------------------------------------------------
add_library(ntfs_suite STATIC
    suite/src/suite_registry.cpp
    suite/src/domains/create_open_test.cpp
    suite/src/domains/create_open_security_test.cpp
    suite/src/domains/delete_test.cpp
    suite/src/domains/dir_enum_test.cpp
    suite/src/domains/rename_move_test.cpp
    suite/src/domains/data_io_test.cpp
    suite/src/domains/data_io_sparse_mmap_test.cpp
    suite/src/domains/locks_oplocks_test.cpp
    suite/src/domains/timestamps_attrs_test.cpp
    suite/src/domains/streams_test.cpp
    suite/src/domains/streams_ops_test.cpp
    suite/src/domains/security_acl_test.cpp
    suite/src/domains/links_reparse_test.cpp
    suite/src/domains/naming_paths_test.cpp
    suite/src/domains/info_queries_test.cpp
    suite/src/domains/info_queries_notify_ea_test.cpp)

target_include_directories(ntfs_suite PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/suite/src")
target_link_libraries(ntfs_suite PUBLIC ntfs_suite_base)
fstest_apply_compiler_flags(ntfs_suite)
if(FSTEST_CORE_NO_EXCEPTIONS AND MSVC)
  target_compile_options(ntfs_suite PRIVATE /EHs-c- /GR-)
endif()

# ---------------------------------------------------------------------------
# Layer 3: dokan_fstest CLI & fstest_self_test unit tests
# ---------------------------------------------------------------------------
if(FSTEST_BUILD_CLI)
  add_library(fstest_cli_lib STATIC
      cli/src/cli_args.cpp
      cli/src/json_util.cpp
      cli/src/gtest_filter.cpp
      cli/src/baseline.cpp
      cli/src/baseline_update.cpp
      cli/src/scorecard.cpp
      cli/src/xfail_listener.cpp
      cli/src/mount_guard.cpp)

  target_include_directories(fstest_cli_lib PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/cli/src")
  target_link_libraries(fstest_cli_lib PUBLIC
      ntfs_suite_base
      ntfs_helpers
      GTest::gtest
      fstest_nlohmann_json)
  fstest_apply_compiler_flags(fstest_cli_lib)

  add_executable(dokan_fstest
      cli/src/main.cpp)

  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
    target_link_libraries(dokan_fstest PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,ntfs_suite>
        fstest_cli_lib)
  else()
    target_link_libraries(dokan_fstest PRIVATE
        ntfs_suite
        fstest_cli_lib)
  endif()
  fstest_apply_compiler_flags(dokan_fstest)
  if(MSVC)
    target_link_options(dokan_fstest PRIVATE
        $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>)
  endif()

  add_executable(fstest_self_test
      unit_tests/path_test.cpp
      unit_tests/temp_dir_test.cpp
      unit_tests/json_test.cpp
      unit_tests/baseline_test.cpp
      unit_tests/scorecard_test.cpp
      unit_tests/cli_args_test.cpp
      unit_tests/xfail_listener_test.cpp
      unit_tests/fixture_security_status_test.cpp
      unit_tests/gtest_filter_test.cpp
      unit_tests/core_helpers_test.cpp
      unit_tests/cli_e2e_test.cpp)
  add_dependencies(fstest_self_test dokan_fstest)

  target_compile_definitions(fstest_self_test PRIVATE
      "FSTEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
  target_link_libraries(fstest_self_test PRIVATE
      fstest_cli_lib
      GTest::gtest_main)
  fstest_apply_compiler_flags(fstest_self_test)
  if(MSVC)
    target_link_options(fstest_self_test PRIVATE
        $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>)
  endif()

  if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
  endif()
  add_test(NAME fstest_self_test COMMAND fstest_self_test)
endif()
