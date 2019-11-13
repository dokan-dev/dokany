cmake_minimum_required(VERSION 2.8.5)
project(dokanfuse1)
include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE)
    message("No CMAKE_BUILD_TYPE specified, defaulting to Release")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif(NOT CMAKE_BUILD_TYPE)

option(FUSE_PKG_CONFIG "Install a libfuse-compatible pkg-config file (fuse.pc)" ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -mwin32 -Wall")
add_definitions(-D_FILE_OFFSET_BITS=64)
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../sys
)

if(FUSE_PKG_CONFIG)
    # Defining helper-function to deal with setups that manually set an
    # absolute path for CMAKE_INSTALL_(LIB|INCLUDE)DIR...
    # We could also just make all paths absolute, but this way the
    # pkg-config-file is more human-readable and pkg-config may be able to deal
    # with varying prefixes.
    # CMake does not have a ternary operator for generator expressions, so this
    # looks more complicated than it is.
    function ( make_pkg_config_absolute out_path in_path )
        if(IS_ABSOLUTE "${${in_path}}")
            set(${out_path} "${${in_path}}" PARENT_SCOPE)
        else()
            set(${out_path} "\${prefix}/${${in_path}}" PARENT_SCOPE)
        endif()
    endfunction()

    set(pkg_config_file "${CMAKE_CURRENT_BINARY_DIR}/fuse.pc")
    make_pkg_config_absolute(PKG_CONFIG_LIBDIR CMAKE_INSTALL_LIBDIR)
    make_pkg_config_absolute(PKG_CONFIG_INCLUDEDIR CMAKE_INSTALL_INCLUDEDIR)

    CONFIGURE_FILE(
        "${CMAKE_CURRENT_SOURCE_DIR}/pkg-config.pc.in"
        ${pkg_config_file}
        @ONLY
    )
endif()

file(GLOB sources src/*.cpp src/*.c src/*.rc)
set(install_headers
    include/fuse.h
    include/fuse_common.h
    include/fuse_opt.h
    include/fuse_sem_fix.h
    include/fuse_win.h
    include/utils.h
)
set(compat_headers
    include/old/fuse.h
)
add_library(dokanfuse1 SHARED ${sources})

INSTALL(FILES ${install_headers} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fuse/)
INSTALL(FILES ${compat_headers} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(FUSE_PKG_CONFIG)
    INSTALL(FILES ${pkg_config_file} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()
INSTALL(TARGETS dokanfuse1
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
