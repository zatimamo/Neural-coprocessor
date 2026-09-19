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
        bool     resolver_returned_null; // the interface was absent; nullptr returned

        // REAL results. -12345 means NOT OBSERVED. These slots are written ONLY
        // by a genuine call, so a probe's status cannot appear here.
        int      real_descriptor_status;
        int      real_cumodule_status;

        // PROBE results, kept separately for the same reason.
        int      descriptor_probe_status;
        int      cumodule_probe_status;
        int      probe_status;          // whichever probe was seen first

        unsigned long long first_blob_size;   // first REAL CreateCuModule blob size
    };

    // Ensures nvapi64.dll is resident, then detours nvapi_QueryInterface.
    bool observe_install(const char *mode);
    void observe_remove();
    ObserveSummary observe_summary();

    // The device the reference lane created, published BEFORE the lane makes any
    // NGX call, so that each private call can be reported as having reached that
    // device or not - the comparison CUDADIAG reports as match=yes/no.
    //
    // Until this is called, no comparison has happened, and the log says so
    // (match=NOT_COMPARED) rather than asserting match=no: a null device is only
    // a negative match when there is something to match it against.
    void observe_set_expected_device(void *device);
}
