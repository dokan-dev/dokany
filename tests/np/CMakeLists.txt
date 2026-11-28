# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dokan Project Contributors

cmake_minimum_required(VERSION 3.22)

project(dokan_np_tests LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR
      "dokan_np_tests targets Windows MPR/Win32 APIs and requires WIN32.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
  # Static CRT (/MT in Release, /MTd in Debug) so test binaries have zero
  # VC++ redistributable dependencies across Windows Server 2016 through 2025.
  if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  get_property(_np_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(NOT _np_is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build configuration" FORCE)
  endif()

  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
endif()

# ---------------------------------------------------------------------------
# Dependency Pins (Shared Single Source of Truth: ../fstest/deps.json)
# ---------------------------------------------------------------------------
set(_np_deps_json_path "${CMAKE_CURRENT_SOURCE_DIR}/../fstest/deps.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${_np_deps_json_path}")
file(READ "${_np_deps_json_path}" _np_deps_json)
string(JSON _np_gtest_version GET "${_np_deps_json}" googletest version)
string(JSON _np_gtest_url_default GET "${_np_deps_json}" googletest url)
string(JSON _np_gtest_filename GET "${_np_deps_json}" googletest filename)
string(JSON _np_gtest_sha256 GET "${_np_deps_json}" googletest sha256)

# ---------------------------------------------------------------------------
# Build Options
# ---------------------------------------------------------------------------
option(NP_TEST_WERROR "Treat compiler warnings as errors" OFF)
option(NP_TEST_USE_SYSTEM_GTEST
       "Use find_package(GTest REQUIRED) instead of FetchContent" OFF)
set(NP_TEST_GTEST_SOURCE_DIR "" CACHE PATH
    "Optional local path to GoogleTest source tree")
set(NP_TEST_GTEST_URL
    "${_np_gtest_url_default}"
    CACHE STRING "GoogleTest v${_np_gtest_version} archive URL or local file path")
set(DOKAN_DEPS_CACHE_DIR "" CACHE PATH
    "Optional local cache directory containing pinned dependency archives")

set(_np_fetch_ts_arg)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
  set(_np_fetch_ts_arg DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# ---------------------------------------------------------------------------
# 3-Tier GoogleTest Acquisition (matches tests/fstest)
# ---------------------------------------------------------------------------
set(_np_acquired_gtest FALSE)
if(NOT TARGET GTest::gtest)
  set(_np_acquired_gtest TRUE)
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

  set(_np_local_gtest_dir "${NP_TEST_GTEST_SOURCE_DIR}")
  if(NOT _np_local_gtest_dir AND
     EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../fstest/third_party/googletest/CMakeLists.txt")
    set(_np_local_gtest_dir
        "${CMAKE_CURRENT_SOURCE_DIR}/../fstest/third_party/googletest")
  endif()

  set(_np_gtest_url_resolved "${NP_TEST_GTEST_URL}")
  if(DOKAN_DEPS_CACHE_DIR AND
     _np_gtest_url_resolved STREQUAL _np_gtest_url_default)
    set(_np_cached_gtest "${DOKAN_DEPS_CACHE_DIR}/${_np_gtest_filename}")
    if(EXISTS "${_np_cached_gtest}")
      file(TO_CMAKE_PATH "${_np_cached_gtest}" _np_gtest_url_resolved)
    endif()
  endif()

  if(NP_TEST_USE_SYSTEM_GTEST)
    find_package(GTest REQUIRED)
  elseif(_np_local_gtest_dir)
    add_subdirectory("${_np_local_gtest_dir}"
                     "${CMAKE_BINARY_DIR}/_deps/googletest-build"
                     EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL "${_np_gtest_url_resolved}"
        URL_HASH "SHA256=${_np_gtest_sha256}"
        ${_np_fetch_ts_arg})
    FetchContent_MakeAvailable(googletest)
  endif()
endif()

if(_np_acquired_gtest)
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
          string(REGEX REPLACE " [/-]WX " " " _gt_flags "${_gt_flags}")
          string(STRIP "${_gt_flags}" _gt_flags)
          set_target_properties(${_gt_target} PROPERTIES
                                COMPILE_FLAGS "${_gt_flags}")
        endif()
      endif()
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Compiler Flags and Compile Definitions
# ---------------------------------------------------------------------------
function(np_apply_compiler_flags target_name is_c_target)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
    target_compile_options(${target_name} PRIVATE
        /W4 /utf-8 /clang:-Wall /clang:-Wextra)
    if(NP_TEST_WERROR)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  elseif(MSVC)
    if(is_c_target)
      target_compile_options(${target_name} PRIVATE /W4 /utf-8 /MP)
    else()
      target_compile_options(${target_name} PRIVATE
          /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
    endif()
    if(NP_TEST_WERROR)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE
        -Wall -Wextra -finput-charset=UTF-8)
    if(NP_TEST_WERROR)
      target_compile_options(${target_name} PRIVATE -Werror)
    endif()
  endif()
endfunction()

set(_np_common_defs
    UNICODE
    _UNICODE
    NOMINMAX
    "_WIN32_WINNT=0x0A00"
    "NTDDI_VERSION=0x0A000002")

set(_np_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ---------------------------------------------------------------------------
# System Under Test (compiled directly for Tier 1 hermetic tests)
# ---------------------------------------------------------------------------
add_library(dokan_np_core_sut STATIC
    "${_np_repo_root}/dokan_np/np_mount_table.c"
    "${_np_repo_root}/dokan_np/np_core.c"
    "${_np_repo_root}/dokan_np/np_transport_live.c"
    "${_np_repo_root}/dokan/np_registry.c")

target_include_directories(dokan_np_core_sut PUBLIC
    "${_np_repo_root}/dokan"
    "${_np_repo_root}/sys"
    "${_np_repo_root}/dokan_np")
target_compile_definitions(dokan_np_core_sut PUBLIC ${_np_common_defs})
target_link_libraries(dokan_np_core_sut PUBLIC kernel32 advapi32)
np_apply_compiler_flags(dokan_np_core_sut TRUE)

# ---------------------------------------------------------------------------
# Tier 1: Hermetic Unit Tests (dokan_np_unit_test.exe)
# ---------------------------------------------------------------------------
add_executable(dokan_np_unit_test
    unit/np_table_test.cpp
    unit/np_core_test.cpp
    unit/np_fuzz_test.cpp
    unit/np_registry_test.cpp)

target_include_directories(dokan_np_unit_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/unit")
target_link_libraries(dokan_np_unit_test PRIVATE
    dokan_np_core_sut
    GTest::gtest_main
    kernel32
    advapi32)
np_apply_compiler_flags(dokan_np_unit_test FALSE)
if(MSVC)
  target_link_options(dokan_np_unit_test PRIVATE
      $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>)
endif()

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
endif()
add_test(NAME dokan_np_unit_test COMMAND dokan_np_unit_test)

# ---------------------------------------------------------------------------
# Tier 2: Real-OS mpr.dll + Shell Integration Suite (dokan_np_test.exe)
# ---------------------------------------------------------------------------
add_executable(dokan_np_test
    integration/main.cpp
    integration/np_test_env.cpp
    integration/np_mpr_test.cpp
    integration/np_shell_test.cpp
    integration/np_session_test.cpp
    integration/np_hygiene_test.cpp)

target_include_directories(dokan_np_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/integration"
    "${_np_repo_root}/dokan"
    "${_np_repo_root}/sys"
    "${_np_repo_root}/dokan_np")
target_compile_definitions(dokan_np_test PRIVATE ${_np_common_defs})
target_link_libraries(dokan_np_test PRIVATE
    dokan_np_core_sut
    GTest::gtest
    mpr
    uuid
    advapi32
    kernel32)
np_apply_compiler_flags(dokan_np_test FALSE)
if(MSVC)
  target_link_options(dokan_np_test PRIVATE
      $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>)
endif()
