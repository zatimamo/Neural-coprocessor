// ============================================================================
// MGPU Bridge - ARCHTEST: architecture-compatibility A/B experiment
// ============================================================================
// ONE QUESTION, ONE BEHAVIOUR.
//
//   Does presenting Blackwell architecture information to the private DLSS-NR
//   runtime, for the RTX 4070 it runs on, change CreateFeature(Reserved18)
//   from 0xBAD00002 into Success or another result?
//
// WHAT THIS IS
//   A process-local, temporally scoped interception of exactly one NVAPI
//   entry point: NvAPI_GPU_GetArchInfo. It is the single call the runtime's
//   own NGXCubinGeneric::SetGPUArch uses to decide whether the GPU is
//   acceptable - its strings name it:
//       "SetGPUArch:: NvAPI_GPU_GetArchInfo failed with error: %d"
//       "DLSSNR: Unsupported GPU architecture 0x%x, minimum required 0x%x"
//
// WHAT THIS IS NOT
//   Not a redesign. It does not create a device, a queue, an allocator, a
//   command list, a parameter block or a private runtime. It does not touch
//   the transport, the presentation path, the DLSS parameters, the feature
//   dimensions or CreateFeature. It reaches MGPU's existing P4.1 path and
//   adds one hook around one call.
//
// SCOPE - TWO AXES, BOTH ENFORCED
//   Identity : the rewrite applies only to the physical GPU whose PCI
//              identity (vendor << 16 | device) equals the SELECTED neural
//              adapter's. The RTX 4070 is 0x10DE2786 and the RTX 4070 Ti
//              SUPER is 0x10DE2705, so a mismatch is a refusal, never a
//              spoof. If identity cannot be established, NOTHING is
//              rewritten and the log says so.
//   Time     : the rewrite is armed only between arch_test_scope_begin()
//              and arch_test_scope_end(), which MGPU calls immediately
//              around the private DLSS-NR feature-creation scope. Outside
//              that window the hook is a pure pass-through.
//
// NO PERSISTENT CHANGE
//   Nothing on disk is modified. No NVIDIA binary is patched - the detour is
//   written into this process's own copy of nvapi64.dll code pages, in
//   memory, and is removed by arch_test_hook_remove().
//
// The real NVIDIA function is ALWAYS called first. The rewrite happens after
// it returns, on its output, and only when every gate above is satisfied.
#pragma once

namespace mgpu::archtest
{
    // The one line that identifies which build a log came from.
    void log_mode();

    // Say which GPU the rewrite is allowed to affect. Normalised the same way
    // NVAPI reports it: (vendor_id << 16) | device_id. Called with the T2
    // selection once it exists.
    void set_neural_gpu(unsigned vendor_id, unsigned device_id);

    // Install the NVAPI detour. Returns false if it could not be installed or
    // if this is the CONTROL build (which never intercepts anything), in which
    // case NOTHING is intercepted and the log says why.
    bool hook_install();

    // Remove the detour. Safe to call when no hook is installed.
    void hook_remove();

    // Arm / disarm the rewrite. Between these two calls the architecture
    // rewrite is live; outside them the hook passes everything through.
    void scope_begin();
    void scope_end();

    // Diagnostics for the surrounding MGPU code to bracket the call under test.
    void log_entering_create_feature();
    void log_create_feature_result(long result, void *handle);
}
