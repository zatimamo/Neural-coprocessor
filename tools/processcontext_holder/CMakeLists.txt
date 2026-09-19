# ============================================================================
# processcontext-holder - the PROCESS A helper for SPLITPROCESS_ISOLATION.
# Its own CMake project, built from its own directory, producing holder_ti.exe.
#
#   cmake -S tools/processcontext_holder -B build-holder -A x64
#   cmake --build build-holder --config Release
#
# NO DEPENDENCIES AT ALL, and that is the point. It does not include the NGX
# headers, it does not link MinHook, and it shares no source with
# tools/processcontext_ab. The only libraries are d3d12 and dxgi, because the
# only thing this program does is create a device, a DIRECT queue and a
# CBV/SRV/UAV heap and hold them.
#
# A configure that needed -DPCAB_NGX_DIR or -DPCAB_MINHOOK_DIR would mean the
# holder had grown a dependency on the diagnostic it is supposed to be
# independent of, so there is deliberately no such option here.
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(processcontext_holder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(holder_ti main.cpp)

target_compile_definitions(holder_ti PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  UNICODE
  _UNICODE)

# d3d12 and dxgi, and nothing else. In particular NOT nvapi64, NOT ngx, NOT
# MinHook: there is no import here that could reach a neural runtime.
target_link_libraries(holder_ti PRIVATE
  d3d12
  dxgi)

# A console application, deliberately: its stdout IS the protocol the parent
# waits on - HOLDER_READY, HOLDER_FAILED, HOLDER_STOPPED.
set_target_properties(holder_ti PROPERTIES
  OUTPUT_NAME "holder_ti"
  RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

message(STATUS "PROCESSCONTEXT-HOLDER: independent - d3d12 + dxgi only, no neural runtime, no detour library")
message(STATUS "PROCESSCONTEXT-HOLDER: output ${CMAKE_BINARY_DIR}/bin/holder_ti.exe")
