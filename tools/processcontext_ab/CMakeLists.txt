# ============================================================================
# PROCESSCONTEXT-AB - standalone diagnostic. Its own CMake project, built from
# its own directory, so that it cannot alter or be altered by the MGPU add-on's
# build. Layout: tools/processcontext_ab/ -> processcontext_ab.exe
#
#   cmake -S tools/processcontext_ab -B build-pcab -A x64 \
#         -DPCAB_NGX_DIR=<ext/ngx> -DPCAB_MINHOOK_DIR=<ext/minhook>
#   cmake --build build-pcab --config Release
#
# Both dependencies are PINNED by the workflow that builds this, and both are
# required: the NGX headers declare the feature ids and the parameter keys, and
# MinHook supplies the x64 relocation trampolines the two detours need. A
# configure without either fails here rather than producing a binary that
# silently cannot observe anything.
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(processcontext_ab CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(PCAB_NGX_DIR     "" CACHE PATH "NVIDIA NGX header tree holding nvsdk_ngx.h")
set(PCAB_MINHOOK_DIR "" CACHE PATH "MinHook tree: <dir>/include/MinHook.h and <dir>/lib/MinHook.lib")

if(NOT PCAB_NGX_DIR)
  message(FATAL_ERROR
    "PCAB_NGX_DIR is not set. Pass -DPCAB_NGX_DIR=<tree> holding nvsdk_ngx.h.")
endif()
if(NOT EXISTS "${PCAB_NGX_DIR}/nvsdk_ngx.h")
  message(FATAL_ERROR
    "PCAB_NGX_DIR=${PCAB_NGX_DIR} does not hold nvsdk_ngx.h.")
endif()

if(NOT PCAB_MINHOOK_DIR)
  message(FATAL_ERROR
    "PCAB_MINHOOK_DIR is not set. Pass -DPCAB_MINHOOK_DIR=<tree>.")
endif()
if(NOT EXISTS "${PCAB_MINHOOK_DIR}/include/MinHook.h")
  message(FATAL_ERROR
    "PCAB_MINHOOK_DIR=${PCAB_MINHOOK_DIR} does not hold include/MinHook.h.")
endif()
if(NOT EXISTS "${PCAB_MINHOOK_DIR}/lib/MinHook.lib")
  message(FATAL_ERROR
    "PCAB_MINHOOK_DIR=${PCAB_MINHOOK_DIR} does not hold lib/MinHook.lib. Build MinHook static "
    "first: cmake -S <minhook> -B <minhook>/build -A x64 -DBUILD_SHARED_LIBS=OFF, then copy the "
    "x64 .lib to <minhook>/lib/MinHook.lib.")
endif()

# The NGX header must still carry the feature this diagnostic creates. Pin drift
# here would silently produce a binary that asks for a different feature.
file(READ "${PCAB_NGX_DIR}/nvsdk_ngx_defs.h" PCAB_NGX_DEFS)
string(FIND "${PCAB_NGX_DEFS}" "NVSDK_NGX_Feature_Reserved18" PCAB_HAS_RESERVED18)
if(PCAB_HAS_RESERVED18 EQUAL -1)
  message(FATAL_ERROR
    "The pinned NGX headers do not declare NVSDK_NGX_Feature_Reserved18 - pin drift.")
endif()

add_executable(processcontext_ab
  main.cpp
  nv.cpp
  observe.cpp)

target_include_directories(processcontext_ab PRIVATE
  "${PCAB_NGX_DIR}"
  "${PCAB_MINHOOK_DIR}/include")

target_compile_definitions(processcontext_ab PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  UNICODE
  _UNICODE)

target_link_libraries(processcontext_ab PRIVATE
  "${PCAB_MINHOOK_DIR}/lib/MinHook.lib"
  d3d12
  dxgi
  bcrypt)

# A console application, deliberately: its stdout IS part of the deliverable.
set_target_properties(processcontext_ab PROPERTIES
  OUTPUT_NAME "processcontext_ab"
  RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

message(STATUS "PROCESSCONTEXT-AB: NGX headers from ${PCAB_NGX_DIR}")
message(STATUS "PROCESSCONTEXT-AB: MinHook from ${PCAB_MINHOOK_DIR}")
message(STATUS "PROCESSCONTEXT-AB: output ${CMAKE_BINARY_DIR}/bin/processcontext_ab.exe")
