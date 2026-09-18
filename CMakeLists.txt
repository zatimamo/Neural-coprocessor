# MGPU Bridge - Cross-Adapter Bridge, Milestone 0 (Gate P0)
#
# Target: mgpu_bridge.addon64  (a ReShade 64-bit add-on, API 20)
#
# The .addon64 extension REPLACES .dll - the output is NOT
# mgpu_bridge.addon64.dll. ReShade loads *.addon64 files; a file with the
# wrong name goes green in CI and silently never loads on the rig. CI
# verifies the exact output name for this reason.

cmake_minimum_required(VERSION 3.21)

project(mgpu_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source list follows the closed manifest (P0_RECORD.md, section 01):
# T2 adds diag/adapter, T3 adds worker/gpu1_context. runtime_probe lands
# with its own task; assets/ is deployed by hand and never compiled.
add_library(mgpu_bridge SHARED
    src/dllmain.cpp
    src/diag.cpp
    src/adapter.cpp
    src/adapter_selection.cpp
    src/worker.cpp
    src/gpu1_context.cpp
    # P9.1: the lateral acquisition probe. Self-contained - it holds every
    # ReShade event callback the probe needs and nothing else, so
    # gpu1_context.cpp keeps its rule of holding no ReShade types and is
    # not touched by this feature at all.
    src/probe.cpp
    # R101: the calibrator. Reads the parameter block the game already hands
    # DLSS every frame, so acquisition stops being inferred. Self-contained
    # like probe.cpp - it holds the import-table interception and the reader
    # and nothing else, so neither gpu1_context nor probe gains a dependency.
    src/calibrator.cpp
    # R108: the idle screen. Draws with scissored clears only - no shader,
    # no font file, no ImGui - so it renders in exactly the situation where
    # nothing else can: an empty reshade-shaders folder.
    src/screen.cpp
    # SL1: the Streamline interposer probe. Self-contained like the three
    # above - it holds no ReShade types and no gpu1_context types, every
    # interface arrives as a void*, and the whole instrument is one
    # documented QueryInterface. No new link library: version.dll is
    # resolved at run time, the way d3dcompiler_47 already is.
    src/sl_probe.cpp
    # SLT1: the Streamline tag tap. Self-contained like the four above - no
    # ReShade type, no gpu1_context type, no Streamline header and no link
    # library; every Streamline struct is read by byte offset, and every
    # offset is quoted from the public header beside it. OFF unless
    # SLTags= is set in mgpu.ini, so this source compiles into every build
    # and touches nothing in any of them until it is asked for.
    src/sl_tags.cpp
    # ARCHTEST: the architecture A/B experiment. Self-contained like the five
    # above - it holds its own NVAPI interception and no ReShade or
    # gpu1_context types, so neither gains a dependency. It is compiled into
    # BOTH variants; MGPU_ARCH_COMPAT decides whether it intercepts anything.
    # No new link library: nvapi64.dll is reached with LoadLibraryW and
    # GetProcAddress, the way the Reflex block in gpu1_context already does.
    src/arch_test.cpp
)

target_include_directories(mgpu_bridge PRIVATE
    "ext/reshade/include"
)

# ---------------------------------------------------------------------------
# P7.1. The overlay panel's only dependency: two ImGui headers.
#
# "ext/reshade/deps/imgui" used to be listed above and did nothing, because CMake
# silently ignores an include directory that does not exist and __has_include
# then just says no - which is how a correct-looking CMakeLists edit produced no
# change at all in the log. ext/reshade is created by CI from the pinned SHA and
# is not in the repo, so nothing was ever there to find.
#
# We do not vendor ReShade and we compile no ImGui source. reshade.hpp supplies
# the whole ImGui API as a table of function pointers the ReShade runtime fills
# in at register_addon time; imgui.h is needed only for the declarations,
# IMGUI_VERSION_NUM and the struct layouts. So the dependency is imgui.h +
# imconfig.h, fetched into the BUILD tree - no submodule, no build step, and
# nothing third-party committed to this repo.
#
# The version is not negotiable: reshade_overlay.hpp #errors on anything but
# 19250 (ImGui 1.92.5), because the function table is a version-numbered struct
# of raw pointers and a near-miss is a silent ABI mismatch. 3912b3d9 is the
# commit ReShade 18deaa52de0c425a78b329e9cb3c497281cd00ec pins at deps/imgui.
#
# If the fetch fails the build still succeeds - it logs NO OVERLAY PANEL IN THIS
# BUILD and the CTRL+ALT+F8/F9/F11 hotkeys remain the control. The configure
# output below says which of the two you are getting, BEFORE you build.
set(MGPU_IMGUI_SHA "3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c")
set(MGPU_IMGUI_DIR "${CMAKE_BINARY_DIR}/ext/imgui")
foreach(_h imgui.h imconfig.h)
    if(NOT EXISTS "${MGPU_IMGUI_DIR}/${_h}")
        file(DOWNLOAD
             "https://raw.githubusercontent.com/ocornut/imgui/${MGPU_IMGUI_SHA}/${_h}"
             "${MGPU_IMGUI_DIR}/${_h}"
             STATUS _mgpu_dl TLS_VERIFY ON)
        list(GET _mgpu_dl 0 _mgpu_code)
        if(NOT _mgpu_code EQUAL 0)
            list(GET _mgpu_dl 1 _mgpu_msg)
            message(WARNING "MGPU: could not fetch ${_h} (${_mgpu_msg}) - building without the overlay panel.")
            file(REMOVE "${MGPU_IMGUI_DIR}/${_h}")
        endif()
    endif()
endforeach()

if(EXISTS "${MGPU_IMGUI_DIR}/imgui.h" AND EXISTS "${MGPU_IMGUI_DIR}/imconfig.h")
    target_include_directories(mgpu_bridge PRIVATE "${MGPU_IMGUI_DIR}")
    message(STATUS "MGPU: overlay panel ENABLED - ImGui headers at ${MGPU_IMGUI_DIR}")
else()
    message(STATUS "MGPU: overlay panel DISABLED - no ImGui headers; hotkeys only.")
endif()
# ---------------------------------------------------------------------------

set_target_properties(mgpu_bridge PROPERTIES
    PREFIX ""
    SUFFIX ".addon64"
    # $<1:...> suppresses the per-configuration subdirectory that
    # multi-config generators (including "Visual Studio 17 2022", which
    # CI uses) would otherwise append to RUNTIME_OUTPUT_DIRECTORY: the
    # output is build/mgpu_bridge.addon64, not build/Release/...
    # CI locates the artifact by name regardless; this makes the stated
    # path true.
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>"
)

target_include_directories(mgpu_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # CI creates ext/reshade/ from crosire/reshade at the pinned SHA
    # (see .github/workflows/build.yml and VENDOR_LOCK.md).
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/reshade
)

target_compile_definitions(mgpu_bridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

# dxgi: factory + EnumAdapters1 (T2). d3d12: the private device (T3).
target_link_libraries(mgpu_bridge PRIVATE
    dxgi
    d3d12
    # V49/V51: the ghost mode. DirectComposition, and D3D11 solely to name an
    # adapter to it - nothing is ever drawn with the D3D11 device. Both are
    # also declared with #pragma comment(lib, ...) inside gpu1_context.cpp, so
    # a build made from src/* without this file still links.
    dcomp
    d3d11
)

# P0: warnings visible, not fatal. A compile error costs the same round
# trip as a logic error and teaches less - keep the first-try bar low.
target_compile_options(mgpu_bridge PRIVATE /W3)

# ---------------------------------------------------------------------------
# ARCHTEST. The architecture A/B experiment.
#
# BOTH variants are built from THIS revision. The ONLY thing that differs is
# one compile definition:
#
#   MGPU_ARCH_COMPAT absent  ->  CONTROL. arch_test.cpp compiles to logging
#                                and identity reporting only; no detour is
#                                ever installed and no value is ever rewritten.
#   MGPU_ARCH_COMPAT defined ->  COMPAT.  the same file additionally detours
#                                nvapi_QueryInterface for the feature-creation
#                                scope and rewrites NvAPI_GPU_GetArchInfo for
#                                the selected neural GPU.
#
# The definition is passed in from the build system rather than written here,
# so one source revision yields both binaries with a single configure flag:
#
#   cmake -B build-control -A x64
#   cmake -B build-compat  -A x64 -DMGPU_ARCH_COMPAT=ON
#
# Nothing in the selection, transport, presentation, parameter or CreateFeature
# code is conditional on it.
# ---------------------------------------------------------------------------
option(MGPU_ARCH_COMPAT "ARCHTEST COMPAT variant: enable the NVAPI architecture hook" OFF)
if(MGPU_ARCH_COMPAT)
    target_compile_definitions(mgpu_bridge PRIVATE MGPU_ARCH_COMPAT=1)
    message(STATUS "MGPU: ARCHTEST variant = COMPAT (architecture hook enabled)")
else()
    message(STATUS "MGPU: ARCHTEST variant = CONTROL (no architecture modification)")
endif()

# ---------------------------------------------------------------------------
# ARCHTEST: MinHook, the x64 trampoline provider.
#
# The detour needs a callable ORIGINAL: the overwritten prologue copied
# elsewhere with every RIP-relative operand and relative branch relocated.
# Instruction-boundary safety alone does NOT make a copied x64 instruction
# relocatable, and that relocation is the entire problem, so it is delegated
# to MinHook (TsudaKageyu/minhook, BSD-2-Clause).
#
# CI fetches a PINNED MinHook, builds it static, and points this at it:
#     -DMGPU_MINHOOK_DIR=<tree>   (header at <tree>/include, lib at
#                                  <tree>/lib/MinHook.lib)
#
# It is OPTIONAL on purpose. When it is absent, src/arch_test.cpp still
# compiles and links: COMPAT logs "MinHook not available in this build" and
# performs NO interception, leaving the process on the original NVAPI entry.
# Fail closed - never patch, then call the patched entry.
# ---------------------------------------------------------------------------
set(MGPU_MINHOOK_DIR "" CACHE PATH "MinHook tree: <dir>/include/MinHook.h and <dir>/lib/MinHook.lib")
if(MGPU_MINHOOK_DIR)
    if(EXISTS "${MGPU_MINHOOK_DIR}/include/MinHook.h")
        target_include_directories(mgpu_bridge PRIVATE "${MGPU_MINHOOK_DIR}/include")
        message(STATUS "MGPU: MinHook headers found at ${MGPU_MINHOOK_DIR}/include")
    else()
        message(WARNING "MGPU: MGPU_MINHOOK_DIR set but ${MGPU_MINHOOK_DIR}/include/MinHook.h is missing - "
                        "ARCHTEST will fail closed and not intercept")
    endif()
    if(EXISTS "${MGPU_MINHOOK_DIR}/lib/MinHook.lib")
        target_link_libraries(mgpu_bridge PRIVATE "${MGPU_MINHOOK_DIR}/lib/MinHook.lib")
        message(STATUS "MGPU: MinHook library linked from ${MGPU_MINHOOK_DIR}/lib/MinHook.lib")
    else()
        message(WARNING "MGPU: ${MGPU_MINHOOK_DIR}/lib/MinHook.lib is missing - "
                        "ARCHTEST will fail to link or fail closed")
    endif()
else()
    message(STATUS "MGPU: MGPU_MINHOOK_DIR not set - ARCHTEST COMPAT will fail closed (no interception)")
endif()

option(MGPU_BUILD_TESTS "Build CPU-only adapter policy tests" OFF)
if(MGPU_BUILD_TESTS)
    enable_testing()
    add_executable(mgpu_adapter_selection_test
        tests/adapter_selection_test.cpp
        src/adapter_selection.cpp
    )
    target_include_directories(mgpu_adapter_selection_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME mgpu_adapter_selection_test
             COMMAND mgpu_adapter_selection_test)
endif()
