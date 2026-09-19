// ============================================================================
// PROCESSCONTEXT-AB - NVAPI bring-up, the NeuralScreen-compatible architecture
// patch, module loading, logging and hashing.
//
// Nothing here creates a device, submits work, or touches NGX. It is the part
// of the reference implementation that is identical in every mode, so that the
// four modes differ ONLY in what the process created before the RTX 4070 lane
// starts.
// ============================================================================

#include "nv.h"

#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "MinHook.h"

#pragma comment(lib, "bcrypt.lib")

namespace pcab
{
namespace
{
    HMODULE g_nvapi = nullptr;
    pfn_query g_query = nullptr;
    bool g_nv_ready = false;
    int  g_init_result = 0;

    HMODULE g_core = nullptr;
    HMODULE g_snippet = nullptr;

    // ---- architecture patch state ----
    //
    // ONE cached entry per physical GPU, filled BEFORE the hook exists. The
    // hook serves each GPU its own real result and rewrites exactly one of them.
    using pfn_get_arch = int(__cdecl *)(void *, NV_GPU_ARCH_INFO *);
    using pfn_enum = int(__cdecl *)(void **, unsigned *);
    using pfn_pci = int(__cdecl *)(void *, unsigned *, unsigned *, unsigned *, unsigned *);

    const unsigned MAX_ARCH_GPUS = 64;

    struct ArchEntry
    {
        void           *gpu;          // NvPhysicalGpuHandle, as an opaque value
        unsigned        raw_pci;      // NVAPI pDeviceId: (device << 16) | vendor
        bool            have_real;
        NV_GPU_ARCH_INFO real;
        bool            spoof;        // the ONE eligible NeuralScreen target
        unsigned        queried_ver;  // which version stamp the real query used
    };

    ArchEntry g_arch[MAX_ARCH_GPUS];
    unsigned  g_arch_count = 0;
    unsigned  g_arch_target_raw = 0;
    bool      g_arch_target_set = false;
    unsigned  g_arch_rewrites = 0;
    unsigned  g_arch_passthrough = 0;
    unsigned  g_arch_unknown = 0;
    unsigned  g_arch_too_small = 0;

    pfn_get_arch g_arch_original = nullptr;   // the real entry, via MinHook
    bool g_arch_active = false;

    FILE *g_log = nullptr;
    char  g_log_path[MAX_PATH]{};

    ArchEntry *find_arch(void *gpu)
    {
        for (unsigned i = 0; i < g_arch_count; ++i)
            if (g_arch[i].gpu == gpu) return &g_arch[i];
        return nullptr;
    }

    // How many bytes the CALLER's structure actually is. MAKE_NVAPI_VERSION
    // packs the size into the low 16 bits, so this is exactly the caller's
    // declared size and nothing may be written outside it.
    unsigned declared_size(const NV_GPU_ARCH_INFO *p)
    {
        return p->version & 0xFFFFu;
    }

    int __cdecl arch_detour(void *gpu, NV_GPU_ARCH_INFO *pInfo)
    {
        if (g_arch_original == nullptr) return -1;
        // NVIDIA's own code still serves the call, so every real error code and
        // every side effect is preserved. Only the returned VALUES are then
        // replaced, and only for the caller's declared structure.
        const int r = g_arch_original(gpu, pInfo);
        if (r != 0 || pInfo == nullptr) return r;

        ArchEntry *e = find_arch(gpu);
        if (e == nullptr)
        {
            // A handle cached before the hook was installed is the only kind
            // this patch knows how to speak for. An unknown one gets the
            // genuine answer, untouched.
            ++g_arch_unknown;
            return r;
        }

        if (!e->spoof)
        {
            // EVERY OTHER GPU GETS ITS OWN CACHED REAL RESULT.
            // Not the target's, and not a shared value: this is the whole point
            // of caching per handle before the hook went in.
            if (e->have_real && declared_size(pInfo) >= sizeof(NV_GPU_ARCH_INFO))
            {
                pInfo->architecture   = e->real.architecture;
                pInfo->implementation = e->real.implementation;
                pInfo->revision       = e->real.revision;
            }
            ++g_arch_passthrough;
            return r;
        }

        // The single eligible target. Rewrite ONLY when the runtime would
        // reject the real value - NeuralScreen's behaviour.
        if (declared_size(pInfo) < sizeof(NV_GPU_ARCH_INFO))
        {
            ++g_arch_too_small;
            logf("[arch] a caller declared only %u bytes for the architecture structure; "
                 "the spoof needs %u, so NOTHING is rewritten for that caller",
                 declared_size(pInfo), (unsigned)sizeof(NV_GPU_ARCH_INFO));
            return r;
        }
        if (pInfo->architecture != SPOOF_ARCHITECTURE)
        {
            pInfo->architecture   = SPOOF_ARCHITECTURE;
            pInfo->implementation = SPOOF_IMPLEMENTATION;
            pInfo->revision       = SPOOF_REVISION;
            ++g_arch_rewrites;
        }
        return r;
    }
}

// ------------------------------------------------------------------ logging

void log_open(const char *mode_name)
{
    std::snprintf(g_log_path, sizeof g_log_path, "context-%s.log", mode_name);
    g_log = std::fopen(g_log_path, "w");
    if (g_log != nullptr)
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::fprintf(g_log,
                     "PROCESSCONTEXT-AB  mode=%s  pid=%lu  started=%04d-%02d-%02d %02d:%02d:%02d\n",
                     mode_name, (unsigned long)GetCurrentProcessId(),
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::fflush(g_log);
    }
}

void log_close()
{
    if (g_log != nullptr)
    {
        std::fprintf(g_log, "---- end of log ----\n");
        std::fclose(g_log);
        g_log = nullptr;
    }
}

void logf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_log != nullptr)
    {
        std::fputs(buf, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}

// --------------------------------------------------------------------- hash

bool sha256_file(const wchar_t *path, char out_hex[65], unsigned long *out_size)
{
    out_hex[0] = '\0';
    if (out_size != nullptr) *out_size = 0;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        logf("[hash] CreateFileW failed for the NR DLL (GetLastError=%lu)",
             (unsigned long)GetLastError());
        return false;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    unsigned char digest[32];
    unsigned long total = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        static unsigned char buf[1 << 20];
        for (;;)
        {
            DWORD got = 0;
            if (!ReadFile(f, buf, (DWORD)sizeof buf, &got, nullptr)) break;
            if (got == 0) break;
            total += got;
            if (BCryptHashData(hash, buf, got, 0) != 0) { goto done; }
        }
        if (BCryptFinishHash(hash, digest, (ULONG)sizeof digest, 0) == 0)
        {
            for (int i = 0; i < 32; ++i)
                std::snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
            out_hex[64] = '\0';
            if (out_size != nullptr) *out_size = total;
            ok = true;
        }
    }

done:
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) logf("[hash] SHA256 of the NR DLL failed");
    return ok;
}

// -------------------------------------------------------------------- NVAPI

bool nv_load_and_initialize()
{
    if (g_nv_ready) return true;

    g_nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (g_nvapi == nullptr) g_nvapi = LoadLibraryW(L"nvapi64.dll");
    if (g_nvapi == nullptr)
    {
        logf("[nvapi] nvapi64.dll did not load (GetLastError=%lu)", (unsigned long)GetLastError());
        return false;
    }
    g_query = (pfn_query)(void *)GetProcAddress(g_nvapi, "nvapi_QueryInterface");
    if (g_query == nullptr)
    {
        logf("[nvapi] nvapi64.dll has no nvapi_QueryInterface export");
        return false;
    }

    // NvAPI_Initialize BEFORE any other id is used - NeuralScreen's ordering.
    void *init_raw = g_query(NVAPI_ID_INITIALIZE);
    if (init_raw == nullptr)
    {
        logf("[nvapi] NvAPI_Initialize did not resolve by id 0x%08X", NVAPI_ID_INITIALIZE);
        return false;
    }
    pfn_init init = (pfn_init)init_raw;
    g_init_result = init();

    logf("[nvapi] NvAPI_Initialize via nvapi_QueryInterface(0x%08X) -> %d (%s)",
         NVAPI_ID_INITIALIZE, g_init_result,
         g_init_result == 0 ? "NVAPI_OK" : "see nvapi.h NvApi_Status");
    if (g_init_result != 0)
    {
        logf("[nvapi] NvAPI_Initialize FAILED - this is not a context result, it is an "
             "environment result; the run answers nothing");
        return false;
    }

    g_nv_ready = true;
    return true;
}

void *nv_query(unsigned id)
{
    return (g_query != nullptr) ? g_query(id) : nullptr;
}

unsigned arch_real_value()
{
    if (g_arch_target_set)
    {
        ArchEntry *e = nullptr;
        for (unsigned i = 0; i < g_arch_count; ++i)
            if (g_arch[i].spoof) { e = &g_arch[i]; break; }
        if (e != nullptr && e->have_real) return e->real.architecture;
    }
    for (unsigned i = 0; i < g_arch_count; ++i)
        if (g_arch[i].have_real) return g_arch[i].real.architecture;
    return 0;
}

void nv_set_arch_target(unsigned vendor_id, unsigned device_id)
{
    // NvAPI_GPU_GetPCIIdentifiers returns the RAW NVAPI pDeviceId, which packs
    // the PCI device id in the HIGH 16 bits and the vendor id in the LOW 16 -
    // not a DXGI DeviceId. MGPU's T2 mapping uses the same packing.
    g_arch_target_raw = (device_id << 16) | (vendor_id & 0xFFFFu);
    g_arch_target_set = true;
    logf("[arch] spoof target: vendor=0x%04X device=0x%04X -> raw pDeviceId=0x%08X "
         "(the ONLY adapter whose architecture will be rewritten)",
         vendor_id & 0xFFFFu, device_id & 0xFFFFu, g_arch_target_raw);
}

bool arch_cache_real()
{
    if (g_arch_count > 0) return true;

    void *enum_raw = nv_query(NVAPI_ID_ENUM_PHYSICAL_GPUS);
    void *pci_raw  = nv_query(NVAPI_ID_GET_PCI_IDENTIFIERS);
    void *arch_raw = nv_query(NVAPI_ID_GET_ARCH_INFO);
    if (enum_raw == nullptr || arch_raw == nullptr)
    {
        logf("[arch] NVAPI enumeration/GetArchInfo entry points are unavailable - the "
             "architecture patch cannot be applied. The runtime may refuse the feature "
             "for an architecture reason that is NOT the hypothesis under test.");
        return false;
    }

    void *gpus[MAX_ARCH_GPUS] = {0};
    unsigned count = 0;
    ((pfn_enum)enum_raw)(gpus, &count);
    if (count > MAX_ARCH_GPUS) count = MAX_ARCH_GPUS;

    pfn_get_arch real_arch = (pfn_get_arch)arch_raw;
    g_arch_count = 0;

    for (unsigned i = 0; i < count; ++i)
    {
        ArchEntry e{};
        e.gpu = gpus[i];
        e.raw_pci = 0;
        e.have_real = false;
        e.queried_ver = 0;

        if (pci_raw != nullptr)
        {
            unsigned dev = 0, sub = 0, rev = 0, ext = 0;
            if (((pfn_pci)pci_raw)(gpus[i], &dev, &sub, &rev, &ext) == 0) e.raw_pci = dev;
        }

        // V2 first - it is the version that carries implementation and
        // revision. If the driver refuses it, NeuralScreen's V1 fallback is the
        // same structure with a V1 stamp, so no layout changes.
        e.real.version = NV_GPU_ARCH_INFO_VER_2;
        int r = real_arch(gpus[i], &e.real);
        if (r == 0)
        {
            e.queried_ver = 2;
        }
        else
        {
            e.real.version = NV_GPU_ARCH_INFO_VER_1;
            r = real_arch(gpus[i], &e.real);
            if (r == 0) e.queried_ver = 1;
        }
        e.have_real = (r == 0);

        e.spoof = (g_arch_target_set && e.raw_pci != 0 && e.raw_pci == g_arch_target_raw);

        logf("[arch] cached nvapi physical[%u] handle=%p vendor=0x%04X device=0x%04X "
             "real=%s arch=0x%X impl=0x%X rev=0x%X (queried V%u) %s",
             i, gpus[i], e.raw_pci & 0xFFFFu, (e.raw_pci >> 16) & 0xFFFFu,
             e.have_real ? "OK" : "FAILED",
             e.real.architecture, e.real.implementation, e.real.revision,
             e.queried_ver,
             e.spoof ? "-> SPOOF TARGET (this one is rewritten)"
                     : "-> left at its own real value");

        g_arch[g_arch_count++] = e;
    }

    if (g_arch_count == 0)
    {
        logf("[arch] NvAPI_EnumPhysicalGPUs returned no GPUs - nothing to cache and the "
             "architecture patch will not be installed");
        return false;
    }
    if (g_arch_target_set)
    {
        bool found = false;
        for (unsigned i = 0; i < g_arch_count; ++i) if (g_arch[i].spoof) found = true;
        if (!found)
            logf("[arch] the spoof target raw pDeviceId=0x%08X is NOT among the %u "
                 "enumerated NVIDIA GPUs - NO adapter will be rewritten. The DLSSNR "
                 "310.8.0 runtime is expected to refuse the real architecture, so a "
                 "failure downstream of this has a cause that is NOT the hypothesis "
                 "under test.", g_arch_target_raw, g_arch_count);
    }
    else
    {
        logf("[arch] no spoof target was named - every GPU will report its own real "
             "architecture and nothing will be rewritten");
    }
    return true;
}

bool arch_patch_install()
{
    if (g_arch_active) return true;
    if (g_arch_count == 0)
    {
        logf("[arch] arch_cache_real() has not run or cached nothing; the patch is NOT "
             "installed. Caching happens BEFORE the hook, on purpose: a value read "
             "through a hook that is already rewriting could be the rewritten one.");
        return false;
    }

    void *entry = nv_query(NVAPI_ID_GET_ARCH_INFO);
    if (entry == nullptr)
    {
        logf("[arch] NvAPI_GPU_GetArchInfo (0x%08X) did not resolve", NVAPI_ID_GET_ARCH_INFO);
        return false;
    }

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        logf("[arch] MH_Initialize -> %d", (int)st);
        return false;
    }

    void *original = nullptr;
    st = MH_CreateHook(entry, (LPVOID)&arch_detour, &original);
    if (st != MH_OK || original == nullptr)
    {
        logf("[arch] MH_CreateHook(NvAPI_GPU_GetArchInfo) -> %d - the patch is NOT "
             "installed", (int)st);
        return false;
    }
    g_arch_original = (pfn_get_arch)original;

    st = MH_EnableHook(entry);
    if (st != MH_OK)
    {
        logf("[arch] MH_EnableHook -> %d", (int)st);
        MH_RemoveHook(entry);
        g_arch_original = nullptr;
        return false;
    }

    g_arch_active = true;
    logf("[arch] patch installed over %u cached GPU(s): the spoof target -> 0x%X / impl "
         "0x%X / rev 0x%X, every other GPU -> its own cached real value",
         g_arch_count, SPOOF_ARCHITECTURE, SPOOF_IMPLEMENTATION, SPOOF_REVISION);
    return true;
}

void arch_patch_remove()
{
    if (!g_arch_active) return;
    void *entry = nv_query(NVAPI_ID_GET_ARCH_INFO);
    if (entry != nullptr) MH_DisableHook(entry);
    g_arch_active = false;
    logf("[arch] patch removed (target rewrites: %u, other-GPU passthroughs: %u, unknown "
         "handles: %u, too-small callers: %u)",
         g_arch_rewrites, g_arch_passthrough, g_arch_unknown, g_arch_too_small);
}

// ------------------------------------------------------------------ modules

bool ngx_load_core()
{
    if (g_core != nullptr) return true;

    // The driver-installed core announces itself in the registry; beside the
    // executable is the other documented location. NeuralScreen's worker resolves
    // it the same way and both routes are tried here, in that order.
    wchar_t dir[MAX_PATH * 2]{};
    bool have_dir = false;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
                      KEY_READ, &k) == ERROR_SUCCESS)
    {
        DWORD cb = sizeof dir;
        DWORD type = 0;
        if (RegQueryValueExW(k, L"FullPath", nullptr, &type, (LPBYTE)dir, &cb) == ERROR_SUCCESS &&
            type == REG_SZ)
        {
            have_dir = (dir[0] != L'\0');
        }
        RegCloseKey(k);
    }

    if (have_dir)
    {
        wchar_t full[MAX_PATH * 2]{};
        swprintf(full, MAX_PATH * 2, L"%s\\_nvngx.dll", dir);
        g_core = LoadLibraryW(full);
        logf("[ngx] core from registry: %ls -> %s", full, g_core ? "loaded" : "FAILED");
    }
    if (g_core == nullptr)
    {
        g_core = GetModuleHandleW(L"_nvngx.dll");
        if (g_core == nullptr) g_core = LoadLibraryW(L"_nvngx.dll");
        logf("[ngx] core from the loader search path -> %s",
             g_core ? "resident/loaded" : "NOT FOUND");
    }
    return g_core != nullptr;
}

bool ngx_load_snippet(const wchar_t *path)
{
    if (g_snippet != nullptr) return true;
    g_snippet = LoadLibraryW(path);
    logf("[ngx] snippet LoadLibraryW(\"%ls\") -> %s", path,
         g_snippet ? "loaded" : "FAILED");
    return g_snippet != nullptr;
}

HMODULE ngx_core() { return g_core; }
HMODULE ngx_snippet() { return g_snippet; }
}
