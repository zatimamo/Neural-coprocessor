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
    using pfn_get_arch = int(__cdecl *)(void *, NV_GPU_ARCH_INFO *);
    pfn_get_arch g_arch_original = nullptr;   // the real entry, via MinHook
    unsigned g_arch_real = 0;
    bool g_arch_active = false;
    unsigned g_arch_rewrites = 0;

    FILE *g_log = nullptr;
    char  g_log_path[MAX_PATH]{};

    int __cdecl arch_detour(void *hPhysicalGpu, NV_GPU_ARCH_INFO *pInfo)
    {
        const int r = g_arch_original(hPhysicalGpu, pInfo);
        if (r == 0 && pInfo != nullptr)
        {
            g_arch_real = pInfo->architecture;
            // Rewrite ONLY when the runtime would reject the real value. Same
            // behaviour as NeuralScreen: "architecture 0x%X is supported anyway
            // - no spoof needed".
            if (pInfo->architecture != SPOOF_ARCHITECTURE)
            {
                pInfo->architecture   = SPOOF_ARCHITECTURE;
                pInfo->implementation = SPOOF_IMPLEMENTATION;
                pInfo->revision       = SPOOF_REVISION;
                ++g_arch_rewrites;
            }
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

unsigned arch_real_value() { return g_arch_real; }

bool arch_patch_install()
{
    if (g_arch_active) return true;

    void *entry = nv_query(NVAPI_ID_GET_ARCH_INFO);
    if (entry == nullptr)
    {
        logf("[arch] NvAPI_GPU_GetArchInfo (0x%08X) did not resolve - the runtime may refuse "
             "the feature for an architecture reason that is NOT the hypothesis under test",
             NVAPI_ID_GET_ARCH_INFO);
        return false;
    }

    // Ask the real function once, so the log names the value being replaced.
    pfn_get_arch real = (pfn_get_arch)entry;
    void *gpus[64]{};
    unsigned count = 0;
    void *enum_raw = nv_query(NVAPI_ID_ENUM_PHYSICAL_GPUS);
    if (enum_raw != nullptr)
    {
        using pfn_enum = int(__cdecl *)(void **, unsigned *);
        ((pfn_enum)enum_raw)(gpus, &count);
    }
    NV_GPU_ARCH_INFO info{};
    info.version = NV_GPU_ARCH_INFO_VER_2;
    if (count > 0 && real(gpus[0], &info) == 0)
        g_arch_real = info.architecture;
    logf("[arch] GPU count=%u, real architecture=0x%X (0x190 is Ada / RTX 40; the DLSSNR "
         "310.8.0 runtime wants 0x%X)", count, g_arch_real, SPOOF_ARCHITECTURE);

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
        logf("[arch] MH_CreateHook(NvAPI_GPU_GetArchInfo) -> %d - the patch is NOT installed",
             (int)st);
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
    logf("[arch] patch installed: architecture 0x%X -> 0x%X / impl 0x%X / rev 0x%X "
         "(NVIDIA's own code still serves the call; only the returned value is rewritten)",
         g_arch_real, SPOOF_ARCHITECTURE, SPOOF_IMPLEMENTATION, SPOOF_REVISION);
    return true;
}

void arch_patch_remove()
{
    if (!g_arch_active) return;
    void *entry = nv_query(NVAPI_ID_GET_ARCH_INFO);
    if (entry != nullptr) MH_DisableHook(entry);
    g_arch_active = false;
    logf("[arch] patch removed (rewrites applied this run: %u)", g_arch_rewrites);
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
