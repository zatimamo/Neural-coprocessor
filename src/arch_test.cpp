// ============================================================================
// MGPU Bridge - ARCHTEST implementation. See arch_test.hpp for the contract.
//
// Independently written for this experiment. No third-party source is copied.
// The NVAPI interface ids and the NV_GPU_ARCH_INFO layout below are public
// vendor interface facts, taken from NVIDIA/nvapi's own headers:
//     nvapi_interface.h : { "NvAPI_GPU_GetArchInfo", 0xd8265d24 }
//                         { "NvAPI_EnumPhysicalGPUs", 0xe5ac921f }
//                         { "NvAPI_GPU_GetPCIIdentifiers", 0x2ddfb66e }
//     nvapi.h           : NV_GPU_ARCH_INFO_V2 = { NvU32 version;
//                             NvU32 architecture; NvU32 implementation;
//                             NvU32 revision; }
//                         NV_GPU_ARCH_INFO_VER_2 = MAKE_NVAPI_VERSION(...,2)
// These are the same class of constant MGPU already carries for NvAPI_Initialize
// and NvAPI_D3D_SetSleepMode in gpu1_context.cpp, and they are cited rather
// than guessed: a wrong id returns a pointer to a DIFFERENT function.
// ============================================================================

#include "arch_test.hpp"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "diag.hpp"
#include "adapter.hpp"

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

    void *g_pfn_query_real = nullptr;      // the REAL nvapi_QueryInterface
    void *g_pfn_pci_ident = nullptr;       // the REAL NvAPI_GPU_GetPCIIdentifiers
    void *g_pfn_arch_real = nullptr;       // the REAL NvAPI_GPU_GetArchInfo

    void *g_hook_page = nullptr;           // our detour stub
    unsigned char g_saved_entry[32] = {};
    unsigned g_saved_len = 0;
    bool g_hook_live = false;

    void *g_target_gpu = nullptr;          // the RTX 4070's physical handle
    unsigned g_target_pci = 0;             // (vendor << 16) | device
    bool g_target_known = false;

    std::atomic<bool> g_active{false};     // the temporal scope
    std::atomic<bool> g_rewrote{false};    // did we actually rewrite once?

    using pfn_query = void *(*)(unsigned int);
    using pfn_get_arch_info = int (*)(void *, NV_GPU_ARCH_INFO *);
    using pfn_pci_identifiers = int (*)(void *, unsigned *, unsigned *, unsigned *, unsigned *);
    using pfn_enum_physical = int (*)(NV_PHYSICAL_GPU_HANDLE_ARRAY *, unsigned *);

    // ---- minimal x64 instruction-length decoder -------------------------
    // Writing a detour over the first bytes of a function is only sound if the
    // copy ends on an instruction boundary. Rather than assume a prologue
    // shape, decode it. Returns 0 for anything not decoded, and the caller
    // REFUSES rather than guessing.
    int insn_len(const unsigned char *p, int avail)
    {
        int i = 0;
        bool rex_w = false;

        // legacy prefixes
        for (;;)
        {
            if (!(i < avail)) return 0;
            const unsigned char b = p[i];
            if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65)
            { ++i; continue; }
            break;
        }
        // REX
        if (i < avail && p[i] >= 0x40 && p[i] <= 0x4F)
        {
            rex_w = (p[i] & 0x08) != 0;
            ++i;
        }
        if (i >= avail) return 0;
        const unsigned char op = p[i++];

        if (op >= 0x50 && op <= 0x5F) return i;          // push/pop r64
        if (op == 0x90 || op == 0xC3 || op == 0xCC) return i;
        if (op >= 0xB8 && op <= 0xBF) return i + (rex_w ? 8 : 4);
        if (op >= 0xB0 && op <= 0xB7) return i + 1;
        if (op == 0x6A) return i + 1;
        if (op == 0x68) return i + 4;
        if (op == 0xE8 || op == 0xE9) return (i + 4 <= avail) ? i + 4 : 0;
        if (op == 0xEB) return (i + 1 <= avail) ? i + 1 : 0;

        bool modrm = false, imm8 = false, imm32 = false;
        switch (op)
        {
            case 0x88: case 0x89: case 0x8A: case 0x8B:
            case 0x00: case 0x01: case 0x02: case 0x03:
            case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x30: case 0x31: case 0x32: case 0x33:
            case 0x84: case 0x85: case 0x8D:
            case 0xFF: case 0xFE:
                modrm = true; break;
            case 0x83: case 0xC1: case 0x6B: case 0x80:
                modrm = true; imm8 = true; break;
            case 0x81: case 0xC7: case 0x69:
                modrm = true; imm32 = true; break;
            default:
                return 0;                                 // not decoded -> refuse
        }
        if (!modrm || i >= avail) return 0;

        const unsigned char m = p[i++];
        const int mod = m >> 6, rm = m & 7;
        if (mod != 3)
        {
            if (rm == 4)
            {
                if (i >= avail) return 0;
                const unsigned char sib = p[i++];
                if ((sib & 7) == 5 && mod == 0) i += 4;
            }
            else if (rm == 5 && mod == 0) i += 4;
            if (mod == 1) i += 1;
            else if (mod == 2) i += 4;
        }
        if (imm8) i += 1;
        if (imm32) i += 4;
        return (i <= avail) ? i : 0;
    }

    // Bytes consumed by whole instructions until >= need. 0 if undecodable.
    int bytes_to_cover(const unsigned char *p, int avail, int need)
    {
        int total = 0;
        while (total < need)
        {
            const int l = insn_len(p + total, avail - total);
            if (l <= 0) return 0;
            total += l;
            if (total > 32) return 0;
        }
        return total;
    }

    // ---- the shim the runtime actually calls ----------------------------
    // Signature is the real one. The real function is called FIRST; the
    // rewrite is applied afterwards to its output and only under every gate.
    int __cdecl get_arch_info_shim(void *hPhysicalGpu, NV_GPU_ARCH_INFO *pInfo)
    {
        const pfn_get_arch_info real_fn =
            reinterpret_cast<pfn_get_arch_info>(g_pfn_arch_real);
        if (real_fn == nullptr)
            return -1;

        const int status = real_fn(hPhysicalGpu, pInfo);   // REAL CALL FIRST

        const unsigned arch0 = (pInfo != nullptr) ? pInfo->architecture : 0u;
        const unsigned impl0 = (pInfo != nullptr) ? pInfo->implementation : 0u;
        const unsigned rev0 = (pInfo != nullptr) ? pInfo->revision : 0u;

        char l[288];
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] GetArchInfo handle=%p status=%d",
                      hPhysicalGpu, status);
        mgpu::diag::info(l);
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] original arch=0x%X impl=0x%X rev=0x%X",
                      arch0, impl0, rev0);
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
                          "[ARCHTEST] target identity pci=0x%08X handle=%p (selected neural adapter)",
                          g_target_pci, g_target_gpu);
            mgpu::diag::info(l);
        }
        else
        {
            std::snprintf(l, sizeof l, "[ARCHTEST] rewrite=no (%s)", why);
            mgpu::diag::info(l);
        }
        return status;
    }

    // ---- the detour on nvapi_QueryInterface -----------------------------
    // It hands the runtime OUR shim for exactly one id per build mode. Every
    // other id is returned untouched. The real function is called first, so
    // its answer is never invented.
    void *__cdecl query_detour(unsigned int id)
    {
        const pfn_query real_fn = reinterpret_cast<pfn_query>(g_pfn_query_real);
        void *answer = (real_fn != nullptr) ? real_fn(id) : nullptr;

        if (id == NVAPI_ID_GET_ARCH_INFO)
        {
            // Remember the REAL address before substituting our shim, so the
            // shim can call the real function through it.
            if (answer != nullptr)
                g_pfn_arch_real = answer;
            return reinterpret_cast<void *>(&get_arch_info_shim);
        }
        return answer;
    }

    // ---- detour installation -------------------------------------------
    bool install_detour(void *target, void *replacement)
    {
        unsigned char probe[32] = {};
        std::memcpy(probe, target, sizeof probe);

        int need = bytes_to_cover(probe, sizeof probe, 12);
        if (need < 12)
        {
            // Either undecodable (0) or too short for the 12-byte jmp. Both are
            // refusals: a split instruction would fault inside nvapi64.
            mgpu::diag::warn("[ARCHTEST] hook refused: nvapi_QueryInterface prologue does not "
                             "reach an instruction boundary at 12 bytes");
            return false;
        }

        DWORD old = 0;
        if (!VirtualProtect(target, static_cast<SIZE_T>(need), PAGE_EXECUTE_READWRITE, &old))
        {
            mgpu::diag::error("[ARCHTEST] hook refused: VirtualProtect failed");
            return false;
        }
        std::memcpy(g_saved_entry, target, static_cast<size_t>(need));
        g_saved_len = static_cast<unsigned>(need);

        unsigned char patch[16] = {};
        patch[0] = 0x48; patch[1] = 0xB8;                       // mov rax, imm64
        std::memcpy(patch + 2, &replacement, sizeof replacement);
        patch[10] = 0xFF; patch[11] = 0xE0;                     // jmp rax
        for (int i = 12; i < need; ++i) patch[i] = 0x90;        // nop pad

        std::memcpy(target, patch, static_cast<size_t>(need));
        VirtualProtect(target, static_cast<SIZE_T>(need), old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, static_cast<SIZE_T>(need));
        return true;
    }

    void remove_detour(void *target)
    {
        if (g_saved_len == 0) return;
        DWORD old = 0;
        if (!VirtualProtect(target, g_saved_len, PAGE_EXECUTE_READWRITE, &old)) return;
        std::memcpy(target, g_saved_entry, g_saved_len);
        VirtualProtect(target, g_saved_len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, g_saved_len);
        g_saved_len = 0;
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
        if (g_pfn_query_real == nullptr)
            return;

        const pfn_query q = reinterpret_cast<pfn_query>(g_pfn_query_real);
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

            // NVAPI returns the PCI device id in the low 16 bits with the
            // vendor in the high 16: 0x10DE2786 for the RTX 4070.
            char l[224];
            std::snprintf(l, sizeof l,
                          "[ARCHTEST] nvapi physical[%u] handle=%p pci=0x%08X subsystem=0x%08X",
                          i, gpus.gpu[i], dev, sub);
            mgpu::diag::info(l);

            if (dev == g_target_pci)
            {
                g_target_gpu = gpus.gpu[i];
                g_target_known = true;
                std::snprintf(l, sizeof l,
                              "[ARCHTEST] selected neural GPU identity MATCHED: pci=0x%08X handle=%p "
                              "(matched by PCI identity, not enumeration order)", dev, gpus.gpu[i]);
                mgpu::diag::info(l);
                return;
            }
        }

        // No match: refuse. Spoofing some other Ada adapter is the exact
        // failure mode this scope rule exists to prevent.
        char l[256];
        std::snprintf(l, sizeof l,
                      "[ARCHTEST] selected neural GPU identity NOT FOUND among %u physical GPUs "
                      "(looking for pci=0x%08X) - NOT spoofing any adapter", count, g_target_pci);
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
    g_target_pci = ((vendor_id & 0xFFFFu) << 16) | (device_id & 0xFFFFu);
    char l[160];
    std::snprintf(l, sizeof l,
                  "[ARCHTEST] selected neural GPU identity vendor=0x%04X device=0x%04X pci=0x%08X",
                  vendor_id & 0xFFFFu, device_id & 0xFFFFu, g_target_pci);
    mgpu::diag::info(l);
}

bool hook_install()
{
#ifdef MGPU_ARCH_COMPAT
    if (g_hook_live) return true;

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
    g_pfn_query_real = target;   // the real address, used directly - no trampoline

    if (!install_detour(target, reinterpret_cast<void *>(&query_detour)))
        return false;

    g_hook_live = true;
    mgpu::diag::info("[ARCHTEST] hook installed");

    map_target_gpu();
    if (!g_target_known)
        mgpu::diag::warn("[ARCHTEST] neural GPU identity not established - the hook will "
                         "pass every call through and rewrite nothing");
    return true;
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
    if (!g_hook_live) return;
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi != nullptr)
    {
        void *target = reinterpret_cast<void *>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
        if (target != nullptr) remove_detour(target);
    }
    g_hook_live = false;
    mgpu::diag::info("[ARCHTEST] hook removed");
#endif
}

void scope_begin()
{
    if (g_target_known)
        g_active.store(true, std::memory_order_relaxed);
    mgpu::diag::info(g_target_known
        ? "[ARCHTEST] rewrite scope ARMED (private DLSS-NR feature creation)"
        : "[ARCHTEST] rewrite scope requested but identity is unknown - staying disarmed");
}

void scope_end()
{
    g_active.store(false, std::memory_order_relaxed);
    mgpu::diag::info(g_rewrote.load()
        ? "[ARCHTEST] rewrite scope DISARMED (a rewrite was applied this scope)"
        : "[ARCHTEST] rewrite scope DISARMED (no rewrite was applied this scope)");
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
