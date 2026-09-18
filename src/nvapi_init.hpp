// ============================================================================
// MGPU Bridge - NVAPIINIT: explicit-NVAPI-initialization A/B
// ============================================================================
// ONE QUESTION, ONE BEHAVIOUR.
//
//   The private DLSS-NR runtime fails CreateFeature like this:
//
//     NGXCubinD3D12::CreateOrRetrieveSurfaceData: error:
//         NvAPI_D3D12_GetCudaIndependentDescriptorObject failed - nvapi status -1
//     NGXCubinD3D12::CreateKernel: error:
//         NGXCubinD3D12::CreateKernel NvAPI_D3D12_CreateCuModule failed - nvapi status -1
//     CG2RNetworkManager::BuildActiveNetwork: DLSSNR: init_kernels() failed
//
//   NVIDIA's own header, verbatim:
//
//     "This function initializes the NvAPI library (if not already
//      initialized) but always increments the ref-counter.
//      This must be called before calling other NvAPI_ functions.
//      Note: It is now mandatory to call NvAPI_Initialize before calling
//      any other NvAPI."
//
//   MGPU's normal path never calls it; the only call lives in the [MGPU][RFX]
//   Reflex diagnostic, which ran in none of the failing sessions. NeuralScreen
//   calls it before any architecture query.
//
//   So: does explicitly initialising NVAPI, before MGPU makes ANY NVAPI call,
//   change CreateFeature(Reserved18) from 0xBAD00002 into Success or another
//   result?
//
// TWO VARIANTS, ONE SOURCE REVISION
//   nvapi_control     : current behaviour. No explicit NvAPI_Initialize.
//   nvapi_initialized : NvAPI_Initialize once, before every NVAPI call MGPU
//                       makes, and before the private runtime is loaded.
//
// BOTH build with MGPU_ARCH_COMPAT=ON and MGPU_CONTRACT_EXPANDED=ON, so the
// architecture spoof, the GPU identity match, the caller, the NR DLL, the
// driver, the expanded CreateFeature contract, the resolution and the transport
// are all frozen and identical. The single variable is this file.
//
// LIFETIME
//   NvAPI_Initialize is reference-counted and increments on every call. This
//   module takes exactly ONE reference, before anything else touches NVAPI, and
//   HOLDS it for the add-on lifetime.
//
//   It deliberately does NOT call NvAPI_Unload. MGPU cannot establish that it
//   is the last user: the private DLSS-NR snippet, the game's own DLSS, the
//   Streamline components and MGPU's own [MGPU][RFX] block all sit on the same
//   process-wide counter, and [RFX] calls NvAPI_Unload unconditionally after
//   its own Initialize. Dropping our reference from a teardown that is not
//   provably last could unload NVAPI underneath DLSS-NR while it is still
//   using it - the one thing the experiment must not do. The reference is
//   therefore released by process exit, which is the same lifetime choice MGPU
//   already makes for its NGX session and its GPU 1 device.
// ============================================================================

#pragma once

namespace mgpu::nvapiinit
{
    // The one line that identifies which variant a log came from.
    void log_variant();

    // Take the single NVAPI reference, before any other NVAPI call in this
    // process. Idempotent: the second and later calls do nothing.
    //
    // Returns true when NVAPI is usable (either we initialised it or it was
    // already initialised). Returns false only when nvapi64.dll or
    // nvapi_QueryInterface or the Initialize entry could not be reached, or
    // when NvAPI_Initialize itself reported a failure.
    bool initialize_once();

    // True when initialize_once() has completed in this process.
    bool attempted();

    // True only when the explicit initialization actually succeeded. Always
    // false in the control variant, which takes no reference and initialises
    // nothing. This is the gate for the hard stop in the caller.
    bool usable();

    // The result NvAPI_Initialize returned, and whether we called it ourselves
    // at all. Both are 0/no in the control variant by construction.
    int  init_result();
    bool called_ourselves();

    // Diagnostic-only, identical code in BOTH variants: the full path of the
    // resident NGX core, or an explicit "not resident" note. Never hashes and
    // never loads anything.
    void log_core_path();

    // Report the unload decision once, at the end of an ordered teardown.
    void log_lifetime_note();
}
