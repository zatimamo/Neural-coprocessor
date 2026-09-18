// ============================================================================
// MGPU Bridge - CUDADIAG: strictly observational instrumentation for the two
// NVAPI CUDA-interop calls the private DLSS-NR runtime makes during its
// CreateFeature.
//
// WHAT THIS IS
//   A pass-through diagnostic. It exists to answer ONE question that the stale
//   historical log could not answer in the current environment:
//
//     In a clean run where P1.0c Init and P4.1 Init both return Success, do
//     NvAPI_D3D12_GetCudaIndependentDescriptorObject and
//     NvAPI_D3D12_CreateCuModule still return NVAPI_ERROR (-1)?
//
//   and, if so, whether they are being called with the WRONG D3D12 device.
//
// WHAT THIS IS NOT
//   It is not an experiment and changes no behaviour. There is no A/B pair.
//   Every wrapper calls the genuine NVIDIA function exactly once with the
//   original arguments, returns the genuine NvApi_Status verbatim, and writes
//   nothing to any caller-owned structure.
//
// HARD PASS-THROUGH CONTRACT (enforced by construction - there is no code here
// that could violate it):
//   no retry, no status replacement, no output replacement, no forced handle,
//   no blob inspection, no descriptor modification, no CUDA capability spoof,
//   no proactive invocation of either target function.
//
// NO UNRELATED MUTEX ON THE NVIDIA CALL PATH
//   The wrappers must not call into MGPU's adapter module: adapter::
//   get_selection() takes a mutex, and acquiring an unrelated lock inside an
//   NVIDIA CUDA/NVAPI call is not something a diagnostic of that call may do.
//   The expected neural adapter is therefore PUSHED IN once, before any
//   interception exists, through set_expected_adapter() below, and the wrappers
//   read nothing but cuda_diag's own atomics.
//
// ABI NOTE - NVDX_ObjectHandle
//   NVIDIA declares handles with
//       #define NV_DECLARE_HANDLE(name)  struct name##__ { int unused; }; \
//                                        typedef struct name##__ *name
//   and NVDX_ObjectHandle is one of them. That declaration is NVIDIA's own and
//   it is PUBLIC - nvapi_lite_common.h carries
//       NV_DECLARE_HANDLE(NVDX_ObjectHandle);
//   It is reproduced here only because this build includes no NVAPI header, and
//   the vendored public nvapi.h uses the name without carrying that companion
//   declaration. The expansion is fixed by the macro, so the ABI is NVIDIA's.
//   The handle is never dereferenced - only its address is passed through, and
//   its value read after the call and only when that call succeeded.
// ============================================================================

#pragma once

// LUID, for set_expected_adapter below.
#include <windows.h>

namespace mgpu::cudadiag
{
    // --- log label only. Never consulted to decide whether to forward a call. ---
    void scope_startup_probe();   // P1.0c
    void scope_real_stream();     // P4.1
    void scope_outside();

    // Interface ids this module instruments. Public vendor interface facts,
    // quoted from NVIDIA/nvapi nvapi_interface.h:
    //   { "NvAPI_D3D12_GetCudaIndependentDescriptorObject", 0x0ddac234 }
    //   { "NvAPI_D3D12_CreateCuModule",                     0xad1a677d }
    constexpr unsigned ID_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT = 0x0DDAC234u;
    constexpr unsigned ID_CREATE_CU_MODULE = 0xAD1A677Du;

    // One-line status for the startup log: which variant of this module is in
    // the build. Compiled out entirely unless MGPU_CUDA_DIAG is defined.
    void log_variant();

    // The diagnostic's own copy of the expected neural adapter identity, pushed
    // in ONCE by the game thread from the existing T2 selection, BEFORE any
    // interception exists and before the private NR runtime is loaded. It lands
    // in cuda_diag's own atomics; the wrappers read only those. This is the
    // reason cuda_diag has no dependency on the adapter module and takes no lock.
    //
    // vendor_id/device_id are DXGI's VendorId/DeviceId, the same pair NVAPI
    // reports per physical GPU, so the diagnostic can name the neural card by
    // identity rather than by enumeration order.
    void set_expected_adapter(const LUID &luid, unsigned vendor_id, unsigned device_id);

    // The composition point. Given the id the runtime asked for and the GENUINE
    // pointer the real nvapi_QueryInterface returned, this returns either that
    // same pointer (nothing to do, or a diagnostic that could not be armed) or a
    // pass-through wrapper for it.
    //
    // The genuine pointer is published with compare_exchange_strong, never with
    // a plain store: an already-published pointer is never overwritten, and a
    // genuinely different later pointer is passed through UNWRAPPED.
    //
    // Returning `real` unchanged is always a correct answer, which is what makes
    // every failure path here fail open.
    void *select(unsigned int id, void *real);
}
