// ============================================================================
// MGPU Bridge - ARCHTEST implementation. See arch_test.hpp for the contract.
//
// Independently written for this experiment. No experiment logic is copied
// from anywhere; the only third-party code involved is a hooking library that
// is LINKED, not vendored into this file (see "THE HOOK" below).
//
// The NVAPI interface ids and the NV_GPU_ARCH_INFO layout below are public
// vendor interface facts, taken from NVIDIA/nvapi's own headers:
//     nvapi_interface.h : { "NvAPI_GPU_GetArchInfo", 0xd8265d24 }
//                         { "NvAPI_EnumPhysicalGPUs", 0xe5ac921f }
//                         { "NvAPI_GPU_GetPCIIdentifiers", 0x2ddfb66e }
//     nvapi.h           : NV_GPU_ARCH_INFO_V2 = { NvU32 version;
//                             NvU32 architecture; NvU32 implementation;
//                             NvU32 revision; }
//                         NV_GPU_ARCH_INFO_VER_2 = MAKE_NVAPI_VERSION(...,2)
// These are the same class of constant MGPU already carries for
// NvAPI_Initialize and NvAPI_D3D_SetSleepMode in gpu1_context.cpp, and they
// are cited rather than guessed: a wrong id returns a pointer to a DIFFERENT
// function.
//
// ============================================================================
// THE HOOK, AND WHY IT IS A LIBRARY CALL RATHER THAN HAND-WRITTEN
// ============================================================================
// nvapi64!nvapi_QueryInterface is detoured. The detour needs a callable
// ORIGINAL, and that original is what this file previously got wrong: it
// stored the function's own entry address and then overwrote that same
// address with the jump, so the "original" became the detour and the first
// call recursed until the stack was gone. The game died immediately after
// "hook installed", which is exactly where map_target_gpu() first calls it.
//
// A correct original is a TRAMPOLINE: the overwritten prologue instructions
// copied elsewhere, every RIP-relative operand and every relative branch/call
// inside them relocated to the new address, ending in a jump back to
// target + copied_length. Instruction-boundary safety alone is NOT enough - a
// copied instruction can be boundary-aligned and still be wrong if it
// addresses memory or jumps relative to its old position.
//
// That relocation is the whole problem, so it is delegated to MinHook
// (TsudaKageyu/minhook, BSD-2-Clause), the de-facto standard x64 hooking
// library: MH_CreateHook() performs the relocation and hands back the
// trampoline. This file contains no instruction decoder.
//
// MINHOOK IS OPTIONAL AT COMPILE TIME. If its header is not on the include
// path this module still compiles and links, hook_install() logs that it
// cannot intercept, and the process runs with the original NVAPI entry
// untouched. That is fail-closed: never patch, then call the patched entry.
// ============================================================================

#include "arch_test.hpp"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "diag.hpp"
#include "adapter.hpp"
#include "cuda_diag.hpp"

#if defined(__has_include)
#  if __has_include(<MinHook.h>)
#    include <MinHook.h>
#    define ARCHTEST_HAVE_MINHOOK 1
#  endif
#endif
#ifndef ARCHTEST_HAVE_MINHOOK
#  define ARCHTEST_HAVE_MINHOOK 0
#endif

namespace mgpu::archtest
{
namespace
{
    // ---- the only interface ids this file names -------------------------
    const unsigned NVAPI_ID_ENUM_PHYSICAL_GPUS = 0xE5AC921Fu;
    const unsigned NVAPI_ID_GET_PCI_IDENTIFIERS = 0x2DDFB66Eu;
    const unsigned NVAPI_ID_GET_ARCH_INFO = 0xD8265D24u;

    // NV_GPU_ARCH_INFO_VER_2 = 2 * 1000 + 0  (MAKE_NVAPI_VERSION)
    const unsigned NV_GPU_ARCH_INFO_VER_2 = 2000u;

    // NVAPI_OK
    const int NVAPI_OK = 0;

    // The experimental compatibility values. Explicitly NOT hardware identity:
    // they exist to answer one question and are compiled into a test build.
    const unsigned TEST_ARCHITECTURE = 0x1B0u;
    const unsigned TEST_IMPLEMENTATION = 0x3u;
    const unsigned TEST_REVISION = 0xA1u;

    // ---- public NVAPI data shapes (vendor interface facts) --------------
    struct NV_GPU_ARCH_INFO
    {
        unsigned version;
        unsigned architecture;
        unsigned implementation;
        unsigned revision;
    };
    static_assert(sizeof(NV_GPU_ARCH_INFO) == 16, "NV_GPU_ARCH_INFO is four NvU32");

    struct NV_PHYSICAL_GPU_HANDLE_ARRAY
    {
        void *gpu[64];
    };

    // THE ORIGINAL. After hook_install() this points at the relocation
    // trampoline, NOT at the patched entry - the distinction an earlier
    // revision got wrong.
    void *g_pfn_query_original = nullptr;

    void *g_pfn_arch_original = nullptr;   // the REAL NvAPI_GPU_GetArchInfo (trampoline)
    void *g_pfn_arch_entry = nullptr;      // its patched entry, for MH removal
    bool g_direct_arch_hooked = false;     // the proactive direct hook is live

    void *g_hook_target = nullptr;         // nvapi_QueryInterface entry
    bool g_hook_live = false;

    void *g_target_gpu = nullptr;          // the RTX 4070's physical handle
    unsigned g_target_pci = 0;             // NVAPI pDeviceId packing: (device << 16) | vendor
    bool g_target_known = false;

    std::atomic<bool> g_active{false};     // the temporal scope
    // PER-SCOPE, not cumulative: reset by every scope_begin() so scope_end()
    // reports whether a rewrite happened in THAT scope. There are two scopes -
    // the startup private NR load and the real stream arm.
    std::atomic<bool> g_rewrote{false};

    using pfn_query = void *(*)(unsigned int);
    using pfn_get_arch_info = int (*)(void *, NV_GPU_ARCH_INFO *);
    using pfn_pci_identifiers = int (*)(void *, unsigned *, unsigned *, unsigned *, unsigned *);
    using pfn_enum_physical = int (*)(NV_PHYSICAL_GPU_HANDLE_ARRAY *, unsigned *);

#if ARCHTEST_HAVE_MINHOOK
    const char *mh_status_string(MH_STATUS s)
    {
        switch (s)
        {
            case MH_OK: return "MH_OK";
            case MH_ERROR_ALREADY_INITIALIZED: return "MH_ERROR_ALREADY_INITIALIZED";
            case MH_ERROR_NOT_INITIALIZED: return "MH_ERROR_NOT_INITIALIZED";
            case MH_ERROR_ALREADY_CREATED: return "MH_ERROR_ALREADY_CREATED";
            case MH_ERROR_NOT_CREATED: return "MH_ERROR_NOT_CREATED";
            case MH_ERROR_ENABLED: return "MH_ERROR_ENABLED";
            case MH_ERROR_DISABLED: return "MH_ERROR_DISABLED";
            case MH_ERROR_NOT_EXECUTABLE: return "MH_ERROR_NOT_EXECUTABLE";
            case MH_ERROR_UNSUPPORTED_FUNCTION: return "MH_ERROR_UNSUPPORTED_FUNCTION";
            case MH_ERROR_MEMORY_ALLOC: return "MH_ERROR_MEMORY_ALLOC";
            case MH_ERROR_MEMORY_PROTECT: return "MH_ERROR_MEMORY_PROTECT";
            case MH_ERROR_MODULE_NOT_FOUND: return "MH_ERROR_MODULE_NOT_FOUND";
            case MH_ERROR_FUNCTION_NOT_FOUND: return "MH_ERROR_FUNCTION_NOT_FOUND";
            default: return "MH_ERROR_?";
        }
    }
#else
    const char *mh_status_string(int) { return "MinHook not compiled in"; }
#endif

    // ---- the shim the runtime actually calls ----------------------------
    // Signature is the real one. The real function is called FIRST, through
    // its trampoline; the rewrite is applied afterwards to its output and only
    // when every gate passes.
    int __cdecl get_arch_info_shim(void *hPhysicalGpu, NV_GPU_ARCH_INFO *pInfo)
    {
        const pfn_get_arch_info real_fn =
            reinterpret_cast<pfn_get_arch_info>(g_pfn_arch_original);
        if (real_fn == nullptr)
            return -1;

        const int status = real_fn(hPhysicalGpu, pInfo);   // REAL CALL FIRST

        const unsigned arch0 = (pInfo != nullptr) ? pInfo->architecture : 0u;
        const unsigned impl0 = (pInfo != nullptr) ? pInfo->implementation : 0u;
        const unsigned rev0 = (pInfo != nullptr) ? pInfo->revision : 0u;

        char l[288];
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] GetArchInfo handle=%p status=%d", hPhysicalGpu, status);
        mgpu::diag::info(l);
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] original arch=0x%X impl=0x%X rev=0x%X", arch0, impl0, rev0);
        mgpu::diag::info(l);

        // ---- the gates, in order. Any one of them can refuse. ----
        bool rewrite = false;
        const char *why = "refused: ";

        if (status != NVAPI_OK)                       why = "refused: real call did not return NVAPI_OK";
        else if (pInfo == nullptr)                    why = "refused: null arch info";
        else if (!g_active.load(std::memory_order_relaxed))
                                                      why = "refused: outside the feature-creation scope (pass-through)";
        else if (!g_target_known)                     why = "refused: neural GPU identity unknown - NOT spoofing";
        else if (hPhysicalGpu != g_target_gpu)        why = "refused: handle is not the selected neural GPU";
        else                                          { rewrite = true; why = "allowed"; }

        if (rewrite)
        {
            pInfo->architecture = TEST_ARCHITECTURE;
            pInfo->implementation = TEST_IMPLEMENTATION;
            pInfo->revision = TEST_REVISION;
            g_rewrote.store(true, std::memory_order_relaxed);

            std::snprintf(l, sizeof l,
                          "[ARCHTEST] rewritten arch=0x%X impl=0x%X rev=0x%X",
                          TEST_ARCHITECTURE, TEST_IMPLEMENTATION, TEST_REVISION);
            mgpu::diag::info(l);
            mgpu::diag::info("[ARCHTEST] rewrite=yes");
            std::snprintf(l, sizeof l,
                          "[ARCHTEST] target identity expected pDeviceId=0x%08X handle=%p "
                          "(selected neural adapter)", g_target_pci, g_target_gpu);
            mgpu::diag::info(l);
        }
        else
        {
            std::snprintf(l, sizeof l, "[ARCHTEST] rewrite=no (%s)", why);
            mgpu::diag::info(l);
        }
        return status;
    }

    // Hook NvAPI_GPU_GetArchInfo itself, so the runtime's key dispatch of the
    // architecture query lands in get_arch_info_shim and the real function
    // stays reachable through its own trampoline.
    bool create_arch_hook(void *target)
    {
#if ARCHTEST_HAVE_MINHOOK
        void *original = nullptr;
        MH_STATUS st = MH_CreateHook(target, reinterpret_cast<LPVOID>(&get_arch_info_shim), &original);
        if (st != MH_OK || original == nullptr)
        {
            char l[192];
            std::snprintf(l, sizeof l, "[ARCHTEST] MH_CreateHook(GetArchInfo) failed: %s",
                          mh_status_string(st));
            mgpu::diag::warn(l);
            return false;
        }
        // PUBLISH THE TRAMPOLINE BEFORE THE HOOK BECOMES CALLABLE.
        //
        // get_arch_info_shim dereferences g_pfn_arch_original on every entry. If
        // the hook were enabled first, this thread - or any other the runtime
        // calls from - could enter the shim in the window between MH_EnableHook
        // returning and the pointer being stored, and the shim would run with a
        // null original. Publishing first closes that window completely: the
        // pointer is non-null from the moment the detour can be reached.
        g_pfn_arch_original = original;    // THE TRAMPOLINE

        st = MH_EnableHook(target);
        if (st != MH_OK)
        {
            char l[192];
            std::snprintf(l, sizeof l, "[ARCHTEST] MH_EnableHook(GetArchInfo) failed: %s",
                          mh_status_string(st));
            mgpu::diag::warn(l);
            // Take the publication back with the hook, so a failed enable cannot
            // leave a trampoline pointing at something that is no longer
            // installed.
            MH_RemoveHook(target);
            g_pfn_arch_original = nullptr;
            return false;
        }
        return true;
#else
        (void)target;
        return false;
#endif
    }

    // ---- proactive GetArchInfo resolution --------------------------------
    // PROACTIVE, NOT REACTIVE. The first private nvngx_dlssnr.dll load happens
    // in the P1.0c startup probe, which runs long before stream_nr_create().
    // Waiting for the runtime to ask nvapi_QueryInterface for
    // NVAPI_ID_GET_ARCH_INFO would therefore install the direct hook after the
    // machinery that needs it has already run - which is exactly what the first
    // game test showed: the hook was installed, the scope was armed, and the
    // runtime never asked, so no GetArchInfo call was ever intercepted.
    //
    // So the entry is resolved NOW, by calling the real QueryInterface
    // trampoline ourselves for that one id, and the direct hook goes on
    // immediately.
    bool resolve_and_hook_arch_proactively()
    {
        if (g_direct_arch_hooked)
            return true;
        if (g_pfn_query_original == nullptr)
        {
            mgpu::diag::warn("[ARCHTEST] proactive GetArchInfo resolve skipped: no QueryInterface "
                             "trampoline");
            return false;
        }

        const pfn_query original_query = reinterpret_cast<pfn_query>(g_pfn_query_original);
        void *entry = original_query(NVAPI_ID_GET_ARCH_INFO);   // the REAL entry

        char l[224];
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] proactive GetArchInfo resolve entry=%p", entry);
        mgpu::diag::info(l);

        if (entry == nullptr)
        {
            mgpu::diag::warn("[ARCHTEST] proactive GetArchInfo resolve returned null - the driver "
                             "does not expose that interface id. No architecture rewrite is "
                             "possible and the run cannot test the theory.");
            return false;
        }

        g_pfn_arch_entry = entry;
        if (!create_arch_hook(entry))
        {
            mgpu::diag::warn("[ARCHTEST] proactive GetArchInfo detour could NOT be installed - "
                             "no architecture rewrite is possible");
            return false;
        }

        g_direct_arch_hooked = true;
        mgpu::diag::info("[ARCHTEST] proactive GetArchInfo detour installed");
        return true;
    }

    // ---- the detour on nvapi_QueryInterface -----------------------------
    // It hands the runtime OUR shim for exactly one id. Every other id is
    // returned untouched. The REAL function is called through the trampoline
    // first, so its answer is never invented and never re-enters this detour.
    //
    // The experiment does NOT depend on this path any more: the direct hook is
    // installed proactively. This remains as a secondary/future-resolution path
    // in case a runtime resolves the id later than the startup probe, and it
    // simply reconfirms the same entry.
    void *__cdecl query_detour(unsigned int id)
    {
        if (g_pfn_query_original == nullptr)
        {
            // No trampoline: refuse to guess. Answering null makes the runtime
            // treat the interface as absent - a clean no-op rather than the
            // re-entrant crash this replaces.
            mgpu::diag::error("[ARCHTEST] detour entered with no original trampoline - "
                              "returning null rather than recursing");
            return nullptr;
        }

        const pfn_query real_fn = reinterpret_cast<pfn_query>(g_pfn_query_original);
        void *answer = real_fn(id);            // ORIGINAL, via the trampoline

        if (id == NVAPI_ID_GET_ARCH_INFO)
        {
            // Secondary path: if the proactive install did not happen for some
            // reason, do it here rather than pass the raw entry through.
            if (!g_direct_arch_hooked && answer != nullptr)
                resolve_and_hook_arch_proactively();

            return g_direct_arch_hooked
                       ? reinterpret_cast<void *>(&get_arch_info_shim)
                       : answer;
        }

        // ---- CUDADIAG composition point (Phase 2) -----------------------
        // Observational only. select() answers `answer` itself for every id it
        // does not instrument, and for the two CUDA-interop ids it answers a
        // pass-through wrapper whose SOLE behaviour is to call the genuine
        // function once with the original arguments and return its status
        // verbatim. It can never alter an answer: with MGPU_CUDA_DIAG undefined
        // it is an inline `return real`.
        //
        // This is deliberately on the EXISTING detour, not a second
        // QueryInterface hook and not a hook on either CUDA entry point. The
        // resolver investigation proved both ids reach the driver through the
        // same cached nvapi_QueryInterface pointer this detour already owns.
        return mgpu::cudadiag::select(id, answer);
    }

    // ---- identity: which physical GPU is MGPU's neural adapter? ---------
    // Preferred mapping is by PCI identity, not by enumeration order. The
    // selected DXGI adapter's vendor/device pair comes from the T2 selection;
    // NVAPI reports the same pair per physical GPU, so the two are matched on
    // the pair itself. Enumeration order is never used as evidence.
    void map_target_gpu()
    {
        if (g_target_pci == 0)
        {
            mgpu::diag::warn("[ARCHTEST] selected neural GPU identity unavailable - "
                             "NOT spoofing any adapter");
            return;
        }
        if (g_pfn_query_original == nullptr)
        {
            mgpu::diag::warn("[ARCHTEST] no QueryInterface trampoline - cannot enumerate GPUs");
            return;
        }

        const pfn_query q = reinterpret_cast<pfn_query>(g_pfn_query_original);
        const pfn_enum_physical enum_phys =
            reinterpret_cast<pfn_enum_physical>(q(NVAPI_ID_ENUM_PHYSICAL_GPUS));
        const pfn_pci_identifiers pci_ids =
            reinterpret_cast<pfn_pci_identifiers>(q(NVAPI_ID_GET_PCI_IDENTIFIERS));
        if (enum_phys == nullptr || pci_ids == nullptr)
        {
            mgpu::diag::warn("[ARCHTEST] NVAPI enumeration/PCI entry points unavailable - "
                             "NOT spoofing any adapter");
            return;
        }

        NV_PHYSICAL_GPU_HANDLE_ARRAY gpus = {};
        unsigned count = 0;
        const int st = enum_phys(&gpus, &count);
        if (st != NVAPI_OK)
        {
            char l[160];
            std::snprintf(l, sizeof l,
                          "[ARCHTEST] NvAPI_EnumPhysicalGPUs status=%d - NOT spoofing", st);
            mgpu::diag::warn(l);
            return;
        }
        if (count > 64) count = 64;

        for (unsigned i = 0; i < count; ++i)
        {
            if (gpus.gpu[i] == nullptr) continue;
            unsigned dev = 0, sub = 0, rev = 0, ext = 0;
            if (pci_ids(gpus.gpu[i], &dev, &sub, &rev, &ext) != NVAPI_OK) continue;

            // `dev` is the RAW NVAPI pDeviceId: vendor in the low 16 bits,
            // device in the high 16. Both it and the packed target are logged so
            // the match is provable from the log alone.
            char l[320];
            std::snprintf(l, sizeof l,
                          "[ARCHTEST] nvapi physical[%u] handle=%p raw pDeviceId=0x%08X "
                          "(vendor=0x%04X device=0x%04X) subsystem=0x%08X expected=0x%08X",
                          i, gpus.gpu[i], dev,
                          dev & 0xFFFFu, (dev >> 16) & 0xFFFFu, sub, g_target_pci);
            mgpu::diag::info(l);

            if (dev == g_target_pci)
            {
                g_target_gpu = gpus.gpu[i];
                g_target_known = true;
                std::snprintf(l, sizeof l,
                              "[ARCHTEST] selected neural GPU identity MATCHED: raw pDeviceId=0x%08X "
                              "== expected 0x%08X handle=%p (matched on the raw NVAPI word, "
                              "not enumeration order)", dev, g_target_pci, gpus.gpu[i]);
                mgpu::diag::info(l);
                return;
            }
        }

        // No match: refuse. Spoofing some other Ada adapter is the exact
        // failure mode this scope rule exists to prevent.
        char l[320];
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] selected neural GPU identity NOT FOUND among %u physical GPUs "
                      "(expected NVAPI pDeviceId=0x%08X) - NOT spoofing any adapter",
                      count, g_target_pci);
        mgpu::diag::warn(l);
    }
}

// =====================================================================
// public interface
// =====================================================================

void log_mode()
{
#ifdef MGPU_ARCH_COMPAT
    mgpu::diag::info("[ARCHTEST] mode=COMPAT");
#else
    mgpu::diag::info("[ARCHTEST] mode=CONTROL");
#endif
}

void set_neural_gpu(unsigned vendor_id, unsigned device_id)
{
    // NVAPI's pDeviceId is packed with the VENDOR in the low 16 bits and the
    // DEVICE in the high 16: NvAPI_GPU_GetPCIIdentifiers returns 0x278610DE for
    // the RTX 4070 (vendor 0x10DE, device 0x2786). DXGI hands us the two halves
    // separately, so the target is packed the SAME WAY NVAPI packs it, and the
    // comparison in map_target_gpu() is then a straight equality on the raw
    // NVAPI word. Packing it the other way round (vendor << 16 | device) would
    // never match and the identity gate would silently refuse every adapter.
    g_target_pci = ((device_id & 0xFFFFu) << 16) | (vendor_id & 0xFFFFu);

    char l[224];
    std::snprintf(l, sizeof l,
                  "[ARCHTEST] selected neural GPU identity vendor=0x%04X device=0x%04X "
                  "expected NVAPI pDeviceId=0x%08X",
                  vendor_id & 0xFFFFu, device_id & 0xFFFFu, g_target_pci);
    mgpu::diag::info(l);
}

bool hook_install()
{
#ifdef MGPU_ARCH_COMPAT
    if (g_hook_live) return true;

#if !ARCHTEST_HAVE_MINHOOK
    // Fail closed. MinHook's header was not on the include path, so no
    // relocation trampoline can be produced. Patch nothing; run normally.
    mgpu::diag::warn("[ARCHTEST] hook not installed: MinHook not available in this build - "
                     "NO interception, the game runs with the original NVAPI entry untouched");
    return false;
#else
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi == nullptr) nvapi = LoadLibraryW(L"nvapi64.dll");
    if (nvapi == nullptr)
    {
        mgpu::diag::warn("[ARCHTEST] hook not installed: nvapi64.dll not present");
        return false;
    }

    void *target = reinterpret_cast<void *>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (target == nullptr)
    {
        mgpu::diag::warn("[ARCHTEST] hook not installed: nvapi_QueryInterface not exported");
        return false;
    }
    g_hook_target = target;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        char l[192];
        std::snprintf(l, sizeof l, "[ARCHTEST] hook not installed: MH_Initialize -> %s",
                      mh_status_string(st));
        mgpu::diag::warn(l);
        return false;
    }

    // MH_CreateHook relocates the prologue and RETURNS THE TRAMPOLINE. That
    // trampoline - never `target` - is the callable original.
    void *original = nullptr;
    st = MH_CreateHook(target, reinterpret_cast<LPVOID>(&query_detour), &original);
    if (st != MH_OK || original == nullptr)
    {
        char l[224];
        std::snprintf(l, sizeof l, "[ARCHTEST] hook not installed: MH_CreateHook -> %s",
                      mh_status_string(st));
        mgpu::diag::warn(l);
        return false;
    }
    g_pfn_query_original = original;

    st = MH_EnableHook(target);
    if (st != MH_OK)
    {
        char l[224];
        std::snprintf(l, sizeof l, "[ARCHTEST] hook not installed: MH_EnableHook -> %s",
                      mh_status_string(st));
        mgpu::diag::warn(l);
        MH_RemoveHook(target);
        g_pfn_query_original = nullptr;
        return false;
    }

    g_hook_live = true;
    mgpu::diag::info("[ARCHTEST] hook installed");
    {
        char l[256];
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] original QueryInterface trampoline=%p (entry=%p, relocated by MinHook)",
                      g_pfn_query_original, target);
        mgpu::diag::info(l);
    }

    // Resolve and hook NvAPI_GPU_GetArchInfo NOW, before the caller loads the
    // private runtime. This is the timing fix: the P1.0c startup probe is the
    // runtime's FIRST load, and it must not be reached before the direct hook
    // exists.
    resolve_and_hook_arch_proactively();

    map_target_gpu();
    if (!g_target_known)
        mgpu::diag::warn("[ARCHTEST] neural GPU identity not established - the hook will "
                         "pass every call through and rewrite nothing");
    if (!g_direct_arch_hooked)
        mgpu::diag::warn("[ARCHTEST] direct GetArchInfo hook is NOT installed - no architecture "
                         "rewrite can occur this run");
    return true;
#endif
#else
    // CONTROL: no interception of any kind, ever. The identity gate is still
    // logged so both logs carry the same facts about which GPU MGPU bound -
    // only the rewrite behaviour differs between the two builds.
    mgpu::diag::info("[ARCHTEST] hook not installed: CONTROL build performs no interception");
    if (g_target_pci == 0)
        mgpu::diag::warn("[ARCHTEST] selected neural GPU identity unavailable - "
                         "NOT spoofing any adapter");
    return false;
#endif
}

void hook_remove()
{
#ifdef MGPU_ARCH_COMPAT
#if ARCHTEST_HAVE_MINHOOK
    // Disable rather than remove: the runtime may still hold pointers to the
    // shim, and disabling restores the original bytes at each entry without
    // freeing anything a live pointer could still reach. The rewrite is off
    // already (scope_end), so every remaining call is a pass-through.
    if (g_hook_live && g_pfn_arch_entry != nullptr)
        MH_DisableHook(g_pfn_arch_entry);
    if (g_hook_live && g_hook_target != nullptr)
    {
        MH_DisableHook(g_hook_target);
        mgpu::diag::info("[ARCHTEST] hook disabled (original NVAPI entry restored)");
    }
    g_direct_arch_hooked = false;
    g_hook_live = false;
#endif
#endif
}

void scope_begin(const char *why)
{
    // PER-SCOPE, NOT CUMULATIVE. There are two distinct scopes now - the
    // startup private NR load and the real stream arm - and scope_end() reports
    // whether a rewrite happened "this scope". Without this reset a rewrite
    // during startup would make the real-stream scope_end() claim a rewrite it
    // never performed, which is exactly the kind of false positive that would
    // misreport the experiment. Reset BEFORE arming.
    g_rewrote.store(false, std::memory_order_relaxed);

    if (g_target_known && g_direct_arch_hooked)
        g_active.store(true, std::memory_order_relaxed);

    char l[256];
    if (g_target_known && g_direct_arch_hooked)
        std::snprintf(l, sizeof l, "[ARCHTEST] rewrite scope ARMED (%s)",
                      (why != nullptr) ? why : "unspecified");
    else if (!g_target_known)
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] rewrite scope requested (%s) but identity is unknown - staying disarmed",
                      (why != nullptr) ? why : "unspecified");
    else
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] rewrite scope requested (%s) but the direct GetArchInfo hook is "
                      "not installed - staying disarmed", (why != nullptr) ? why : "unspecified");
    mgpu::diag::info(l);
}

void scope_end()
{
    g_active.store(false, std::memory_order_relaxed);
    mgpu::diag::info(g_rewrote.load()
        ? "[ARCHTEST] rewrite scope DISARMED (a rewrite was applied this scope)"
        : "[ARCHTEST] rewrite scope DISARMED (no rewrite was applied this scope)");
}

void log_scope_marker(const char *why)
{
    char l[224];
    std::snprintf(l, sizeof l, "[ARCHTEST] ---- scope: %s ----",
                  (why != nullptr) ? why : "unspecified");
    mgpu::diag::info(l);
}

bool direct_arch_hook_installed()
{
    return g_direct_arch_hooked;
}

unsigned matched_gpu_pci()
{
    return g_target_pci;
}

void log_entering_create_feature()
{
    mgpu::diag::info("[ARCHTEST] entering CreateFeature(Reserved18)");
}

void log_create_feature_result(long result, void *handle)
{
    char l[224];
    std::snprintf(l, sizeof l,
                  "[ARCHTEST] CreateFeature result=0x%08lX handle=%p",
                  static_cast<unsigned long>(static_cast<unsigned>(result)), handle);
    mgpu::diag::info(l);
}
}
