# ============================================================================
# nr-worker - the OUT-OF-PROCESS neural lane, its control protocol, and the
#             cross-adapter transport benchmark.
#
#   cmake -S tools/nr_worker -B build-nr -A x64 \
#         -DPCAB_NGX_DIR=<ext/ngx> -DPCAB_MINHOOK_DIR=<ext/minhook>
#   cmake --build build-nr --config Release
#
# FOUR TARGETS
#   mgpu_nr_worker.exe      the RTX 4070 lane, in its own process. The game side
#                           must never create that device, that NGX session or
#                           that Reserved18 feature.
#   nr_selftest.exe         GPU-free tests: control protocol v1, a real named-pipe
#                           handshake, the frame-slot pipeline, the JSON writer.
#   nr_ipc_probe.exe        the client half of the protocol, used by CI against
#                           the real worker and later by the game side.
#   nr_transport_bench.exe  the standalone two-GPU round trip. Standalone: it
#                           loads no NGX and no NVAPI.
#
# WHY THE PROCESSCONTEXT SOURCES ARE COMPILED IN HERE
#   The worker's lane must BE the proven lane, not a second interpretation of it.
#   nv.cpp and observe.cpp are the PROCESSCONTEXT-AB translation units - NVAPI,
#   the architecture cache/patch, the logging, the file hash and the two private
#   CUDA-interop observers - compiled VERBATIM into this target from
#   ../processcontext_ab/. That directory is frozen by the processcontext-ab
#   workflow; nothing here modifies it, and a CI gate fails if either copy of
#   those files differs from the frozen blobs.
#
#   The creation contract and the step order are asserted equal to the frozen
#   lane's by another CI gate, key for key and line for line.
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(nr_worker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(PCAB_NGX_DIR     "" CACHE PATH "NVIDIA NGX header tree holding nvsdk_ngx.h")
set(PCAB_MINHOOK_DIR "" CACHE PATH "MinHook tree: <dir>/include/MinHook.h and <dir>/lib/MinHook.lib")

if(NOT PCAB_NGX_DIR OR NOT EXISTS "${PCAB_NGX_DIR}/nvsdk_ngx.h")
  message(FATAL_ERROR
    "PCAB_NGX_DIR must hold nvsdk_ngx.h. The worker's lane is the proven lane, "
    "and that lane needs the pinned NGX headers.")
endif()
if(NOT PCAB_MINHOOK_DIR OR NOT EXISTS "${PCAB_MINHOOK_DIR}/lib/MinHook.lib")
  message(FATAL_ERROR
    "PCAB_MINHOOK_DIR must hold include/MinHook.h and lib/MinHook.lib. nv.cpp "
    "detours nvapi_QueryInterface and NvAPI_GPU_GetArchInfo, which needs MinHook.")
endif()

set(NR_PROVEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../processcontext_ab")

# The frozen translation units, compiled here as they are. Not copied, not
# re-implemented: the SAME files, so a fix to the proven layer is a fix to both.
set(NR_PROVEN_SOURCES
  "${NR_PROVEN_DIR}/nv.cpp"
  "${NR_PROVEN_DIR}/observe.cpp")

file(GLOB NR_PROVEN_HEADERS
  "${NR_PROVEN_DIR}/nv.h"
  "${NR_PROVEN_DIR}/observe.hpp")

# ---------------------------------------------------------------- the worker
add_executable(mgpu_nr_worker
  nr_worker.cpp
  nr_lane.cpp
  nr_proto.cpp
  nr_pipe.cpp
  nr_slots.cpp
  nr_json.cpp
  ${NR_PROVEN_SOURCES})

# ---------------------------------------------------- GPU-free self-test
add_executable(nr_selftest
  nr_selftest.cpp
  nr_proto.cpp
  nr_pipe.cpp
  nr_slots.cpp
  nr_json.cpp)

# ------------------------------------------------------------- the IPC client
add_executable(nr_ipc_probe
  nr_ipc_probe.cpp
  nr_proto.cpp
  nr_pipe.cpp
  nr_json.cpp)

# --------------------------------------------------------- the transport bench
add_executable(nr_transport_bench
  nr_bench.cpp
  nr_transport.cpp
  nr_slots.cpp
  nr_json.cpp)

foreach(TARGET mgpu_nr_worker nr_selftest nr_ipc_probe nr_transport_bench)
  target_include_directories(${TARGET} PRIVATE
    "${NR_PROVEN_DIR}"
    "${PCAB_NGX_DIR}"
    "${PCAB_MINHOOK_DIR}/include")
  target_compile_definitions(${TARGET} PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE)
  set_target_properties(${TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
endforeach()

# The worker is the only target that touches NVAPI, NGX, MinHook and bcrypt,
# because it is the only one that runs the lane. The benchmark deliberately links
# d3d12 and dxgi only: "NR is not involved in this test" has to be true of the
# binary as well as of the prose. The two GPU-free tools link nothing but the CRT
# and kernel32, which is what makes them runnable anywhere.
target_link_libraries(mgpu_nr_worker PRIVATE
  "${PCAB_MINHOOK_DIR}/lib/MinHook.lib"
  d3d12 dxgi bcrypt shell32)

target_link_libraries(nr_transport_bench PRIVATE
  d3d12 dxgi bcrypt)

message(STATUS "NR-WORKER: proven layer from ${NR_PROVEN_DIR}")
message(STATUS "NR-WORKER: NGX headers from ${PCAB_NGX_DIR}")
message(STATUS "NR-WORKER: output ${CMAKE_BINARY_DIR}/bin/mgpu_nr_worker.exe and three tools")
