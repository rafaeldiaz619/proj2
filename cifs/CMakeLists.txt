# //////////////////////////////////////////////////////////////////////////
# ///
# /// Copyright (c) 2020 Prof. AJ Bieszczad. All rights reserved.
# ///
# //////////////////////////////////////////////////////////////////////////
#/*
#* Rafael Diaz
#* Spring 2025
#* COMP 362 Section 1 - Operating Systems
#*/

cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(cifs VERSION 0.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_FILE_OFFSET_BITS=64")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Wall --pedantic -g -O0 -D_DEBUG -DNO_FUSE_DEBUG")
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake" ${CMAKE_MODULE_PATH})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

set(CMAKE_BUILD_TYPE Debug)


find_package(PkgConfig REQUIRED)
include(FindPkgConfig)

pkg_check_modules(FUSE3 REQUIRED fuse3)   # looks for fuse3.pc

message(STATUS ">>> Building with FUSE3 via pkg-config:")
message(STATUS "    include dirs: ${FUSE3_INCLUDE_DIRS}")
message(STATUS "    libraries:   ${FUSE3_LIBRARIES}")

include_directories(
  inc
  ${FUSE3_INCLUDE_DIRS}
)
link_directories(${FUSE3_LIBRARY_DIRS})
add_definitions(${FUSE3_CFLAGS_OTHER})

add_executable(cifs
src/test_cifs.c
src/cifs.c
)

target_link_libraries(cifs PRIVATE ${FUSE3_LIBRARIES})

# runs step 1
enable_testing()

add_executable(cifs_step1
src/test_cifs.c
src/cifs.c
)

target_compile_definitions(cifs_step1 PRIVATE RUN_ONLY_STEP1)
target_link_libraries(cifs_step1 PRIVATE ${FUSE3_LIBRARIES})

add_test(
NAME CIFS_Step1
COMMAND cifs_step1
)

add_executable(cifs_step2
        src/test_cifs.c
        src/cifs.c
)

target_compile_definitions(cifs_step2 PRIVATE RUN_ONLY_STEP2)
target_link_libraries(cifs_step2 PRIVATE ${FUSE3_LIBRARIES})

add_test(
   NAME CIFS_Step2
   COMMAND cifs_step2
)

add_executable(blockVolume src/blockVolume.c)
