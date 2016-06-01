cmake_minimum_required(VERSION 2.8.4)

project (dokanfuse1)

set(CMAKE_LEGACY_CYGWIN_WIN32 0) 

if(NOT CMAKE_BUILD_TYPE)
    message("No CMAKE_BUILD_TYPE specified, defaulting to Release")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif(NOT CMAKE_BUILD_TYPE)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -mwin32")
add_definitions(-D_FILE_OFFSET_BITS=64)

file(GLOB source src/*.cpp src/*.c)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include
					${CMAKE_CURRENT_SOURCE_DIR}/../sys)

add_library(dokanfuse1 SHARED ${source})
