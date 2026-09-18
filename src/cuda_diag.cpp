// ============================================================================
// MGPU Bridge - CUDADIAG implementation. See cuda_diag.hpp for the contract.
//
// Independently written for this diagnostic. No third-party source is copied.
//
// THE SIGNATURES BELOW ARE NOT RECONSTRUCTED. They are the declarations from
// the vendored NVIDIA public header, verbatim:
//
//   nvapi.h:18802
//     NVAPI_INTERFACE NvAPI_D3D12_GetCudaIndependentDescriptorObject(
//         __inout NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS* pParams);
//
//   nvapi.h:18828
//     NVAPI_INTERFACE NvAPI_D3D12_CreateCuModule(__in  ID3D12Device*      pDevice,
//                                                __in  const void*        pBlob,
//                                                __in  NvU32              size,
//                                                __out NVDX_ObjectHandle* phModule);
//
// NVAPI_INTERFACE is __cdecl on x64, so the wrappers are __cdecl.
//
// Both declarations carry NVIDIA's own marker:
//     // Experimental API for internal use. DO NOT USE!
// which is why they are undocumented publicly and why the struct is
// version-sized rather than fixed.
//
// DEPENDENCIES. This file includes diag.hpp and nothing else from MGPU. In
// particular it does NOT include adapter.hpp: see the header's "NO UNRELATED
// MUTEX ON THE NVIDIA CALL PATH".
// ============================================================================

#include "cuda_diag.hpp"

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdio>

#include "diag.hpp"

#ifdef MGPU_CUDA_DIAG

// ID3D12Device is a real type here, not a forward declaration: the wrappers
// take one as a parameter and GetAdapterLuid() is called on it. <d3d12.h>
// also gives us the genuine D3D12_CPU_DESCRIPTOR_HANDLE, which is part of the
// parameter block's ABI below and is therefore NOT reproduced by hand.
#include <d3d12.h>

// NvU64, as nvapi.h uses it.
using CUDADIAG_NvU64 = unsigned long long;

// NV_DECLARE_HANDLE(NVDX_ObjectHandle) expanded exactly:
//     struct NVDX_ObjectHandle__ { int unused; };
//     typedef struct NVDX_ObjectHandle__ *NVDX_ObjectHandle;
//
// The declaration is NVIDIA's own and it is PUBLIC: nvapi_lite_common.h carries
//     NV_DECLARE_HANDLE(NVDX_ObjectHandle);
// It is reproduced here only because this build includes NO NVAPI header at all,
// and the vendored public nvapi.h uses the name without carrying that companion
// declaration. The expansion is fixed by NVIDIA's macro, so the ABI is theirs,
// not ours. Opaque: never dereferenced anywhere in this file except, on success
// only, through read_module_handle() below.
struct NVDX_ObjectHandle__ { int unused; };
typedef struct NVDX_ObjectHandle__ *NVDX_ObjectHandle;

// NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_TYPE (nvapi.h:18784-18789)
enum CUDADIAG_DESCRIPTOR_OBJECT_TYPE
{
    CUDADIAG_DESC_OBJECT_SURFACE = 0,
    CUDADIAG_DESC_OBJECT_TEXTURE = 1,
    CUDADIAG_DESC_OBJECT_SAMPLER = 2,
};

// NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS (nvapi.h:18791-18800),
// field-for-field and type-for-type. Note that the struct is version-sized
// (structSizeIn/structSizeOut) rather than fixed, so nothing here may assume a
// field is present unless the caller's own size fields say it is.
struct CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS
{
    size_t                                   structSizeIn;
    size_t                                   structSizeOut;

    ID3D12Device                            *pDevice;
    CUDADIAG_DESCRIPTOR_OBJECT_TYPE          type;
    D3D12_CPU_DESCRIPTOR_HANDLE              desc;
    CUDADIAG_NvU64                           handle;
};

namespace
{
    using pfn_get_desc = int(__cdecl *)(CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS *);
    using pfn_create_module = int(__cdecl *)(ID3D12Device *, const void *, unsigned, NVDX_ObjectHandle *);

    // NvApi_Status values, from nvapi.h. NVAPI_OK is the ONLY success value and
    // it is the gate on every read of a caller-owned __out parameter.
    //
    // NVAPI_ERROR_UNREACHABLE is returned by EXACTLY ONE code path: the two
    // unreachable null-guards, which can only be reached if a wrapper were
    // entered before its genuine pointer was published. A genuine call's status
    // is never replaced by it - the two `return status;` statements are the only
    // returns on any path a runtime call can take.
    const int NVAPI_OK = 0;
    const int NVAPI_ERROR_UNREACHABLE = -1;

    // ---- the genuine pointers ------------------------------------------
    // FIRST publication uses compare_exchange_strong from nullptr. An
    // already-published pointer is NEVER overwritten: the wrapper that the
    // runtime was handed calls the pointer in this slot, so replacing the slot
    // behind its back would make the log describe a call that did not happen.
    //
    // Memory ordering: the CAS succeeds with release, and the wrapper reads the
    // slot with acquire, so a wrapper cannot observe nullptr - publication
    // happens before the wrapper is handed out.
    std::atomic<void *> g_real_desc{nullptr};
    std::atomic<void *> g_real_module{nullptr};

    // Set if a later resolver call ever returns a DIFFERENT pointer for an id we
    // already armed. That pointer is returned unwrapped and this is logged.
    std::atomic<bool> g_mismatch_desc{false};
    std::atomic<bool> g_mismatch_module{false};

    std::atomic<unsigned long long> g_calls_desc{0};
    std::atomic<unsigned long long> g_calls_module{0};

    // ---- observational scope label. NEVER consulted to forward a call. ----
    std::atomic<int> g_scope{0};   // 0 = outside, 1 = P1.0c, 2 = P4.1

    // ---- the expected neural adapter, pushed in once ---------------------
    // NOT read from the adapter module. adapter::get_selection() takes a mutex,
    // and this diagnostic may not acquire an unrelated lock inside an NVIDIA
    // CUDA/NVAPI call. The game thread publishes the identity here before any
    // interception exists; the wrappers only ever load these atomics.
    //
    // `known` is published LAST, with release, and read FIRST, with acquire, so
    // a reader that observes known==true is guaranteed to see the payload.
    std::atomic<unsigned long long> g_expected_luid_packed{0};
    std::atomic<unsigned> g_expected_pci{0};
    std::atomic<bool> g_expected_known{false};

    const char *scope_name(int s)
    {
        switch (s)
        {
            case 1: return "P1.0c";
            case 2: return "P4.1";
            default: return "outside";
        }
    }

    // Reads cuda_diag's own atomics. No lock, no call into another MGPU module.
    void expected_adapter(LUID &out, unsigned &pci, bool &known)
    {
        out = LUID{};
        pci = 0;
        known = g_expected_known.load(std::memory_order_acquire);
        if (!known)
            return;

        const unsigned long long packed = g_expected_luid_packed.load(std::memory_order_acquire);
        out.LowPart  = (unsigned long)(packed & 0xFFFFFFFFull);
        out.HighPart = (long)((packed >> 32) & 0xFFFFFFFFull);
        pci = g_expected_pci.load(std::memory_order_acquire);
    }

    // Logs pDevice plus its adapter LUID, compared against the pushed-in
    // expected neural adapter. GetAdapterLuid() is a local COM accessor; it
    // mutates nothing and is only called when pDevice != nullptr.
    void log_device(const char *fn, const char *phase, ID3D12Device *pDevice)
    {
        LUID want{};
        unsigned want_pci = 0;
        bool want_known = false;
        expected_adapter(want, want_pci, want_known);

        char l[400];

        if (pDevice == nullptr)
        {
            // A null device is only a negative MATCH when there is something to
            // match against. With no published identity no comparison happened,
            // and reporting match=no would assert a result this run never had.
            if (want_known)
            {
                std::snprintf(l, sizeof l,
                              "[CUDADIAG] %s %s pDevice=(null) expected_luid=%08X-%08X match=no "
                              "selected_pci=0x%08X",
                              fn, phase, (unsigned)want.HighPart, (unsigned)want.LowPart, want_pci);
            }
            else
            {
                std::snprintf(l, sizeof l,
                              "[CUDADIAG] %s %s pDevice=(null) expected=unknown "
                              "match=NOT_COMPARED (no identity was published by the game thread)",
                              fn, phase);
            }
            mgpu::diag::info(l);
            return;
        }

        const LUID got = pDevice->GetAdapterLuid();
        const bool match = want_known &&
                           got.LowPart == want.LowPart && got.HighPart == want.HighPart;

        if (want_known)
        {
            std::snprintf(l, sizeof l,
                          "[CUDADIAG] %s %s pDevice=%p GetAdapterLuid=%08X-%08X expected_luid=%08X-%08X "
                          "match=%s selected_pci=0x%08X",
                          fn, phase, (void *)pDevice,
                          (unsigned)got.HighPart, (unsigned)got.LowPart,
                          (unsigned)want.HighPart, (unsigned)want.LowPart,
                          match ? "yes" : "no", want_pci);
        }
        else
        {
            // No identity was pushed in, so no match can be asserted. Saying
            // "match=no" here would imply a comparison that never happened.
            std::snprintf(l, sizeof l,
                          "[CUDADIAG] %s %s pDevice=%p GetAdapterLuid=%08X-%08X expected=unknown "
                          "match=NOT_COMPARED (no identity was published by the game thread)",
                          fn, phase, (void *)pDevice,
                          (unsigned)got.HighPart, (unsigned)got.LowPart);
        }
        mgpu::diag::info(l);
    }

    // Version-sized read guard. A field occupying [off, off+len) is present
    // only if the size the caller declared covers it. Deliberately a free
    // function rather than a lambda inside a wrapper, so that the ONLY `return`
    // statements inside a wrapper are the two verbatim status returns and the
    // two unreachable null-guards - a property the scope gate in CI checks.
    bool field_present(size_t declared, size_t off, size_t len)
    {
        return off + len <= declared;
    }

    // ---- the ONLY two places either __out value is read -------------------
    // Each takes the status as a parameter and returns BEFORE touching the
    // output unless that status is NVAPI_OK.
    //
    // This is deliberately a function rather than an `if (status == NVAPI_OK) {
    // ... }` at each call site. It makes "never read a failed __out value" a
    // property of the ONLY code in this translation unit that can read it - one
    // guard, one early return, one read - instead of a property of one branch of
    // one function that a later edit could quietly move. A CI gate checks the
    // ordering, which is why the guard is the first statement and the read comes
    // after the `return;`.
    //
    // It matters here specifically because the historical target result is -1:
    // on the failure path the __out field is uninitialised caller memory, and
    // printing it would report garbage as a finding.
    void read_descriptor_handle(int status,
                               CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS *pParams,
                               bool output_size_known,
                               size_t output_size)
    {
        char l[400];

        // THE GUARD. Nothing below may read pParams->handle unless this returns.
        if (status != NVAPI_OK)
        {
            std::snprintf(l, sizeof l,
                          "[CUDADIAG]   returned handle NOT READ because NvApi_Status != NVAPI_OK "
                          "(status=%d)", status);
            mgpu::diag::info(l);
            return;
        }

        if (pParams == nullptr)
        {
            std::snprintf(l, sizeof l,
                          "[CUDADIAG]   returned handle NOT READ because pParams is null");
            mgpu::diag::info(l);
            return;
        }

        const size_t off =
            offsetof(CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS, handle);
        if (!output_size_known || !(off + sizeof(pParams->handle) <= output_size))
        {
            if (output_size_known)
                std::snprintf(l, sizeof l,
                              "[CUDADIAG]   returned handle NOT READ: structSizeOut=%zu does not "
                              "cover it (needs %zu)", output_size, off + sizeof(pParams->handle));
            else
                std::snprintf(l, sizeof l,
                              "[CUDADIAG]   returned handle NOT READ: the callee's declared output "
                              "size was not readable");
            mgpu::diag::info(l);
            return;
        }

        std::snprintf(l, sizeof l, "[CUDADIAG]   returned handle=0x%llX",
                      (unsigned long long)pParams->handle);
        mgpu::diag::info(l);
    }

    void read_module_handle(int status, NVDX_ObjectHandle *phModule)
    {
        char l[400];

        // THE GUARD. Nothing below may dereference phModule unless this returns.
        if (status != NVAPI_OK)
        {
            std::snprintf(l, sizeof l,
                          "[CUDADIAG]   module handle NOT READ because NvApi_Status != NVAPI_OK "
                          "(status=%d)", status);
            mgpu::diag::info(l);
            return;
        }

        if (phModule == nullptr)
        {
            std::snprintf(l, sizeof l,
                          "[CUDADIAG]   module handle NOT READ because phModule is null");
            mgpu::diag::info(l);
            return;
        }

        std::snprintf(l, sizeof l, "[CUDADIAG]   returned module handle=%p", (void *)(*phModule));
        mgpu::diag::info(l);
    }

    // ---- Wrapper 1 -------------------------------------------------------
    // NVAPI_D3D12_GetCudaIndependentDescriptorObject(pParams)
    int __cdecl diag_get_cuda_independent_descriptor_object(
        CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS *pParams)
    {
        const pfn_get_desc real = reinterpret_cast<pfn_get_desc>(
            g_real_desc.load(std::memory_order_acquire));   // acquire
        if (real == nullptr)
        {
            // UNREACHABLE BY CONSTRUCTION: select() publishes the pointer with a
            // release CAS BEFORE it hands the wrapper to the runtime.
            mgpu::diag::error("[CUDADIAG] GetCudaIndependentDescriptorObject wrapper entered with no "
                              "published real pointer - NOT calling anything, returning NVAPI_ERROR. "
                              "This line means the publication ordering is broken.");
            return NVAPI_ERROR_UNREACHABLE;
        }

        const unsigned long long n = g_calls_desc.fetch_add(1, std::memory_order_relaxed) + 1;
        const int scope = g_scope.load(std::memory_order_relaxed);
        const bool mismatched = g_mismatch_desc.load(std::memory_order_relaxed);

        char l[440];
        std::snprintf(l, sizeof l,
                      "[CUDADIAG] GetCudaIndependentDescriptorObject seq=%llu tid=%lu scope=%s "
                      "mismatch_seen=%s pParams=%p",
                      n, (unsigned long)GetCurrentThreadId(), scope_name(scope),
                      mismatched ? "yes" : "no", (void *)pParams);
        mgpu::diag::info(l);

        // ---- version-sized reads ----------------------------------------
        // The struct carries its own sizes, so a field is only touched when the
        // size that governs it says it is present. The order matters:
        // structSizeIn is at offset 0 and is the minimum a caller must provide;
        // structSizeOut is itself only readable if structSizeIn covers it;
        // and the OUTPUT field (handle) is governed by structSizeOut, not by
        // structSizeIn, because that is the size the callee declares it wrote.
        const size_t sz = (pParams != nullptr) ? pParams->structSizeIn : 0;
        const bool has_out =
            pParams != nullptr &&
            field_present(sz,
                          offsetof(CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS, structSizeOut),
                          sizeof(size_t));
        const size_t out = has_out ? pParams->structSizeOut : 0;

        if (pParams != nullptr)
        {
            bool logged_any = false;

            if (has_out)
            {
                std::snprintf(l, sizeof l, "[CUDADIAG]   structSizeIn=%zu structSizeOut=%zu", sz, out);
                mgpu::diag::info(l);
                logged_any = true;
            }
            else
            {
                std::snprintf(l, sizeof l,
                              "[CUDADIAG]   structSizeIn=%zu does not cover structSizeOut - the "
                              "output size is unavailable and the returned handle will not be read",
                              sz);
                mgpu::diag::info(l);
                logged_any = true;
            }

            if (field_present(sz,
                              offsetof(CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS, type),
                              sizeof(pParams->type)))
            {
                const char *tn = (pParams->type == CUDADIAG_DESC_OBJECT_SURFACE) ? "SURFACE"
                               : (pParams->type == CUDADIAG_DESC_OBJECT_TEXTURE) ? "TEXTURE"
                               : (pParams->type == CUDADIAG_DESC_OBJECT_SAMPLER) ? "SAMPLER"
                               : "?";
                std::snprintf(l, sizeof l, "[CUDADIAG]   type=%d (%s)", (int)pParams->type, tn);
                mgpu::diag::info(l);
                logged_any = true;
            }

            if (field_present(sz,
                              offsetof(CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS, desc) +
                              offsetof(D3D12_CPU_DESCRIPTOR_HANDLE, ptr),
                              sizeof(pParams->desc.ptr)))
            {
                std::snprintf(l, sizeof l, "[CUDADIAG]   desc.ptr=0x%llX",
                              (unsigned long long)pParams->desc.ptr);
                mgpu::diag::info(l);
                logged_any = true;
            }

            if (!logged_any)
            {
                std::snprintf(l, sizeof l,
                              "[CUDADIAG]   structSizeIn=%zu does not cover any readable field", sz);
                mgpu::diag::info(l);
            }
        }

        // Device evidence: pDevice lives inside the caller's struct, so it is
        // read only when the struct covers it. pDevice is __in, so it is valid
        // on entry regardless of what the call will return.
        ID3D12Device *dev = nullptr;
        if (pParams != nullptr &&
            field_present(sz,
                          offsetof(CUDADIAG_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS, pDevice),
                          sizeof(pParams->pDevice)))
        {
            dev = pParams->pDevice;
        }
        log_device("GetCudaIndependentDescriptorObject", "before", dev);

        // THE REAL CALL, exactly once, with the original argument.
        const int status = real(pParams);

        std::snprintf(l, sizeof l,
                      "[CUDADIAG] GetCudaIndependentDescriptorObject status=%d (0x%08X)",
                      status, (unsigned)status);
        mgpu::diag::info(l);

        // The __out value, through its single guarded reader. Never written.
        read_descriptor_handle(status, pParams, has_out, out);

        return status;   // VERBATIM
    }

    // ---- Wrapper 2 -------------------------------------------------------
    // NVAPI_D3D12_CreateCuModule(pDevice, pBlob, size, phModule)
    int __cdecl diag_create_cu_module(ID3D12Device *pDevice, const void *pBlob,
                                      unsigned size, NVDX_ObjectHandle *phModule)
    {
        const pfn_create_module real = reinterpret_cast<pfn_create_module>(
            g_real_module.load(std::memory_order_acquire));   // acquire
        if (real == nullptr)
        {
            // UNREACHABLE BY CONSTRUCTION - same reasoning as wrapper 1.
            mgpu::diag::error("[CUDADIAG] CreateCuModule wrapper entered with no published real "
                              "pointer - NOT calling anything, returning NVAPI_ERROR. This line "
                              "means the publication ordering is broken.");
            return NVAPI_ERROR_UNREACHABLE;
        }

        const unsigned long long n = g_calls_module.fetch_add(1, std::memory_order_relaxed) + 1;
        const int scope = g_scope.load(std::memory_order_relaxed);
        const bool mismatched = g_mismatch_module.load(std::memory_order_relaxed);

        char l[440];
        std::snprintf(l, sizeof l,
                      "[CUDADIAG] CreateCuModule seq=%llu tid=%lu scope=%s mismatch_seen=%s "
                      "pBlob=%p size=%u phModule=%p",
                      n, (unsigned long)GetCurrentThreadId(), scope_name(scope),
                      mismatched ? "yes" : "no", (const void *)pBlob, size, (void *)phModule);
        mgpu::diag::info(l);

        // pBlob is NEVER dereferenced - only its value is printed above.
        log_device("CreateCuModule", "before", pDevice);

        // THE REAL CALL, exactly once, with the original arguments.
        const int status = real(pDevice, pBlob, size, phModule);

        std::snprintf(l, sizeof l, "[CUDADIAG] CreateCuModule status=%d (0x%08X)",
                      status, (unsigned)status);
        mgpu::diag::info(l);

        // The __out value, through its single guarded reader. Never written.
        read_module_handle(status, phModule);

        return status;   // VERBATIM
    }
}

namespace mgpu::cudadiag
{
void log_variant()
{
#ifdef MGPU_ARCH_COMPAT
    mgpu::diag::info("[CUDADIAG] variant=CUDA_DIAG_ON reachable=yes "
                     "(the ARCHTEST nvapi_QueryInterface detour is compiled in)");
    // What the reader must look for, stated up front. An absent observation is
    // NOT a result, and the wording here is deliberately narrow: this
    // diagnostic can only report what it intercepted.
    mgpu::diag::info("[CUDADIAG] expect an 'armed GetCudaIndependentDescriptorObject' line and an "
                     "'armed CreateCuModule' line once each, then one seq=1 line per call. If an "
                     "armed line is absent, this diagnostic did not intercept that interface: its "
                     "NvAPI_Status and its device argument are therefore UNOBSERVED in this run. "
                     "Possible reasons include earlier caching, or the execution path not resolving "
                     "or not reaching that interface. An absent armed line is not a result.");
#else
    // Fail loudly rather than silently. select() is only ever reached from
    // arch_test.cpp's query_detour, and that detour is installed under
    // MGPU_ARCH_COMPAT. Without it this module is inert and the run answers
    // nothing.
    mgpu::diag::warn("[CUDADIAG] variant=CUDA_DIAG_ON reachable=NO - MGPU_ARCH_COMPAT is not "
                     "defined, so no nvapi_QueryInterface detour exists and select() is never "
                     "called. This run is VOID for the CUDA diagnostic.");
#endif
}

void set_expected_adapter(const LUID &luid, unsigned vendor_id, unsigned device_id)
{
    const unsigned long long packed =
        ((unsigned long long)(unsigned)luid.HighPart << 32) |
        (unsigned long long)(unsigned)luid.LowPart;
    // DXGI VendorId/DeviceId are the same pair NVAPI reports per physical GPU.
    // Little-endian-ish packing chosen to read the same way ARCHTEST's log does:
    // device in the high half, vendor in the low half, i.e. 0x278610DE.
    const unsigned pci = ((device_id & 0xFFFFu) << 16) | (vendor_id & 0xFFFFu);

    g_expected_luid_packed.store(packed, std::memory_order_relaxed);
    g_expected_pci.store(pci, std::memory_order_relaxed);
    // Published LAST with release; the readers load it FIRST with acquire.
    g_expected_known.store(true, std::memory_order_release);

    char l[320];
    std::snprintf(l, sizeof l,
                  "[CUDADIAG] expected neural adapter pushed in: luid=%08X-%08X pci=0x%08X "
                  "(diagnostic-local atomics; the CUDA call path acquires no lock)",
                  (unsigned)luid.HighPart, (unsigned)luid.LowPart, pci);
    mgpu::diag::info(l);
}

void scope_startup_probe() { g_scope.store(1, std::memory_order_relaxed); }
void scope_real_stream()   { g_scope.store(2, std::memory_order_relaxed); }
void scope_outside()       { g_scope.store(0, std::memory_order_relaxed); }

void *select(unsigned int id, void *real)
{
    if (real == nullptr)
        return nullptr;   // nothing to wrap; the resolver's own answer stands

    const bool is_desc = (id == ID_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT);
    const bool is_mod = (id == ID_CREATE_CU_MODULE);
    if (!is_desc && !is_mod)
        return real;      // not ours - untouched

    std::atomic<void *> &slot = is_desc ? g_real_desc : g_real_module;
    std::atomic<bool> &mismatch = is_desc ? g_mismatch_desc : g_mismatch_module;
    const char *nm = is_desc ? "GetCudaIndependentDescriptorObject" : "CreateCuModule";

    // FIRST publication is a compare_exchange_strong from nullptr, and nothing
    // else in this file stores to either slot. An already-published pointer is
    // therefore never overwritten.
    //
    // Success order is release so the wrapper's acquire load is guaranteed to
    // see it; failure order is acquire so the pointer the CAS observed is the
    // one we then decide about.
    void *expected = nullptr;
    if (slot.compare_exchange_strong(expected, real,
                                     std::memory_order_release,
                                     std::memory_order_acquire))
    {
        char l[320];
        std::snprintf(l, sizeof l, "[CUDADIAG] armed %s real=%p id=0x%08X", nm, real, id);
        mgpu::diag::info(l);
    }
    else if (expected != real)
    {
        // A genuinely different pointer, and the slot was NOT touched: the
        // runtime keeps the wrapper it was already handed, which calls the
        // pointer published first. Wrapping this second one would silently
        // redirect it to a function the caller did not ask for, so it is passed
        // through UNWRAPPED instead, loudly.
        mismatch.store(true, std::memory_order_relaxed);
        char l[400];
        std::snprintf(l, sizeof l,
                      "[CUDADIAG] MISMATCH %s armed=%p now=%p - the published pointer is kept and the "
                      "new one is returned UNWRAPPED (diagnostic not applied to this call)",
                      nm, expected, real);
        mgpu::diag::warn(l);
        return real;
    }
    // else: expected == real. Another thread armed this exact pointer first; the
    // wrapper is correct for it too, so fall through and return the wrapper.

    return is_desc ? reinterpret_cast<void *>(&diag_get_cuda_independent_descriptor_object)
                   : reinterpret_cast<void *>(&diag_create_cu_module);
}
}

#else   // !MGPU_CUDA_DIAG

// Compiled out: every entry point is an inert no-op, so a build without
// MGPU_CUDA_DIAG behaves exactly as before and this module links to nothing.
namespace mgpu::cudadiag
{
void log_variant() {}
void set_expected_adapter(const LUID &, unsigned, unsigned) {}
void scope_startup_probe() {}
void scope_real_stream() {}
void scope_outside() {}
void *select(unsigned int, void *real) { return real; }
}

#endif  // MGPU_CUDA_DIAG
