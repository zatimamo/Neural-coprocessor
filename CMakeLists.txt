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
    # CREATECONTRACT: the creation-time parameter A/B. Compiled into BOTH
    # variants of that experiment; MGPU_CONTRACT_EXPANDED decides whether it
    # adds the extra keys or is a log line only. It writes interface values
    # into MGPU's existing parameter block and builds nothing.
    src/create_contract.cpp
    # NVAPIINIT: the explicit-NVAPI-initialization A/B. Compiled into BOTH
    # variants of that experiment; MGPU_NVAPI_EXPLICIT_INIT decides whether it
    # takes the single NvAPI reference or is a log line only. It uses MGPU's
    # existing dynamic-NVAPI route (LoadLibraryW + GetProcAddress +
    # nvapi_QueryInterface), so no new link library and no NVAPI header.
    src/nvapi_init.cpp
    # CUDADIAG: the two NVAPI CUDA-interop calls the private DLSS-NR runtime
    # makes inside its CreateFeature. Self-contained like the five above - it
    # reaches no NVAPI module itself: it is handed the GENUINE pointer the
    # existing ARCHTEST nvapi_QueryInterface detour already resolved, and hands
    # back a pass-through wrapper. No new link library, no NVAPI header, no
    # second detour and no hook on either CUDA entry point.
    #
    #   MGPU_CUDA_DIAG undefined -> this file is BEHAVIOURALLY INERT: select()
    #                               returns the real resolver result unchanged,
    #                               so the add-on behaves as it does without the
    #                               instrumentation. The file is still compiled
    #                               and linked; it is NOT claimed that the binary
    #                               is byte-identical to one built without it.
    #   MGPU_CUDA_DIAG defined   -> the two target interface ids are substituted
    #                               with observational pass-through wrappers.
    src/cuda_diag.cpp
    # RESHADENATIVE: the P1.0c NGX lane on the unwrapped original device. One
    # question - is ReShade's D3D12 descriptor virtualization what makes the
    # private NVAPI CUDA-interop lane return NVAPI_ERROR? Self-contained like
    # the six above: no ReShade header, no link library, and the one GUID it
    # carries is written out because ReShade's IID_UnwrappedObject is internal.
    src/reshade_native.cpp
    # APPID_NS: the NGX ApplicationId for the Reserved18 sessions. One number
    # and five log lines. It creates no object, resolves no entry point, loads
    # no module, writes no parameter and calls nothing in NGX or NVAPI; the
    # call sites in gpu1_context.cpp read the value from here instead of
    # writing a literal, so the id a session is initialised with and the id the
    # log states cannot drift apart.
    src/appid_ns.cpp
    # NR_RELAY: the game-side half of the OUT-OF-PROCESS neural lane. The whole
    # file is inside #ifdef MGPU_NR_RELAY, so without the definition it is an
    # empty translation unit and the add-on is byte-for-byte the build it was
    # before this file existed. It owns the IPC half and nothing of D3D12:
    # frames arrive as plain host pointers, four named file mappings carry them
    # to mgpu_nr_worker.exe, and the worker's own Reserved18 result is quoted
    # rather than interpreted. See the MGPU_NR_RELAY option block below, and
    # src/nr_relay.hpp for why the carrier is host memory and not a shared
    # surface. Nothing calls it yet; the frame-pipeline call site is a separate
    # piece of work written against that header.
    src/nr_relay.cpp
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
# CREATECONTRACT. The creation-time parameter contract A/B.
#
#   MGPU_CONTRACT_EXPANDED absent  ->  contract_control.
#                                      MGPU's existing CreateFeature
#                                      parameters, unchanged. create_contract.cpp
#                                      compiles to a single log line.
#   MGPU_CONTRACT_EXPANDED defined ->  contract_expanded.
#                                      the same parameters PLUS the additional
#                                      creation-time keys, logged one by one
#                                      immediately before CreateFeature.
#
# BOTH variants of THIS experiment keep the architecture COMPAT hook ON, so that
# the architecture spoof and the GPU identity are frozen exactly as they stand in
# the current known-clean baseline; changing that would vary two variables
# instead of one. That is a statement about the baseline, NOT a causal claim:
# whether Init returns Success is a RUN-TIME gate on every run, and a run whose
# P1.0c Init or P4.1 Init is FAIL_OutOfDate has a void NGX portion and is not a
# valid comparison, whatever the flags were. The CI job builds it that way.
# ---------------------------------------------------------------------------
option(MGPU_CONTRACT_EXPANDED
       "CREATECONTRACT expanded creation parameter variant"
       OFF)
if(MGPU_CONTRACT_EXPANDED)
    target_compile_definitions(mgpu_bridge PRIVATE MGPU_CONTRACT_EXPANDED=1)
    message(STATUS "MGPU: CREATECONTRACT variant = CONTRACT_EXPANDED")
else()
    message(STATUS "MGPU: CREATECONTRACT variant = CONTRACT_CONTROL")
endif()

# ---------------------------------------------------------------------------
# NVAPIINIT. The explicit-NVAPI-initialization A/B.
#
#   MGPU_NVAPI_EXPLICIT_INIT absent  ->  nvapi_control.
#                                        current behaviour: no explicit
#                                        NvAPI_Initialize anywhere on the path.
#   MGPU_NVAPI_EXPLICIT_INIT defined ->  nvapi_initialized.
#                                        NvAPI_Initialize once, before every
#                                        NVAPI call MGPU makes and before the
#                                        private runtime is loaded.
#
# BOTH variants of this experiment build with MGPU_ARCH_COMPAT=ON and
# MGPU_CONTRACT_EXPANDED=ON (the CI job sets all three flags), so the
# architecture spoof, GPU identity, caller, NR DLL, driver, expanded
# CreateFeature contract, resolution and transport are frozen. This definition
# is the only experimental variable.
# ---------------------------------------------------------------------------
option(MGPU_NVAPI_EXPLICIT_INIT
       "NVAPIINIT initialized variant: call NvAPI_Initialize explicitly before any NVAPI use"
       OFF)
if(MGPU_NVAPI_EXPLICIT_INIT)
    target_compile_definitions(mgpu_bridge PRIVATE MGPU_NVAPI_EXPLICIT_INIT=1)
    message(STATUS "MGPU: NVAPIINIT variant = NVAPI_INITIALIZED")
else()
    message(STATUS "MGPU: NVAPIINIT variant = NVAPI_CONTROL")
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

# ---------------------------------------------------------------------------
# CUDADIAG. The pass-through instrumentation for the two NVAPI CUDA-interop
# calls DLSS-NR issues inside CreateFeature.
#
#   MGPU_CUDA_DIAG undefined ->  this file is BEHAVIOURALLY INERT: select()
#                                returns the real resolver result unchanged, so
#                                the add-on behaves as it does without the
#                                instrumentation. The file is still compiled and
#                                linked; it is NOT claimed that the binary is
#                                byte-identical to one built without it.
#   MGPU_CUDA_DIAG defined   ->  the two target interface ids are substituted
#                                with observational pass-through wrappers. Every
#                                wrapper calls the genuine function exactly once,
#                                with the original arguments, and returns the
#                                genuine NvApi_Status verbatim.
#
# There is NO second variant: this is instrumentation, not an experiment, so the
# CI job produces ONE artifact and there is no compare job. It is built with
# MGPU_ARCH_COMPAT=ON for two reasons, neither of which is a claim about Init:
#   a) COMPAT=ON is the configuration of the current known-clean baseline, so the
#      diagnostic runs in the state that baseline was recorded in;
#   b) that definition owns the existing nvapi_QueryInterface detour, which is
#      the mechanism CUDA-DIAG uses to substitute a returned pointer. With it off
#      there is no detour, nothing is intercepted, and cuda_diag::log_variant()
#      reports the run as VOID for this diagnostic.
#
# ARCH_COMPAT is neither universally necessary nor sufficient for Init success.
# Init success is a RUN-TIME validity gate on every run: the NGX portion of a run
# whose P1.0c Init or P4.1 Init is FAIL_OutOfDate is void.
#
# Nothing in the selection, transport, presentation, parameter or CreateFeature
# code is conditional on it.
# ---------------------------------------------------------------------------
option(MGPU_CUDA_DIAG "CUDADIAG: instrument the two NVAPI CUDA-interop calls (observational)" OFF)
if(MGPU_CUDA_DIAG)
    target_compile_definitions(mgpu_bridge PRIVATE MGPU_CUDA_DIAG=1)
    message(STATUS "MGPU: CUDADIAG = ON (pass-through instrumentation of the two CUDA-interop ids)")
else()
    message(STATUS "MGPU: CUDADIAG = OFF (cuda_diag.cpp compiles to no-ops)")
endif()

# ---------------------------------------------------------------------------
# RESHADENATIVE. The P1.0c NGX lane on the unwrapped original device.
#
#   MGPU_RESHADE_NATIVE absent  ->  reshade_native.cpp logs variant=off and
#                                   every entry point returns its argument
#                                   unchanged. The P1.0c probe runs on the
#                                   ReShade proxy device exactly as before, so
#                                   the build is the pre-existing behaviour.
#   MGPU_RESHADE_NATIVE defined ->  after the GPU 1 device is created, the
#                                   proxy is asked for IID_UnwrappedObject; the
#                                   unwrap is validated (non-null, different
#                                   from the proxy, same LUID as T2 selected)
#                                   and, only then, the P1.0c NGX lane - device,
#                                   private allocator, private list, private
#                                   fence and the queue that drains it - is
#                                   built on that original device.
#
# SCOPE, and it is deliberate: the P1.0c lane ONLY. It creates no textures, no
# buffers and no transport, so a native lane there mixes nothing. The P4.1
# stream lane is NOT converted: one command list and queue (s.nl/s.nq) carry the
# transport copies, the NGX Evaluate calls AND the CreateFeature arm, and the
# device passed to stream_nr_create creates every NR texture the feature touches.
# Running that lane on the native device would either mix proxy resources with a
# native list - which this experiment forbids - or move the transport with it.
# That separation is a design decision that has not been made yet.
#
# There is ONE artifact and no compare job. The CI job builds it with
# MGPU_ARCH_COMPAT=ON, MGPU_CONTRACT_EXPANDED=ON, MGPU_NVAPI_EXPLICIT_INIT=OFF
# and MGPU_CUDA_DIAG=ON so nothing else moves.
# ---------------------------------------------------------------------------
option(MGPU_RESHADE_NATIVE
       "RESHADENATIVE: run the P1.0c NGX lane on the unwrapped original D3D12 device"
       OFF)
if(MGPU_RESHADE_NATIVE)
    target_compile_definitions(mgpu_bridge PRIVATE MGPU_RESHADE_NATIVE=1)
    message(STATUS "MGPU: RESHADENATIVE = ON (P1.0c NGX lane on the unwrapped device)")
else()
    message(STATUS "MGPU: RESHADENATIVE = OFF (P1.0c lane unchanged, proxy device)")
endif()

# ---------------------------------------------------------------------------
# APPID_NS. The NGX ApplicationId A/B for the Reserved18 sessions.
#
#   MGPU_APPID_NS absent  ->  CONTROL.  every Reserved18 init is passed 0ULL.
#                                        This is today's behaviour, unchanged.
#   MGPU_APPID_NS defined ->  APPID_NS. every Reserved18 init is passed
#                                        0x1000000ULL, the value NeuralScreen's
#                                        working Reserved18 path uses for BOTH
#                                        the core Init and the snippet Init_Ext.
#
# SIX CALL SITES, FOUR SESSIONS, ONE FUNCTION
#   P1.0c core     NVSDK_NGX_D3D12_Init        (and its Init_Ext fallback)
#   P1.0c snippet  NVSDK_NGX_D3D12_Init_Ext    (and its Init fallback)
#   P4.1  core     NVSDK_NGX_D3D12_Init
#   P4.1  snippet  NVSDK_NGX_D3D12_Init_Ext
#   All six call mgpu::appidns::reserved18_app_id(), so the sessions cannot
#   disagree with each other. The C2-SR session (nvngx_dlss.dll) is NOT a
#   Reserved18 session and keeps 0ULL.
#
# There is NO fallback id, NO retry with another id and NO project-id
# substitution, in either arm: a second attempt would be a second variable.
#
# The CI job builds it with MGPU_ARCH_COMPAT=ON, MGPU_CONTRACT_EXPANDED=ON,
# MGPU_NVAPI_EXPLICIT_INIT=OFF, MGPU_CUDA_DIAG=ON and MGPU_RESHADE_NATIVE=ON -
# the frozen baseline - so this one number is the only thing that moves.
# ---------------------------------------------------------------------------
option(MGPU_APPID_NS
       "APPID_NS: pass 0x1000000 as the NGX ApplicationId for the Reserved18 sessions"
       OFF)
if(MGPU_APPID_NS)
    target_compile_definitions(mgpu_bridge PRIVATE MGPU_APPID_NS=1)
    message(STATUS "MGPU: APPID_NS = ON (Reserved18 app id 0x1000000)")
else()
    message(STATUS "MGPU: APPID_NS = OFF (Reserved18 app id 0, control)")
endif()

# ---------------------------------------------------------------------------
# NR_RELAY. The game-side half of the out-of-process neural lane.
#
#   MGPU_NR_RELAY absent  ->  src/nr_relay.cpp is compiled to NOTHING. Every
#                             line of it is inside #ifdef MGPU_NR_RELAY, so the
#                             translation unit is empty, it links nothing new
#                             and it cannot touch any other unit's behaviour.
#   MGPU_NR_RELAY defined ->  the same file is a working module: it launches
#                             mgpu_nr_worker.exe, speaks control protocol v1 to
#                             it over a named pipe and publishes the four
#                             host-staged frame mappings.
#
# WHY IT IS ALWAYS IN THE SOURCE LIST RATHER THAN CONDITIONAL
#   "Compiled into the add-on but inert unless MGPU_NR_RELAY is defined" is
#   exactly what an empty-at-preprocessing translation unit gives, and it is
#   also what lets the GPU-free selftest (tools/nr_worker/nr_relay_selftest.cpp)
#   build the SAME file with the definition ON, in CI, with no GPU. Two
#   different source lists would be two different modules.
#
# WHAT IT DELIBERATELY DOES NOT GET
#   No include directory and no link library. The relay is self-contained on
#   purpose: it reproduces the v1 wire layout with the same static_asserts
#   nr_proto.h uses, instead of reaching into tools/nr_worker/ for the header.
#   The add-on must not gain a build-time path into the worker's tree - that
#   tree owns the RTX 4070 device, the NGX core and the Reserved18 session, and
#   the architecture exists precisely to keep them out of this process. The
#   nr-worker workflow's architecture gate checks the same thing from the other
#   side, and the nr-relay-ab workflow diffs the reproduced layout against
#   nr_proto.h so the separation cannot drift.
#
# NOTHING CALLS IT YET. The frame-pipeline call site is a separate, later
# piece of work written against src/nr_relay.hpp. The compiled-in relay is
# inert until that exists.
# ---------------------------------------------------------------------------
option(MGPU_NR_RELAY
       "NR_RELAY: compile the out-of-process relay module (inert until something calls it)"
       OFF)
if(MGPU_NR_RELAY)
    target_compile_definitions(mgpu_bridge PRIVATE MGPU_NR_RELAY=1)
    message(STATUS "MGPU: NR_RELAY = ON (src/nr_relay.cpp compiled; no caller yet, so no behaviour change)")
else()
    message(STATUS "MGPU: NR_RELAY = OFF (src/nr_relay.cpp compiles to an empty translation unit)")
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
