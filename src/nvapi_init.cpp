// ============================================================================
// MGPU Bridge - NVAPIINIT implementation. See nvapi_init.hpp.
//
// Independently written for this experiment. No third-party source is copied.
//
// Uses MGPU's existing dynamic-NVAPI mechanism - GetModuleHandleW /
// LoadLibraryW("nvapi64.dll") + GetProcAddress("nvapi_QueryInterface") +
// the documented interface id - exactly as the [MGPU][RFX] block already does.
// No new link library and no NVAPI header are added to the build.
//
// The interface id is a public vendor interface fact:
//     NVIDIA/nvapi nvapi_interface.h : { "NvAPI_Initialize", 0x0150e828 }
// It is cited rather than guessed, and it is the same constant gpu1_context.cpp
// already carries for the Reflex path.
// ============================================================================

#include "nvapi_init.hpp"

#include <windows.h>
#include <atomic>
#include <cstdio>

#include "diag.hpp"

namespace mgpu::nvapiinit
{
namespace
{
    // { "NvAPI_Initialize", 0x0150e828 } - NVIDIA/nvapi nvapi_interface.h
    const unsigned NVAPI_ID_INITIALIZE = 0x0150E828u;

    // NVAPI_OK
    const int NVAPI_OK = 0;

    using pfn_query = void *(*)(unsigned int);
    using pfn_init  = int (*)(void);

    std::atomic<bool> g_attempted{false};
    std::atomic<bool> g_usable{false};
    int  g_init_result = 0;
    bool g_called_ourselves = false;

    void *g_nvapi_module = nullptr;
    void *g_query = nullptr;

    const char *nvapi_status_name(int s)
    {
        switch (s)
        {
            case 0:    return "NVAPI_OK";
            case -1:   return "NVAPI_ERROR";
            case -2:   return "NVAPI_LIBRARY_NOT_FOUND";
            case -3:   return "NVAPI_NO_IMPLEMENTATION";
            case -4:   return "NVAPI_API_NOT_INITIALIZED";
            case -5:   return "NVAPI_INVALID_ARGUMENT";
            case -6:   return "NVAPI_NVIDIA_DEVICE_NOT_FOUND";
            case -38:  return "NVAPI_API_IN_USE";
            default:   return "see nvapi.h NvAPI_Status";
        }
    }
}

void log_variant()
{
#ifdef MGPU_NVAPI_EXPLICIT_INIT
    mgpu::diag::info("[NVAPIINIT] variant=initialized");
#else
    mgpu::diag::info("[NVAPIINIT] variant=control");
#endif
}

bool initialize_once()
{
    if (g_attempted.load(std::memory_order_acquire))
        return g_usable.load(std::memory_order_relaxed);

    g_attempted.store(true, std::memory_order_release);

#ifndef MGPU_NVAPI_EXPLICIT_INIT
    // nvapi_control: current behaviour. No explicit NvAPI_Initialize anywhere on
    // this path, and no NVAPI module is loaded by this file.
    return false;
#else
    HMODULE m = GetModuleHandleW(L"nvapi64.dll");
    if (m == nullptr)
        m = LoadLibraryW(L"nvapi64.dll");
    if (m == nullptr)
    {
        mgpu::diag::warn("[NVAPIINIT] nvapi64.dll not present - NvAPI_Initialize NOT called");
        return false;
    }
    g_nvapi_module = m;

    void *q_raw = reinterpret_cast<void *>(GetProcAddress(m, "nvapi_QueryInterface"));
    if (q_raw == nullptr)
    {
        mgpu::diag::warn("[NVAPIINIT] nvapi_QueryInterface not exported - "
                         "NvAPI_Initialize NOT called");
        return false;
    }
    g_query = q_raw;

    const pfn_query q = reinterpret_cast<pfn_query>(g_query);
    void *init_raw = q(NVAPI_ID_INITIALIZE);
    if (init_raw == nullptr)
    {
        mgpu::diag::warn("[NVAPIINIT] NvAPI_Initialize did not resolve - NOT called");
        return false;
    }

    const pfn_init init = reinterpret_cast<pfn_init>(init_raw);
    const int r = init();
    g_init_result = r;
    g_called_ourselves = true;

    char l[224];
    std::snprintf(l, sizeof l,
                  "[NVAPIINIT] NvAPI_Initialize result=%d (%s)", r, nvapi_status_name(r));
    if (r == NVAPI_OK) mgpu::diag::info(l); else mgpu::diag::error(l);

    // The header states Initialize "initializes the NvAPI library (if not
    // already initialized) but always increments the ref-counter", and exposes
    // no way to ask whether it was already initialized. This line therefore
    // reports only what is knowable: that the call succeeded. It does not claim
    // we performed the first initialization in the process.
    mgpu::diag::info("[NVAPIINIT] NVAPI is initialized (ref-counter incremented by 1; "
                     "whether it was already initialized is not queryable through any "
                     "documented NvAPI entry)");

    g_usable.store(r == NVAPI_OK, std::memory_order_relaxed);
    return r == NVAPI_OK;
#endif
}

bool attempted()
{
    return g_attempted.load(std::memory_order_relaxed);
}

bool usable()
{
#ifdef MGPU_NVAPI_EXPLICIT_INIT
    return g_usable.load(std::memory_order_relaxed);
#else
    // control: nothing was initialised, so nothing is usable. The caller's
    // hard stop is compiled out of the control arm entirely, so this value is
    // never consulted there.
    return false;
#endif
}

int init_result()
{
    return g_init_result;
}

bool called_ourselves()
{
    return g_called_ourselves;
}

void log_core_path()
{
    // Diagnostic only, identical in BOTH variants. Does not load the module and
    // does not hash it: a path is enough to identify which copy the process
    // actually bound, which nothing in the log currently records.
    HMODULE core = GetModuleHandleW(L"_nvngx.dll");
    if (core == nullptr)
    {
        mgpu::diag::info("[NVAPIINIT] _nvngx.dll path=\"(not resident in this process)\"");
        return;
    }

    wchar_t wpath[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(core, wpath, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2)
    {
        char l[160];
        std::snprintf(l, sizeof l,
                      "[NVAPIINIT] _nvngx.dll path=\"(GetModuleFileNameW failed, err=%lu)\"",
                      (unsigned long)GetLastError());
        mgpu::diag::warn(l);
        return;
    }

    char path[MAX_PATH * 4] = {};
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, (int)sizeof path, nullptr, nullptr);

    char l[1200];
    std::snprintf(l, sizeof l, "[NVAPIINIT] _nvngx.dll path=\"%s\"", path);
    mgpu::diag::info(l);

    // The same line for the private NR snippet is useful for the same reason and
    // costs nothing extra: it names the copy the process bound in THIS session.
    HMODULE snip = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (snip != nullptr)
    {
        wchar_t ws[MAX_PATH * 2] = {};
        const DWORD sn = GetModuleFileNameW(snip, ws, MAX_PATH * 2);
        if (sn != 0 && sn < MAX_PATH * 2)
        {
            char spath[MAX_PATH * 4] = {};
            WideCharToMultiByte(CP_UTF8, 0, ws, -1, spath, (int)sizeof spath, nullptr, nullptr);
            char sl[1200];
            std::snprintf(sl, sizeof sl, "[NVAPIINIT] nvngx_dlssnr.dll path=\"%s\"", spath);
            mgpu::diag::info(sl);
        }
    }
}

void log_lifetime_note()
{
    // Teardown is deliberately NOT an experimental variable in this A/B. The
    // one reference taken by initialize_once() is retained until process
    // termination in BOTH arms - in the control arm there is no reference at
    // all. No NvAPI_Unload is called anywhere in this file, so the two arms
    // cannot differ by teardown behaviour.
    //
    // This line is emitted unconditionally, from the same source in both arms,
    // so the teardown logging is byte-identical apart from runtime values.
    mgpu::diag::info("[NVAPIINIT] explicit NVAPI reference retained until process "
                     "termination for experiment");
}
}
