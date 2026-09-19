// ============================================================================
// PROCESSCONTEXT-AB - observation interface.
//
// The observer is a pure pass-through. It exists so that the diagnostic can say
// what the two private NVAPI CUDA-interop calls returned, in exactly the same
// terms MGPU's CUDADIAG reports them, without influencing any of it.
// ============================================================================

#pragma once

namespace pcab
{
    struct ObserveSummary
    {
        unsigned descriptor_calls;      // total invocations, probes included
        unsigned cumodule_calls;
        unsigned descriptor_probes;     // null-argument capability probes
        unsigned cumodule_probes;
        bool     mismatch;              // the resolver returned a second pointer

        int      real_descriptor_status;  // status of the REAL call, or -12345
        int      real_cumodule_status;    // status of the REAL call, or -12345
        int      probe_status;            // status seen for a probe, or -12345
        unsigned long long first_blob_size;
    };

    // Ensures nvapi64.dll is resident, then detours nvapi_QueryInterface.
    bool observe_install(const char *mode);
    void observe_remove();
    ObserveSummary observe_summary();
}
