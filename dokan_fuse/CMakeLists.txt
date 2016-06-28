cmake_minimum_required(VERSION 2.8.5)
project(dokanfuse1)
include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE)
    message("No CMAKE_BUILD_TYPE specified, defaulting to Release")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif(NOT CMAKE_BUILD_TYPE)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -mwin32 -Wall")
add_definitions(-D_FILE_OFFSET_BITS=64)
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../sys
)

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
INSTALL(TARGETS dokanfuse1
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
