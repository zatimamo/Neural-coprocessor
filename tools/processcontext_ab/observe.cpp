// ============================================================================
// PROCESSCONTEXT-AB - observation of the private NVAPI CUDA-interop calls.
//
// STRICTLY OBSERVATIONAL, on the same contract MGPU's CUDADIAG uses:
//   * the real function is called exactly once, with the original arguments;
//   * its NvApi_Status is returned verbatim;
//   * no retry, no status replacement, no output modification, no forced handle;
//   * pBlob is never dereferenced;
//   * a caller-owned __out value is read ONLY when the status is NVAPI_OK.
//
// The two ids are reached by detouring nvapi64!nvapi_QueryInterface, which is how
// the DLSSNR runtime resolves them. The resolver normally answers each id once, so
// the wrapper is published once and never overwritten; if it ever answers twice
// with different pointers the new pointer is handed back UNWRAPPED so that the
// caller sees byte-for-byte what it would have seen without this hook.
//
// NULL-argument capability probes are separated from real calls: a probe
// returning -14 (NVAPI_INVALID_POINTER) is the expected answer and must never be
// read as the failure.
// ============================================================================

#include "observe.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>

#include "MinHook.h"
#include "nv.h"

namespace pcab
{
namespace
{
    using pfn_get_desc = int(__cdecl *)(void *);
    using pfn_create_cu = int(__cdecl *)(void *, const void *, unsigned, void **);

    // NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS (nvapi.h:18791),
    // field for field. size_t is 8 bytes on this x64 target.
    struct GetDescParams
    {
        size_t                      structSizeIn;
        size_t                      structSizeOut;
        ID3D12Device               *pDevice;
        int                         type;        // SURFACE=0 TEXTURE=1 SAMPLER=2
        D3D12_CPU_DESCRIPTOR_HANDLE desc;
        unsigned long long          handle;
    };
    static_assert(sizeof(GetDescParams) == 48, "the documented 48-byte contract");

    pfn_query g_query_original = nullptr;
    std::atomic<void *> g_real_desc{nullptr};
    std::atomic<void *> g_real_cu{nullptr};

    // The ONLY return a wrapper may have besides the observed status, and it is
    // unreachable in a correctly armed run: it is taken when the real pointer was
    // never published, in which case nothing is called at all. It is a NAMED
    // constant and not a literal, so that a numeric literal appearing in a
    // wrapper return is always a rewritten status and never anything else.
    const int NVAPI_ERROR_UNREACHABLE = -1;
    std::atomic<bool> g_mismatch{false};
    std::atomic<unsigned> g_query_calls{0};
    std::atomic<unsigned> g_desc_calls{0};
    std::atomic<unsigned> g_cu_calls{0};
    std::atomic<unsigned> g_desc_probes{0};
    std::atomic<unsigned> g_cu_probes{0};
    std::atomic<unsigned> g_desc_ok{0};
    std::atomic<unsigned> g_cu_ok{0};

    // ---- REAL and PROBE status storage, deliberately separate -------------
    //
    // A NULL-argument capability probe and a genuine call are DIFFERENT EVENTS
    // and their statuses are kept in different variables. Nothing can move a
    // probe's status into a real result, because there is no code path that
    // writes one into the other.
    //
    // The previous revision kept a single "last status" per function and a
    // probe count, then reported it as the real status whenever real calls
    // outnumbered probes. A real call followed by a probe therefore reported
    // the probe's -14 AS the real status. That is exactly the class of error
    // this separation removes.
    //
    // Within each kind the FIRST observation wins, by compare-and-swap out of a
    // sentinel, so "the first real status" and "the first real blob size" mean
    // what they say even if the runtime calls again.
    const int STATUS_UNSET = 0x7FFFFFFF;   // no NvApi_Status has this value

    std::atomic<int> g_desc_real_status{STATUS_UNSET};
    std::atomic<int> g_desc_probe_status{STATUS_UNSET};
    std::atomic<int> g_cu_real_status{STATUS_UNSET};
    std::atomic<int> g_cu_probe_status{STATUS_UNSET};
    std::atomic<bool> g_resolver_null{false};

    void record_real(std::atomic<int> &slot, int status)
    {
        int expected = STATUS_UNSET;
        slot.compare_exchange_strong(expected, status);   // first real call wins
    }

    void record_probe(std::atomic<int> &slot, int status)
    {
        int expected = STATUS_UNSET;
        slot.compare_exchange_strong(expected, status);   // first probe wins
    }

    int read_status(const std::atomic<int> &slot)
    {
        const int v = slot.load(std::memory_order_acquire);
        return (v == STATUS_UNSET) ? -12345 : v;
    }

    bool observed(const std::atomic<int> &slot)
    {
        return slot.load(std::memory_order_acquire) != STATUS_UNSET;
    }

    bool covered(size_t declared, size_t off, size_t len) { return (off + len) <= declared; }

    std::atomic<void *> g_expected_device{nullptr};
    std::atomic<bool> g_expected_known{false};

    // Three states, never two. With no published device no comparison has
    // happened, and claiming match=no there would assert a result this run never
    // had - the same trap CUDADIAG's gate forbids.
    const char *device_match(void *device)
    {
        if (!g_expected_known.load(std::memory_order_acquire)) return "match=NOT_COMPARED";
        return (device == g_expected_device.load(std::memory_order_acquire)) ? "match=yes"
                                                                            : "match=no";
    }

    // Always zero-padded. The observer work this diagnostic descends from had a
    // real bug where an id was compared against a zero-padded variant of itself,
    // so the comparison silently never matched; every id printed here goes
    // through this one function and is never written as a bare literal.
    void id_hex(unsigned id, char out[11])
    {
        static const char *d = "0123456789ABCDEF";
        out[0] = '0'; out[1] = 'x';
        for (int i = 0; i < 8; ++i) out[2 + i] = d[(id >> ((7 - i) * 4)) & 0xF];
        out[10] = '\0';
    }

    // "module+0xoffset" for an address that lives in a loaded image.
    const char *where(void *addr, char *buf, size_t n)
    {
        if (addr == nullptr) { std::snprintf(buf, n, "<null>"); return buf; }
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)addr, &mod) && mod != nullptr)
        {
            char path[MAX_PATH] = {0};
            GetModuleFileNameA(mod, path, MAX_PATH);
            const char *base = std::strrchr(path, '\\');
            base = base ? base + 1 : path;
            std::snprintf(buf, n, "%s+0x%llX", base,
                          (unsigned long long)((const char *)addr - (const char *)mod));
        }
        else
        {
            std::snprintf(buf, n, "<unmapped>+0x%p", addr);
        }
        return buf;
    }

    // The stack above nvapi_QueryInterface, which is where the interop call is
    // actually made from.
    void log_stack(unsigned depth)
    {
        void *frames[8] = {0};
        const USHORT got = CaptureStackBackTrace(1, 8, frames, nullptr);
        for (USHORT i = 0; i < got && i < depth; ++i)
        {
            char b[192];
            pcab::logf("[cuda]     stack[%u] %p %s", (unsigned)i, frames[i],
                       where(frames[i], b, sizeof b));
        }
    }

    int __cdecl wrap_get_desc(void *pParams)
    {
        GetDescParams *p = (GetDescParams *)pParams;
        const bool probe = (p == nullptr);
        const unsigned n = g_desc_calls.fetch_add(1, std::memory_order_relaxed) + 1;

        size_t sz = 0, out_before = 0;
        void *dev = nullptr;
        int type = -1;
        unsigned long long desc_ptr = 0;
        if (p != nullptr)
        {
            sz = p->structSizeIn;                                                 // offset 0
            if (covered(sz, 8, 8))  out_before = p->structSizeOut;                 // offset 8
            if (covered(sz, 16, 8)) dev = (void *)p->pDevice;                      // offset 16
            if (covered(sz, 24, 4)) type = p->type;                                // offset 24
            if (covered(sz, 32, 8)) desc_ptr = (unsigned long long)p->desc.ptr;    // offset 32
        }

        pcab::logf("[cuda] #%u GetCudaIndependentDescriptorObject 0x0DDAC234 %s pDevice=%p "
                   "type=%d desc.ptr=0x%llX structSizeIn=%llu structSizeOut_before=%llu %s",
                   n, probe ? "CAPABILITY-PROBE" : "REAL", dev, type, desc_ptr,
                   (unsigned long long)sz, (unsigned long long)out_before,
                   probe ? "match=NA-probe" : device_match(dev));

        pfn_get_desc real = (pfn_get_desc)g_real_desc.load(std::memory_order_acquire);
        if (real == nullptr)
        {
            pcab::logf("[cuda] #%u the real pointer was never published - returning NVAPI_ERROR "
                       "WITHOUT calling anything", n);
            return NVAPI_ERROR_UNREACHABLE;
        }

        const int status = real(pParams);          // ONCE, original argument
        // A real call can ONLY ever reach the real slot.
        if (probe) record_probe(g_desc_probe_status, status);
        else       record_real(g_desc_real_status, status);

        size_t out_after = 0;
        bool have_out = false;
        if (p != nullptr && covered(sz, 8, 8)) { out_after = p->structSizeOut; have_out = true; }

        unsigned long long handle = 0;
        const char *handle_note = nullptr;
        if (status != 0)               handle_note = "NOT READ: NvApi_Status != NVAPI_OK";
        else if (p == nullptr)         handle_note = "NOT READ: pParams is null";
        else if (!have_out)            handle_note = "NOT READ: the callee's declared input size "
                                                     "does not cover structSizeOut, so the declared "
                                                     "output size is unreadable";
        else if (!covered(out_after, 40, 8))
                                       handle_note = "NOT READ: structSizeOut does not cover it";
        else                           handle = (unsigned long long)p->handle;   // offset 40

        if (probe)
        {
            g_desc_probes.fetch_add(1, std::memory_order_relaxed);
            pcab::logf("[cuda] #%u probe status=%d (0x%08X)%s - null-argument validation probe; "
                       "NOT a failure", n, status, (unsigned)status,
                       status == -14 ? " NVAPI_INVALID_POINTER" : "");
        }
        else
        {
            if (status == 0) g_desc_ok.fetch_add(1, std::memory_order_relaxed);
            pcab::logf("[cuda] #%u REAL status=%d (0x%08X) structSizeOut_after=%llu "
                       "returned_handle=0x%llX %s", n, status, (unsigned)status,
                       (unsigned long long)out_after, handle, handle_note ? handle_note : "");
        }
        return status;                              // VERBATIM
    }

    int __cdecl wrap_create_cu(void *pDevice, const void *pBlob, unsigned size, void **phModule)
    {
        const bool probe = (pDevice == nullptr && pBlob == nullptr && size == 0 &&
                            phModule == nullptr);
        const unsigned n = g_cu_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!probe)
        {
            unsigned long long expected = 0;
            g_first_blob_size.compare_exchange_strong(expected, size);
        }

        pcab::logf("[cuda] #%u CreateCuModule 0xAD1A677D %s pDevice=%p pBlob=%p (pointer only; "
                   "never dereferenced) size=%u phModule=%p %s",
                   n, probe ? "CAPABILITY-PROBE" : "REAL", pDevice, pBlob, size, phModule,
                   probe ? "match=NA-probe" : device_match(pDevice));

        pfn_create_cu real = (pfn_create_cu)g_real_cu.load(std::memory_order_acquire);
        if (real == nullptr)
        {
            pcab::logf("[cuda] #%u the real pointer was never published - returning NVAPI_ERROR "
                       "WITHOUT calling anything", n);
            return NVAPI_ERROR_UNREACHABLE;
        }

        const int status = real(pDevice, pBlob, size, phModule);
        // A real call can ONLY ever reach the real slot.
        if (probe) record_probe(g_cu_probe_status, status);
        else       record_real(g_cu_real_status, status);

        unsigned long long mod = 0;
        const char *note = nullptr;
        if (status != 0)              note = "NOT READ: NvApi_Status != NVAPI_OK";
        else if (phModule == nullptr) note = "NOT READ: phModule is null";
        else                          mod = (unsigned long long)*phModule;

        if (probe)
        {
            g_cu_probes.fetch_add(1, std::memory_order_relaxed);
            pcab::logf("[cuda] #%u probe status=%d (0x%08X)%s - null-argument validation probe; "
                       "NOT a failure", n, status, (unsigned)status,
                       status == -14 ? " NVAPI_INVALID_POINTER" : "");
        }
        else
        {
            if (status == 0) g_cu_ok.fetch_add(1, std::memory_order_relaxed);
            pcab::logf("[cuda] #%u REAL status=%d (0x%08X) module_handle=0x%llX %s",
                       n, status, (unsigned)status, mod, note ? note : "");
        }
        return status;                              // VERBATIM
    }

    void *__cdecl query_detour(unsigned int id)
    {
        if (g_query_original == nullptr) return nullptr;
        void *answer = g_query_original(id);        // ORIGINAL first, unmodified
        const unsigned n = g_query_calls.fetch_add(1, std::memory_order_relaxed) + 1;

        const unsigned interesting = (id == NVAPI_ID_INITIALIZE ||
                                      id == NVAPI_ID_GET_ARCH_INFO ||
                                      id == NVAPI_ID_GET_CUDA_DESCRIPTOR ||
                                      id == NVAPI_ID_CREATE_CU_MODULE);

        // Every id is logged; the four that decide this diagnostic are tagged.
        char tag[32] = "";
        if (id == NVAPI_ID_INITIALIZE)          std::snprintf(tag, sizeof tag, " [NvAPI_Initialize]");
        else if (id == NVAPI_ID_GET_ARCH_INFO)  std::snprintf(tag, sizeof tag, " [GetArchInfo]");
        else if (id == NVAPI_ID_GET_CUDA_DESCRIPTOR)
                                                std::snprintf(tag, sizeof tag, " [GetCudaDesc]");
        else if (id == NVAPI_ID_CREATE_CU_MODULE)
                                                std::snprintf(tag, sizeof tag, " [CreateCuModule]");

        char b1[192];
        char b2[192];
        char ids[11];
        id_hex(id, ids);
        pcab::logf("[cuda] query #%u tid=%lu id=%s -> %p %s caller=%p %s%s",
                   n, (unsigned long)GetCurrentThreadId(), ids, answer,
                   where(answer, b1, sizeof b1), _ReturnAddress(),
                   where(_ReturnAddress(), b2, sizeof b2), tag);

        if (interesting) log_stack(6);

        if (id == NVAPI_ID_GET_CUDA_DESCRIPTOR)
        {
            if (answer == nullptr)
            {
                // OBSERVATIONAL MEANS OBSERVATIONAL. The genuine resolver
                // answered "this interface is not here", and that answer is
                // returned unchanged. Manufacturing a wrapper around a null
                // pointer would invent a call that does not exist, turn a
                // capability answer into a failure, and make the log describe
                // an invocation that never happened.
                g_resolver_null.store(true);
                pcab::logf("[cuda] nvapi_QueryInterface(0x0DDAC234) returned nullptr - "
                           "returning nullptr UNCHANGED. No wrapper is manufactured for "
                           "an interface that is not present.");
                return nullptr;
            }
            void *expected = nullptr;
            if (g_real_desc.compare_exchange_strong(expected, answer,
                                                    std::memory_order_release,
                                                    std::memory_order_acquire))
            {
                pcab::logf("[cuda] ARMED GetCudaIndependentDescriptorObject real=%p", answer);
            }
            else if (expected != answer)
            {
                g_mismatch.store(true);
                pcab::logf("[cuda] MISMATCH descriptor armed=%p now=%p - the already-published "
                           "pointer is kept for its holders and this new pointer is returned "
                           "UNWRAPPED, so behaviour is unchanged", expected, answer);
                return answer;
            }
            return (void *)&wrap_get_desc;
        }

        if (id == NVAPI_ID_CREATE_CU_MODULE)
        {
            if (answer == nullptr)
            {
                g_resolver_null.store(true);
                pcab::logf("[cuda] nvapi_QueryInterface(0xAD1A677D) returned nullptr - "
                           "returning nullptr UNCHANGED. No wrapper is manufactured for "
                           "an interface that is not present.");
                return nullptr;
            }
            void *expected = nullptr;
            if (g_real_cu.compare_exchange_strong(expected, answer,
                                                  std::memory_order_release,
                                                  std::memory_order_acquire))
            {
                pcab::logf("[cuda] ARMED CreateCuModule real=%p", answer);
            }
            else if (expected != answer)
            {
                g_mismatch.store(true);
                pcab::logf("[cuda] MISMATCH cumodule armed=%p now=%p - the already-published "
                           "pointer is kept for its holders and this new pointer is returned "
                           "UNWRAPPED, so behaviour is unchanged", expected, answer);
                return answer;
            }
            return (void *)&wrap_create_cu;
        }

        return answer;
    }
}

bool observe_install(const char *mode)
{
    (void)mode;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        logf("[cuda] MH_Initialize -> %d - the two private calls will NOT be observed", (int)st);
        return false;
    }

    // nvapi64.dll is loaded here if it is not already resident. The observer is
    // installed BEFORE NvAPI_Initialize so that the very first query - the one
    // that resolves 0x0150E828 - is observed too, and MinHook patches the
    // function ENTRY, so the caller's own cached GetProcAddress pointer is
    // intercepted as well.
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi == nullptr) nvapi = LoadLibraryW(L"nvapi64.dll");
    if (nvapi == nullptr)
    {
        logf("[cuda] nvapi64.dll not resident and could not be loaded (GetLastError=%lu) - "
             "cannot observe", (unsigned long)GetLastError());
        return false;
    }
    void *entry = (void *)GetProcAddress(nvapi, "nvapi_QueryInterface");
    if (entry == nullptr)
    {
        logf("[cuda] nvapi64.dll has no nvapi_QueryInterface export");
        return false;
    }

    void *original = nullptr;
    st = MH_CreateHook(entry, (LPVOID)&query_detour, &original);
    if (st != MH_OK || original == nullptr)
    {
        logf("[cuda] MH_CreateHook(nvapi_QueryInterface) -> %d (original=%p)", (int)st, original);
        return false;
    }
    g_query_original = (pfn_query)original;     // the TRAMPOLINE, not the patched entry

    st = MH_EnableHook(entry);
    if (st != MH_OK)
    {
        logf("[cuda] MH_EnableHook(nvapi_QueryInterface) -> %d", (int)st);
        MH_RemoveHook(entry);
        g_query_original = nullptr;
        return false;
    }
    logf("[cuda] nvapi_QueryInterface detoured (entry=%p test=%p) - observing 0x0DDAC234 and "
         "0xAD1A677D, pass-through only", entry, (void *)&query_detour);
    return true;
}

void observe_remove()
{
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi != nullptr)
    {
        void *entry = (void *)GetProcAddress(nvapi, "nvapi_QueryInterface");
        if (entry != nullptr) MH_DisableHook(entry);
    }
}

void observe_set_expected_device(void *device)
{
    g_expected_device.store(device, std::memory_order_release);
    g_expected_known.store(true, std::memory_order_release);   // flag published AFTER its payload
    logf("[cuda] expected device for the private-call observation = %p - every call from here is "
         "reported as having reached it or not", device);
}

ObserveSummary observe_summary()
{
    ObserveSummary s{};
    s.descriptor_calls = g_desc_calls.load();
    s.cumodule_calls = g_cu_calls.load();
    s.descriptor_probes = g_desc_probes.load();
    s.cumodule_probes = g_cu_probes.load();
    s.mismatch = g_mismatch.load();
    s.first_blob_size = g_first_blob_size.load();
    s.resolver_returned_null = g_resolver_null.load();

    // REAL results come from the real slots and PROBE results from the probe
    // slots. There is no arithmetic that could move one into the other, which
    // is the property the previous revision got wrong.
    s.real_descriptor_status = read_status(g_desc_real_status);
    s.real_cumodule_status = read_status(g_cu_real_status);
    s.descriptor_probe_status = read_status(g_desc_probe_status);
    s.cumodule_probe_status = read_status(g_cu_probe_status);
    s.probe_status = (observed(g_desc_probe_status)) ? read_status(g_desc_probe_status)
                   : (observed(g_cu_probe_status))   ? read_status(g_cu_probe_status)
                                                     : -12345;
    return s;
}
}
