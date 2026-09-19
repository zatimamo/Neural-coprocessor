// ============================================================================
// MGPU Bridge - RESHADENATIVE. See the implementation file for the full note.
//
// ONE QUESTION: is ReShade's D3D12 descriptor virtualization what makes the
// private NVAPI CUDA-interop lane return NVAPI_ERROR?
//
// ReShade's D3D12CreateDevice hook returns a PROXY device, and every CPU
// descriptor handle that proxy produces is synthetic - the heap index lives in
// bit 28 (d3d12_impl_device.cpp: "heap index begins at bit 28 on Win64"). The
// measured failing values are exactly that shape:
//
//     0x30000004   3 << 28 | 4
//     0x40000004   4 << 28 | 4
//     0x40000024   4 << 28 | 0x24
//
// A real driver CPU descriptor address is a pointer, e.g. 0x1A4BAD70000.
//
// DLSSNR resolves the private NVAPI entry directly (0x0DDAC234), so ReShade's
// convert_to_original_cpu_descriptor_handle() forwarding path is bypassed and
// NVIDIA is handed the synthetic handle. That is the hypothesis under test.
//
// This module unwraps the proxy to the ORIGINAL device and hands that to the
// NGX lane, so the descriptor that reaches NVIDIA is a real one.
//
// SCOPE - READ THIS BEFORE EXTENDING IT
//   It is applied to the P1.0c startup probe lane ONLY, because that lane is
//   fully separable: it creates no textures, no buffers and no transport, so
//   running the whole lane on the native device mixes nothing. The P4.1 stream
//   lane is NOT separable - one command list and queue (s.nl/s.nq) carry the
//   transport copies, the NGX Evaluate calls AND the CreateFeature arm, and
//   ndev creates every NR texture the feature touches. Moving that lane to the
//   native device would either mix proxy resources with a native list or move
//   the transport too. Neither is done here.
//
// Not an experiment with two arms: there is one build, and the option decides
// whether the P1.0c lane runs on the unwrapped device.
// ============================================================================

#pragma once

#include <windows.h>

// Forward declaration keeps this header free of d3d12.h, the way the other
// experiment headers stay free of ReShade's headers.
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace mgpu::reshadenative
{
    // One line for the startup log: which variant of this module is in the
    // build. Compiled out unless MGPU_RESHADE_NATIVE is defined.
    void log_variant();

    // Called once, immediately after MGPU's GPU 1 device has been created and
    // validated. Attempts the unwrap and, on success, builds the native lane.
    //
    // STRICT: the unwrap only counts when
    //   * QueryInterface(IID_UnwrappedObject) succeeds,
    //   * the returned pointer is non-null AND different from the proxy,
    //   * its GetAdapterLuid() equals selected_luid exactly.
    // On any failure it logs why and stays inactive - ngx_device() then returns
    // its argument unchanged, so the run is the pre-existing behaviour and the
    // log says the unwrap did not happen rather than implying it did.
    bool init(ID3D12Device *reshade_proxy_device, const LUID &selected_luid);

    // True only when the unwrap succeeded AND the native lane exists.
    bool active();

    // The device the NGX lane should use: the unwrapped original when active,
    // otherwise the argument, unchanged.
    ID3D12Device *ngx_device(ID3D12Device *fallback);

    // A DIRECT queue on the native device, for executing a list recorded on it.
    // Returns the argument unchanged when inactive.
    ID3D12CommandQueue *ngx_queue(ID3D12CommandQueue *fallback);

    // Diagnostic only, and NON-NVAPI: creates one tiny descriptor heap on each
    // device, logs both CPU start handles and releases both immediately. The
    // heap is never handed to NGX and no private NVAPI call is made.
    // Called by init(); exposed so the fingerprint can be re-read from a log.
    void log_descriptor_fingerprint();

    // Releases the native objects, in the right order: the native queue first,
    // then the native device. It is provided but deliberately NOT called from
    // the bridge today, and that is a decision rather than an omission: the NGX
    // session is initialised on the native device and outlives the probe, and
    // this codebase has not reasoned about an NGX-session-vs-device teardown
    // order. Retaining the reference until process exit is the safe reading, and
    // the process exit reclaims it. Call this only after the NGX session that
    // was initialised on this device has been shut down.
    void shutdown();
}
