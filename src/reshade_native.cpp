// ============================================================================
// MGPU Bridge - RESHADENATIVE implementation. See reshade_native.hpp for the
// contract and for why the scope is the P1.0c lane only.
//
// Independently written for this experiment. No third-party source is copied.
// The GUID below is the ONE vendor fact this file carries, and it is written
// out rather than included because ReShade's internal D3D12 device interface
// is not part of its public headers.
// ============================================================================

#include "reshade_native.hpp"

#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdio>

#include "diag.hpp"

namespace mgpu::reshadenative
{
namespace
{
    // ---------------------------------------------------------------------
    // ReShade's IID_UnwrappedObject, for the ReShade build this repository is
    // pinned to (crosire/reshade @ 18deaa52de0c425a78b329e9cb3c497281cd00ec,
    // the same SHA .github/workflows/*.yml fetches into ext/reshade).
    //
    // The proxy D3D12 device implements this interface so that an add-on can
    // ask for the ORIGINAL object the proxy wrapped. It is internal to ReShade
    // (d3d12_impl_device.cpp) and therefore not declared in reshade.hpp, so it
    // is reproduced here exactly as ReShade defines it.
    //
    //     {7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42}
    //
    // If a future ReShade pin changes this GUID the QueryInterface below simply
    // fails, init() logs "the unwrap did not happen" and the lane falls back to
    // the proxy device - it cannot silently produce a half-unwrapped device.
    // ---------------------------------------------------------------------
    const GUID IID_ReShadeUnwrappedObject =
        {0x7F2C9A11, 0x3B4E, 0x4D6A, {0x81, 0x2F, 0x5E, 0x9C, 0xD3, 0x7A, 0x1B, 0x42}};

    // The unwrapped original, AddRef'd by QueryInterface and held until
    // shutdown(). Never released earlier: the NGX session outlives the probe.
    ID3D12Device *g_native = nullptr;
    ID3D12Device *g_proxy  = nullptr;   // borrowed, for logging only
    ID3D12CommandQueue *g_queue = nullptr;   // owned, on g_native
    LUID g_native_luid{};
    std::atomic<bool> g_active{false};

    const char *luid_text(const LUID &l, char *buf, size_t n)
    {
        std::snprintf(buf, n, "%08X-%08X", (unsigned)l.HighPart, (unsigned)l.LowPart);
        return buf;
    }

    // Diagnostic only. Creates the smallest possible non-shader-visible
    // descriptor heap on each device, reads the CPU start handle and releases
    // it. This is the non-NVAPI proof of the difference: a proxy heap yields a
    // synthetic handle, a native heap yields a driver address.
    void fingerprint_one(ID3D12Device *dev, const char *label)
    {
        if (dev == nullptr)
        {
            char l[256];
            std::snprintf(l, sizeof l, "[RESHADENATIVE] %s descriptor heap: device is null", label);
            mgpu::diag::warn(l);
            return;
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;   // CPU-visible only

        ID3D12DescriptorHeap *heap = nullptr;
        const HRESULT hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        if (FAILED(hr) || heap == nullptr)
        {
            char l[256];
            std::snprintf(l, sizeof l,
                          "[RESHADENATIVE] %s descriptor heap CreateDescriptorHeap hr=0x%08X - "
                          "fingerprint unavailable for this device", label, (unsigned)hr);
            mgpu::diag::warn(l);
            if (heap != nullptr) heap->Release();
            return;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        char l[320];
        std::snprintf(l, sizeof l,
                      "[RESHADENATIVE] %s heap CPU start handle = 0x%llX  (type=CBV_SRV_UAV "
                      "count=1 flags=NONE)",
                      label, (unsigned long long)h.ptr);
        mgpu::diag::info(l);

        heap->Release();   // released immediately; never handed to NGX
    }

    void report_failure(const char *why, HRESULT hr)
    {
        char l[400];
        std::snprintf(l, sizeof l,
                      "[RESHADENATIVE] the unwrap did NOT happen: %s (hr=0x%08X). The P1.0c lane "
                      "keeps using the ReShade proxy device, so this run is the pre-existing "
                      "behaviour and proves nothing about the descriptor hypothesis.",
                      why, (unsigned)hr);
        mgpu::diag::error(l);
    }
}

void log_variant()
{
#ifdef MGPU_RESHADE_NATIVE
    mgpu::diag::info("[RESHADENATIVE] variant=reshade_native (P1.0c NGX lane on the unwrapped "
                     "original device; P4.1 stream lane unchanged)");
#else
    mgpu::diag::info("[RESHADENATIVE] variant=off (no unwrap; this build is the pre-existing "
                     "behaviour)");
#endif
}

bool init(ID3D12Device *reshade_proxy_device, const LUID &selected_luid)
{
#ifdef MGPU_RESHADE_NATIVE
    if (g_active.load(std::memory_order_acquire))
        return true;

    char lbuf[64];

    if (reshade_proxy_device == nullptr)
    {
        report_failure("the GPU 1 device is null", E_POINTER);
        return false;
    }
    g_proxy = reshade_proxy_device;

    {
        char l[360];
        std::snprintf(l, sizeof l,
                      "[RESHADENATIVE] reshade_proxy_device=%p selected_luid=%s - attempting "
                      "QueryInterface(IID_UnwrappedObject)", (void *)reshade_proxy_device,
                      luid_text(selected_luid, lbuf, sizeof lbuf));
        mgpu::diag::info(l);
    }

    ID3D12Device *native = nullptr;
    const HRESULT hr = reshade_proxy_device->QueryInterface(
        IID_ReShadeUnwrappedObject, reinterpret_cast<void **>(&native));

    if (FAILED(hr) || native == nullptr)
    {
        report_failure("QueryInterface(IID_UnwrappedObject) failed - this ReShade build does not "
                       "expose the unwrapped object", hr);
        if (native != nullptr) native->Release();
        return false;
    }
    if (native == reshade_proxy_device)
    {
        report_failure("IID_UnwrappedObject returned the proxy itself, so nothing was unwrapped",
                       E_UNEXPECTED);
        native->Release();
        return false;
    }

    // The unwrap is only meaningful if it landed on the SAME physical adapter
    // T2 selected. A different LUID would silently re-bind every downstream
    // result, which is the failure mode this whole project exists to exclude.
    const LUID nl = native->GetAdapterLuid();
    if (nl.LowPart != selected_luid.LowPart || nl.HighPart != selected_luid.HighPart)
    {
        char l[360];
        std::snprintf(l, sizeof l,
                      "[RESHADENATIVE] native LUID MISMATCH: native=%08X-%08X selected=%08X-%08X - "
                      "releasing the native device and staying on the proxy (a different adapter "
                      "would make every downstream result meaningless)",
                      (unsigned)nl.HighPart, (unsigned)nl.LowPart,
                      (unsigned)selected_luid.HighPart, (unsigned)selected_luid.LowPart);
        mgpu::diag::error(l);
        native->Release();
        return false;
    }

    // A DIRECT queue on the native device. The P1.0c probe's init list is
    // recorded on the native device, so it must be executed on a native queue -
    // never on the stream's ReShade queue.
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    ID3D12CommandQueue *nq = nullptr;
    const HRESULT qhr = native->CreateCommandQueue(&qd, IID_PPV_ARGS(&nq));
    if (FAILED(qhr) || nq == nullptr)
    {
        char l[360];
        std::snprintf(l, sizeof l,
                      "[RESHADENATIVE] CreateCommandQueue on ngx_native_device failed hr=0x%08X - "
                      "without a native queue the init list could not be drained, so the lane "
                      "stays on the proxy", (unsigned)qhr);
        mgpu::diag::error(l);
        native->Release();
        return false;
    }

    g_native = native;                 // owns the AddRef from QueryInterface
    g_queue = nq;
    g_native_luid = nl;
    g_active.store(true, std::memory_order_release);

    {
        char l[520];
        std::snprintf(l, sizeof l,
                      "[RESHADENATIVE] reshade_proxy_device=%p ngx_native_device=%p "
                      "ngx_native_queue=%p  proxy pointer != native pointer  |  "
                      "native LUID %s == selected RTX4070 LUID  |  proxy device refcounted by "
                      "the bridge, native device AddRef'd by QueryInterface and held until "
                      "shutdown",
                      (void *)reshade_proxy_device, (void *)g_native, (void *)g_queue,
                      luid_text(nl, lbuf, sizeof lbuf));
        mgpu::diag::info(l);
    }

    log_descriptor_fingerprint();
    return true;
#else
    (void)reshade_proxy_device;
    (void)selected_luid;
    return false;
#endif
}

bool active()
{
    return g_active.load(std::memory_order_acquire);
}

ID3D12Device *ngx_device(ID3D12Device *fallback)
{
#ifdef MGPU_RESHADE_NATIVE
    if (g_active.load(std::memory_order_acquire) && g_native != nullptr)
        return g_native;
#endif
    return fallback;
}

ID3D12CommandQueue *ngx_queue(ID3D12CommandQueue *fallback)
{
#ifdef MGPU_RESHADE_NATIVE
    if (g_active.load(std::memory_order_acquire) && g_queue != nullptr)
        return g_queue;
#endif
    return fallback;
}

void log_descriptor_fingerprint()
{
#ifdef MGPU_RESHADE_NATIVE
    if (g_proxy == nullptr || g_native == nullptr)
    {
        mgpu::diag::warn("[RESHADENATIVE] descriptor fingerprint skipped - no proxy/native pair");
        return;
    }
    mgpu::diag::info("[RESHADENATIVE] descriptor fingerprint (diagnostic only, non-NVAPI, no heap "
                     "is handed to NGX):");
    fingerprint_one(g_proxy, "reshade_proxy_device");
    fingerprint_one(g_native, "ngx_native_device ");
    mgpu::diag::info("[RESHADENATIVE] expected shape: the proxy handle carries the heap index in "
                     "bit 28 (synthetic), the native handle is an ordinary driver CPU descriptor "
                     "address");
#endif
}

void shutdown()
{
#ifdef MGPU_RESHADE_NATIVE
    if (g_queue != nullptr) { g_queue->Release(); g_queue = nullptr; }
    // The native device reference is intentionally NOT dropped before the NGX
    // session is closed: the session was initialised on this device, and
    // releasing the device out from under it is a use-after-free waiting for a
    // teardown order that has not been reasoned about. It is released here, at
    // shutdown, and the process exit reclaims it either way.
    if (g_native != nullptr) { g_native->Release(); g_native = nullptr; }
    g_proxy = nullptr;
    g_active.store(false, std::memory_order_release);
#endif
}
}
