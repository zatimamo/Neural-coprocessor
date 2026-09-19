// ============================================================================
// PROCESSCONTEXT-AB
//
// ONE question, fixed in advance:
//
//   Does the D3D12 device / queue / descriptor state that Cyberpunk carries when
//   the neural lane runs explain why the private NVAPI CUDA-interop calls
//   (0x0DDAC234 GetCudaIndependentDescriptorObject, 0xAD1A677D CreateCuModule)
//   return NVAPI_ERROR there but succeed in NeuralScreen?
//
// The method is a four-arm single-variable A/B. Every arm runs the SAME
// NeuralScreen-equivalent CONTROL lane, in its own process, on the RTX 4070, and
// differs ONLY in second-adapter state established before that lane starts:
//
//   single          nothing else at all                       (the reference)
//   dual-held       RTX 4070 Ti SUPER device, created + held, no NGX, no swapchain
//   dual-active     ... plus a DIRECT command queue and a tiny CBV/SRV/UAV heap
//   dual-released   Ti SUPER device created, recorded, then RELEASED
//
// THE CONTROL LANE IS NeuralScreen v1.15.0's SEQUENCE, and the order is part of
// the reproduction rather than an implementation detail:
//
//   1. create the RTX 4070 device
//   2. load the NGX core
//   3. core Init                  app id 0x1000000
//   4. AllocateParameters         a private block, not the core's shared one
//   5. NvAPI_Initialize           ^ after core Init, which is NeuralScreen's order
//   6. cache every GPU's real architecture, then install the patch over one target
//   7. install the observational QueryInterface observer
//   8. load nvngx_dlssnr.dll
//   9. snippet Init_Ext           app id 0x1000000
//  10. params->Reset(), the exact 640x360 contract, CreateFeature Reserved18
//
// The data path is dirname(--nr-dll) - the directory the runtime actually sits
// in, which is the directory NeuralScreen hands NGX. The NR DLL path and the
// derived data path are both logged, and the DLL must be inside the data path.
//
// THE FIFTH MODE, AND IT IS NOT AN ARM
//
//   --mode holder-ti    Keeps ACTIVE RTX 4070 Ti SUPER D3D12 state alive in a
//                       process of its own, and nothing else: a device, a DIRECT
//                       queue and a small CBV/SRV/UAV descriptor heap.
//
//   It exists for ONE question, which the four arms raise and cannot answer:
//   the dual-active arm shows that active second-adapter state in the SAME
//   process breaks the RTX 4070 lane. Does it still break it when the state is
//   in a DIFFERENT process?
//
//   So the holder is PROCESS A, it prints HOLDER_READY once its state is
//   established, and it then waits for a stop signal. PROCESS B is the existing
//   SINGLE arm, launched completely separately and completely unchanged. If B
//   succeeds, process isolation is validated as the fix direction.
//
//   It never loads the NGX core, never touches NVAPI, never installs the
//   architecture patch, never creates the RTX 4070 device, never creates a
//   swapchain and never submits anything. It RETURNS before the lane begins, so
//   there is no code path from the holder into the lane.
//
// INTERPRETATION, FIXED IN ADVANCE - one rule per arm, in this order:
//
//   E) SINGLE does not succeed  -> THE DIAGNOSTIC IS INVALID. Stop. Interpret
//      nothing. Every other arm's result is unreadable, because the reference
//      lane itself did not reproduce, so no difference can be attributed to the
//      variable.
//   A) SINGLE ok + DUAL-HELD fail -> the mere SIMULTANEOUS presence of a second
//      adapter is sufficient to break the calls.
//   B) A and DUAL-ACTIVE fails    -> an ACTIVE second command queue / descriptor
//      heap is what triggers it, not the device alone.
//   C) DUAL-RELEASED fails        -> process-global NVIDIA state persists after
//      the second device is released, so release is not a cure.
//   D) all four succeed           -> the multi-device hypothesis is EXONERATED.
//
// WHAT THIS PROGRAM DOES NOT DO
//   It does not modify MGPU, NeuralScreen, Cyberpunk, or any NVIDIA binary. It
//   does not patch a result, fabricate a handle, retry a call, or replace an
//   NvApi_Status. It loads no ReShade and no Streamline. It never selects an
//   adapter by ordinal: both adapters are identified by PCI device id.
//
// WHY THE EXECUTABLE MUST BE NAMED *nvngx.dll*
//   The DLSSNR snippet inspects the module name of its caller and refuses a
//   caller whose file name does not contain "nvngx.dll" with 0xBAD00002
//   FAIL_PlatformError. MGPU answers this by deploying as
//   "nvngx.dll_mgpu_bridge.addon64"; this diagnostic answers it the same way,
//   and RUN-CONTEXT-AB.cmd runs the exe under a name that carries the prefix.
//   The name is checked and reported at startup, because a run that skips this
//   would fail SINGLE for a reason that has nothing to do with the hypothesis.
// ============================================================================

#include "nv.h"
#include "observe.hpp"

#include <dxgi1_6.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

namespace
{
    // ------------------------------------------------------------- constants

    // 10DE:2705 RTX 4070 Ti SUPER - the second adapter, held by Cyberpunk's process.
    const unsigned PCI_TI_SUPER  = 0x2705u;
    // 10DE:2786 RTX 4070 - the adapter NeuralScreen's lane works on.
    const unsigned PCI_RTX_4070  = 0x2786u;
    const unsigned VENDOR_NVIDIA = 0x10DEu;

    // The fifth mode. NOT one of the four arms: it never runs the NR lane, so it
    // is not in MODE_NAMES, it is not read by --mode table, and it is not part of
    // the experiment's result rules.
    const char *HOLDER_MODE = "holder-ti";
    const unsigned HOLDER_DEFAULT_SECONDS = 1800u;

    // The exact snippet build NeuralScreen v1.15.0 runs. A different build has a
    // different runtime and the comparison would be against the wrong thing, so
    // the hash is a gate, not a note.
    const char *EXPECTED_NR_SHA256 =
        "dcc0dc2414aedec4a8e084647070383be068554042587180c20c784d4772d36f";

    const unsigned CTRL_W = 640u;
    const unsigned CTRL_H = 360u;

    const char *MODE_NAMES[4] = { "single", "dual-held", "dual-active", "dual-released" };

    // ---------------------------------------------------------------- helpers

    bool ieq_ascii(const char *a, const char *b)
    {
        for (; *a && *b; ++a, ++b)
        {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return *a == '\0' && *b == '\0';
    }

    void hex32(unsigned v, char out[11])
    {
        static const char *d = "0123456789ABCDEF";
        out[0] = '0'; out[1] = 'x';
        for (int i = 0; i < 8; ++i) out[2 + i] = d[(v >> ((7 - i) * 4)) & 0xF];
        out[10] = '\0';
    }

    // A signed status as plain decimal. Round-robin over four slots so that two
    // calls in one argument list cannot share a buffer and clobber each other -
    // the same class of bug as reusing one `char b[]` for two %s arguments.
    const char *dec(int v)
    {
        static char bufs[4][16];
        static unsigned slot = 0;
        char *b = bufs[slot++ & 3u];
        std::snprintf(b, 16, "%d", v);
        return b;
    }

    // "NVAPI_ERROR", from nvapi.h NvApi_Status. Only the two values this
    // diagnostic can be judged on are named; everything else is reported
    // numerically rather than guessed at.
    const char *nv_status_name(int s)
    {
        switch (s)
        {
        case  0: return "NVAPI_OK";
        case -1: return "NVAPI_ERROR";
        case -5: return "NVAPI_INVALID_ARGUMENT";
        case -6: return "NVAPI_NVIDIA_DEVICE_NOT_FOUND";
        case -9: return "NVAPI_INCOMPATIBLE_STRUCT_VERSION";
        case -14: return "NVAPI_INVALID_POINTER";
        default: return "see nvapi.h NvApi_Status";
        }
    }

    const char *ngx_result_name(unsigned r)
    {
        switch (r)
        {
        case 0x00000001u: return "NVSDK_NGX_Result_Success";
        case 0xBAD00001u: return "FAIL_InvalidParameter";
        case 0xBAD00002u: return "FAIL_PlatformError";
        case 0xBAD00003u: return "FAIL_NotInitialized";
        case 0xBAD00004u: return "FAIL_NotSupported";
        case 0xBAD00005u: return "FAIL_InvalidState";
        case 0xBAD0000Bu: return "FAIL_UnableToInitializeFeature";
        case 0xBAD0000Cu: return "FAIL_OutOfDate";
        case 0xBAD00012u: return "FAIL_FeatureRequirementsQueryFailed";
        default:          return "see nvsdk_ngx_defs.h";
        }
    }

    // ------------------------------------------------------- the CONTROL lane

    struct Lane
    {
        bool     reached_create = false;   // CreateFeature was actually invoked
        unsigned core_init = 0xDEADBEEFu;
        unsigned alloc     = 0xDEADBEEFu;
        unsigned snip_init = 0xDEADBEEFu;
        unsigned feature   = 0xDEADBEEFu;
        void    *handle    = nullptr;
        bool     drained   = false;

        ID3D12Device              *dev    = nullptr;
        ID3D12CommandQueue        *queue  = nullptr;
        ID3D12CommandAllocator    *allocator = nullptr;
        ID3D12GraphicsCommandList *list   = nullptr;
        ID3D12Fence               *fence  = nullptr;
        HANDLE                     event  = nullptr;
        NVSDK_NGX_Parameter       *params = nullptr;
    };

    // NeuralScreen's logging configuration: a callback that discards, because
    // "Discard" is the whole specification. Nothing is printed and nothing is
    // stored; its only job is to be a NON-NULL callback so that
    // DisableOtherLoggingSinks = true is honoured as the header requires.
    void NVSDK_CONV ngx_log_discard(const char *message, NVSDK_NGX_Logging_Level level,
                                    NVSDK_NGX_Feature source)
    {
        (void)message;
        (void)level;
        (void)source;
    }

    // THE EXACT NeuralScreen v1.15.0 640x360 Reserved18 CREATION CONTRACT.
    //
    // The key names are the ones MGPU's expanded contract already uses, which
    // are the names the snippet actually reads. The generic Width/Height pair is
    // deliberately NOT set: NeuralScreen does not set it, and this diagnostic
    // exists to REPRODUCE NeuralScreen, not to improve on it with extra keys
    // that would make the two paths differ in the very place being compared.
    //
    // Every key is printed as it is set, so the log carries the contract rather
    // than a claim that it was applied.
    void apply_reference_contract(NVSDK_NGX_Parameter *params)
    {
        if (params == nullptr)
        {
            pcab::logf("[contract] params is null - nothing to apply");
            return;
        }
        params->Reset();                     // NeuralScreen resets before filling
        pcab::logf("[contract] params->Reset() done; applying the NeuralScreen 640x360 contract");

        params->Set(NVSDK_NGX_Parameter_CreationNodeMask,   1u);
        params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        pcab::logf("[contract] CreationNodeMask=1 VisibilityNodeMask=1");

        params->Set("DLSSNR.Width",  (unsigned)CTRL_W);
        params->Set("DLSSNR.Height", (unsigned)CTRL_H);
        params->Set("DLSSNR.InputWidth",  (unsigned)CTRL_W);
        params->Set("DLSSNR.InputHeight", (unsigned)CTRL_H);
        params->Set("DLSSNR.OutputWidth",  (unsigned)CTRL_W);
        params->Set("DLSSNR.OutputHeight", (unsigned)CTRL_H);
        params->Set("DLSSNR.Output.Width",  (unsigned)CTRL_W);
        params->Set("DLSSNR.Output.Height", (unsigned)CTRL_H);
        pcab::logf("[contract] DLSSNR.Width/Height=%u/%u Input=%u/%u Output=%u/%u "
             "DLSSNR.Output.Width/Height=%u/%u",
             CTRL_W, CTRL_H, CTRL_W, CTRL_H, CTRL_W, CTRL_H, CTRL_W, CTRL_H);

        params->Set("DLSSNR.Upscaling", 0u);
        params->Set("DLSSNR.Scale", 1.0f);
        params->Set("DLSSNR.ScalingRatio", 1.0f);
        pcab::logf("[contract] DLSSNR.Upscaling=0 DLSSNR.Scale=1.0 DLSSNR.ScalingRatio=1.0");

        params->Set("DLSSNR.Hint.Render.Preset", 0u);
        params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0);
        pcab::logf("[contract] DLSSNR.Hint.Render.Preset=0 DLSS.Feature.Create.Flags=0");

        pcab::logf("[contract] NO generic Width/Height is set - NeuralScreen does not set them, and "
             "adding them here would make this lane differ from the reference it reproduces");
    }

    FARPROC pick(HMODULE m, const char *name, const char *who)
    {
        FARPROC p = (m != nullptr) ? GetProcAddress(m, name) : nullptr;
        pcab::logf("[ngx] %-40s (%-7s) -> %p", name, who, (void *)p);
        return p;
    }

    // ------------------------------------------------- the holder-ti process
    //
    // holder-ti EXISTS ONLY TO KEEP ACTIVE Ti SUPER D3D12 STATE ALIVE in a
    // process of its own. It is PROCESS A of the split-process experiment.
    //
    // It never loads the NGX core, never touches NVAPI, never creates the RTX
    // 4070 device, never creates a swapchain and never submits anything. What it
    // holds is exactly the state that PROCESSCONTEXT's dual-active arm showed to
    // be sufficient to break an RTX 4070 NR lane in the SAME process: a device, a
    // DIRECT queue and a small CBV/SRV/UAV descriptor heap.
    //
    // If an RTX 4070 NR lane succeeds in a DIFFERENT process while this is alive,
    // process isolation is the fix direction.

    // True while the parent still has stdin open. A byte written by the parent is
    // an EXPLICIT STOP COMMAND, and a broken pipe means the parent is gone -
    // which is what keeps a holder from outliving its orchestrator.
    bool holder_stdin_open()
    {
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == nullptr || h == INVALID_HANDLE_VALUE) return true;
        DWORD avail = 0;
        if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
        {
            if (avail == 0) return true;
            char buf[64];
            DWORD got = 0;
            if (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
            {
                pcab::logf("[holder] an explicit stop command arrived on stdin (%lu byte(s))",
                           (unsigned long)got);
                return false;
            }
            return true;
        }
        if (GetLastError() == ERROR_BROKEN_PIPE)
        {
            pcab::logf("[holder] stdin is closed - the parent is gone");
            return false;
        }
        // Not a pipe at all. The named event is then the only control, which is
        // the documented first choice anyway.
        return true;
    }

    int run_holder_ti(IDXGIAdapter1 *adapter, const DXGI_ADAPTER_DESC1 &desc,
                      const wchar_t *event_name, unsigned cap_seconds)
    {
        pcab::logf("[holder] PROCESS A - active Ti SUPER D3D12 state, and nothing else");
        pcab::logf("[holder] adapter: desc=\"%ls\" vendor=0x%04X device=0x%04X "
                   "luid=%08lX:%08lX",
                   desc.Description, desc.VendorId, desc.DeviceId,
                   (unsigned long)desc.AdapterLuid.HighPart,
                   (unsigned long)desc.AdapterLuid.LowPart);
        pcab::logf("[holder] NO NGX, NO NVAPI, NO arch patch, NO RTX 4070 device, NO "
                   "swapchain, NO submissions");

        ID3D12Device *dev = nullptr;
        HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        pcab::logf("[holder] D3D12CreateDevice(FL_11_0) hr=0x%08X device=%p",
                   (unsigned)hr, (void *)dev);
        if (FAILED(hr) || dev == nullptr) { pcab::logf("HOLDER_FAILED"); return 1; }

        ID3D12CommandQueue *queue = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        pcab::logf("[holder] CreateCommandQueue(DIRECT) hr=0x%08X queue=%p",
                   (unsigned)hr, (void *)queue);
        if (FAILED(hr) || queue == nullptr) { pcab::logf("HOLDER_FAILED"); return 1; }

        ID3D12DescriptorHeap *heap = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 8;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        pcab::logf("[holder] CreateDescriptorHeap(CBV_SRV_UAV x8, SHADER_VISIBLE) "
                   "hr=0x%08X heap=%p", (unsigned)hr, (void *)heap);
        if (FAILED(hr) || heap == nullptr) { pcab::logf("HOLDER_FAILED"); return 1; }

        // The named stop event. The PARENT owns it; this process only opens it,
        // so there is exactly one owner and no way for a stale event to be
        // inherited from an earlier run.
        HANDLE stop = nullptr;
        if (event_name != nullptr && event_name[0] != L'\0')
        {
            stop = OpenEventW(SYNCHRONIZE, FALSE, event_name);
            pcab::logf("[holder] stop event \"%ls\" -> %p%s", event_name, (void *)stop,
                       (stop != nullptr) ? "" : "  (could not be opened; stdin is the control)");
        }
        else
        {
            pcab::logf("[holder] no stop event was named; stdin is the control");
        }

        pcab::logf("[holder] the state is established and will be HELD. Waiting for the stop "
                   "signal: the named event, an explicit byte on stdin, or stdin closing - "
                   "which is what happens if the parent dies. Cap %u seconds.",
                   cap_seconds);
        pcab::logf("HOLDER_READY");

        const DWORD started = GetTickCount();
        const char *why = "the cap expired";
        for (;;)
        {
            if (stop != nullptr && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
            {
                why = "the named stop event was signalled";
                break;
            }
            if (!holder_stdin_open())
            {
                why = "stdin closed, or an explicit stop command";
                break;
            }
            if (cap_seconds != 0 && (GetTickCount() - started) / 1000u >= cap_seconds)
            {
                why = "the cap expired";
                break;
            }
            Sleep(100);
        }

        pcab::logf("[holder] stopping: %s", why);
        if (stop != nullptr) CloseHandle(stop);
        pcab::logf("[holder] held state is released by process exit; nothing was submitted");
        pcab::logf("HOLDER_STOPPED");
        return 0;
    }

    // ------------------------------------------------------------ the table

    struct Row
    {
        bool present = false;
        bool valid = false;
        char reason[64] = "";
        bool control_ok = false;
        unsigned core_init = 0, alloc = 0, snip_init = 0, feature = 0;
        int desc_status = -12345;
        unsigned desc_calls = 0, desc_probes = 0;
        int cu_status = -12345;
        unsigned cu_calls = 0, cu_probes = 0;
    };

    // Parse "PCAB-RESULT key=value key=value ..." into a Row.
    bool parse_result_line(const char *line, Row &row, const char *want_mode)
    {
        const char *p = line;
        if (std::strncmp(p, "PCAB-RESULT ", 12) != 0) return false;
        p += 12;

        char mode[64] = "";
        bool mode_ok = false;
        row = Row{};

        while (*p != '\0')
        {
            while (*p == ' ') ++p;
            if (*p == '\0') break;
            const char *eq = std::strchr(p, '=');
            if (eq == nullptr) break;
            const char *val = eq + 1;
            const char *end = std::strchr(val, ' ');
            const size_t vlen = (end != nullptr) ? (size_t)(end - val) : std::strlen(val);
            const size_t klen = (size_t)(eq - p);

            char key[48] = "";
            char v[96] = "";
            if (klen < sizeof key) { std::memcpy(key, p, klen); key[klen] = '\0'; }
            if (vlen < sizeof v)   { std::memcpy(v, val, vlen); v[vlen] = '\0'; }

            if (std::strcmp(key, "mode") == 0)
            {
                std::snprintf(mode, sizeof mode, "%s", v);
                mode_ok = (std::strcmp(v, want_mode) == 0);
            }
            else if (std::strcmp(key, "valid") == 0)        row.valid = (std::strcmp(v, "YES") == 0);
            else if (std::strcmp(key, "reason") == 0)       std::snprintf(row.reason, sizeof row.reason, "%s", v);
            else if (std::strcmp(key, "control") == 0)      row.control_ok = (std::strcmp(v, "OK") == 0);
            else if (std::strcmp(key, "core_init") == 0)    row.core_init = (unsigned)std::strtoul(v, nullptr, 16);
            else if (std::strcmp(key, "alloc") == 0)        row.alloc = (unsigned)std::strtoul(v, nullptr, 16);
            // `caps` was the field name before the block came from
            // AllocateParameters. Still accepted so an older log still reads.
            else if (std::strcmp(key, "caps") == 0)         row.alloc = (unsigned)std::strtoul(v, nullptr, 16);
            else if (std::strcmp(key, "snip_init") == 0)    row.snip_init = (unsigned)std::strtoul(v, nullptr, 16);
            else if (std::strcmp(key, "populate") == 0)     { /* retired field */ }
            else if (std::strcmp(key, "feature") == 0)      row.feature = (unsigned)std::strtoul(v, nullptr, 16);
            else if (std::strcmp(key, "desc_status") == 0)  row.desc_status = (std::strcmp(v, "NA") == 0) ? -12345 : (int)std::strtol(v, nullptr, 0);
            else if (std::strcmp(key, "desc_calls") == 0)   row.desc_calls = (unsigned)std::strtoul(v, nullptr, 10);
            else if (std::strcmp(key, "desc_probes") == 0)  row.desc_probes = (unsigned)std::strtoul(v, nullptr, 10);
            else if (std::strcmp(key, "cu_status") == 0)    row.cu_status = (std::strcmp(v, "NA") == 0) ? -12345 : (int)std::strtol(v, nullptr, 0);
            else if (std::strcmp(key, "cu_calls") == 0)     row.cu_calls = (unsigned)std::strtoul(v, nullptr, 10);
            else if (std::strcmp(key, "cu_probes") == 0)    row.cu_probes = (unsigned)std::strtoul(v, nullptr, 10);

            if (end == nullptr) break;
            p = end + 1;
        }
        if (!mode_ok)
        {
            pcab::logf("      (ignoring a result line for mode \"%s\", wanted \"%s\")",
                       mode[0] ? mode : "?", want_mode);
            return false;
        }
        row.present = true;
        return true;
    }

    void status_cell(int status, unsigned calls, unsigned probes, char out[40])
    {
        if (status == -12345)
        {
            std::snprintf(out, 40, "%-12s %u/%up", "NOT-OBSERVED", calls, probes);
            return;
        }
        char h[11];
        hex32((unsigned)status, h);
        std::snprintf(out, 40, "%-12s %u/%up", h, calls, probes);
    }

    int run_table()
    {
        Row rows[4];
        for (int i = 0; i < 4; ++i)
        {
            char path[MAX_PATH] = "";
            std::snprintf(path, sizeof path, "context-%s.log", MODE_NAMES[i]);
            FILE *f = std::fopen(path, "r");
            if (f == nullptr)
            {
                pcab::logf("[table] %s is missing - mode \"%s\" did not run here",
                           path, MODE_NAMES[i]);
                continue;
            }
            char line[4096];
            while (std::fgets(line, sizeof line, f) != nullptr)
            {
                const size_t n = std::strlen(line);
                if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
                if (parse_result_line(line, rows[i], MODE_NAMES[i])) break;
            }
            std::fclose(f);
            if (!rows[i].present)
                pcab::logf("[table] %s carries no PCAB-RESULT line - mode \"%s\" produced no result",
                           path, MODE_NAMES[i]);
        }

        pcab::logf("");
        pcab::logf("================================================================================");
        pcab::logf("PROCESSCONTEXT-AB  -  FIXED RESULT TABLE");
        pcab::logf("================================================================================");
        pcab::logf("                             GetCudaIndependent-  CreateCu-");
        pcab::logf("MODE            VALID  LANE  DescriptorObject    Module        Res18");
        pcab::logf("--------------------------------------------------------------------------------");
        char cellA[40], cellB[40];
        for (int i = 0; i < 4; ++i)
        {
            const Row &r = rows[i];
            if (!r.present)
            {
                pcab::logf("%-15s NO     %-5s %-19s %-13s %s",
                           MODE_NAMES[i], "-", "NOT RUN", "NOT RUN", "NOT RUN");
                continue;
            }
            status_cell(r.desc_status, r.desc_calls, r.desc_probes, cellA);
            status_cell(r.cu_status, r.cu_calls, r.cu_probes, cellB);
            char fh[11];
            if (r.feature == 0xDEADBEEFu) std::snprintf(fh, sizeof fh, "NOT-RUN");
            else hex32(r.feature, fh);
            pcab::logf("%-15s %-6s %-5s %-19s %-13s %s",
                       MODE_NAMES[i],
                       r.valid ? "YES" : "NO",
                       r.valid ? (r.control_ok ? "OK" : "FAIL") : "-",
                       cellA, cellB, fh);
        }
        pcab::logf("================================================================================");
        pcab::logf("cells: <NvAPI_Status> <real-calls>/<null-arg-probes>");
        pcab::logf("LANE OK = the two private calls returned NVAPI_OK and CreateFeature returned 1");
        pcab::logf("");

        // ---- the fixed interpretation ----
        pcab::logf("VERDICT");

        if (!rows[0].present)
        {
            pcab::logf("  NO VERDICT: mode \"single\" is absent from this directory. The reference");
            pcab::logf("  arm is the only arm whose success makes any other arm readable, so");
            pcab::logf("  without it nothing here can be interpreted.");
            return 4;
        }
        if (!rows[0].valid)
        {
            pcab::logf("  E) INVALID. The SINGLE reference lane did not complete, so this run");
            pcab::logf("     run does not reproduce NeuralScreen's working path and NO arm of it");
            pcab::logf("     may be interpreted. Read context-single.log: the reference failed");
            pcab::logf("     before the variable under test was ever introduced.");
            return 2;
        }
        for (int i = 1; i < 4; ++i)
        {
            if (!rows[i].present || !rows[i].valid)
            {
                pcab::logf("  NO VERDICT: mode \"%s\" %s.", MODE_NAMES[i],
                           rows[i].present ? "did not complete" : "is absent");
                pcab::logf("  The four arms must all be valid before a difference can be "
                           "attributed to");
                pcab::logf("  the second-adapter variable. Nothing here is interpreted.");
                if (rows[i].present && rows[i].reason[0] != '\0')
                    pcab::logf("  (that arm reported reason=%s)", rows[i].reason);
                return 4;
            }
        }

        if (!rows[0].control_ok)
        {
            pcab::logf("  E) INVALID. The SINGLE reference lane is valid but did NOT succeed:");
            pcab::logf("     the private CUDA-interop calls, or CreateFeature, did not reproduce");
            pcab::logf("     NeuralScreen's working result. Do NOT interpret the other arms - a");
            pcab::logf("     reference that does not reproduce cannot attribute anything to the");
            pcab::logf("     variable under test.");
            return 2;
        }

        if (!rows[1].control_ok)
        {
            pcab::logf("  A) SINGLE succeeded and DUAL-HELD failed.");
            pcab::logf("     The SIMULTANEOUS PRESENCE of the second adapter (RTX 4070 Ti SUPER)");
            pcab::logf("     device is sufficient to break the private CUDA-interop calls. No second");
            pcab::logf("     queue, no descriptor heap and no NGX use on that device were needed.");
            return 0;
        }

        if (!rows[2].control_ok)
        {
            pcab::logf("  B) SINGLE and DUAL-HELD succeeded, DUAL-ACTIVE failed.");
            pcab::logf("     The device alone is not enough: an ACTIVE second DIRECT command queue");
            pcab::logf("     and CBV/SRV/UAV descriptor heap is what triggers the failure. The");
            pcab::logf("     trigger is active primary command/descriptor state on the second");
            pcab::logf("     adapter, not multi-adapter presence.");
            return 0;
        }

        if (!rows[3].control_ok)
        {
            pcab::logf("  C) SINGLE, DUAL-HELD and DUAL-ACTIVE succeeded, DUAL-RELEASED failed.");
            pcab::logf("     Process-global NVIDIA state PERSISTS after the second D3D12 device is");
            pcab::logf("     released: releasing the device is not a cure, so the residual state is");
            pcab::logf("     created at device creation and outlives it. This is the shape that would");
            pcab::logf("     explain the same failures surviving any amount of device teardown.");
            return 0;
        }

        pcab::logf("  D) ALL FOUR ARMS SUCCEEDED.");
        pcab::logf("     The multi-device explanation is EXONERATED. Neither the simultaneous");
        pcab::logf("     presence of a second adapter, nor an active second DIRECT queue and");
        pcab::logf("     descriptor heap, nor residual process-global state after releasing that");
        pcab::logf("     device reproduces the failure. Whatever breaks these calls inside");
        pcab::logf("     Cyberpunk is NOT the mere existence of the second device, and the");
        pcab::logf("     difference must be sought in what the game does with it (bindings,");
        pcab::logf("     resources, or the caller's own state) rather than in its presence.");
        return 0;
    }
}

// ============================================================================
//  main
// ============================================================================

int main(int argc, char **argv)
{
    const char *mode = nullptr;
    static wchar_t w_nr_dll[MAX_PATH * 2]{};
    static wchar_t w_data[MAX_PATH * 2]{};
    static wchar_t w_holder_event[MAX_PATH]{};
    const wchar_t *nr_dll = nullptr;
    const wchar_t *data_path = nullptr;
    const wchar_t *holder_event = nullptr;
    unsigned holder_seconds = HOLDER_DEFAULT_SECONDS;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
        {
            mode = argv[++i];
        }
        else if (std::strcmp(argv[i], "--nr-dll") == 0 && i + 1 < argc)
        {
            // CP_ACP, not CP_UTF8: these bytes came from the C runtime's narrow
            // argv, which is the process ANSI codepage - the same bytes cmd.exe
            // put on the command line. Decoding them as UTF-8 would mangle any
            // non-ASCII path.
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, w_nr_dll, MAX_PATH * 2);
            nr_dll = w_nr_dll;
        }
        else if (std::strcmp(argv[i], "--data-path") == 0 && i + 1 < argc)
        {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, w_data, MAX_PATH * 2);
            data_path = w_data;
        }
        else if (std::strcmp(argv[i], "--holder-event") == 0 && i + 1 < argc)
        {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, w_holder_event, MAX_PATH);
            holder_event = w_holder_event;
        }
        else if (std::strcmp(argv[i], "--holder-seconds") == 0 && i + 1 < argc)
        {
            const long v = std::strtol(argv[++i], nullptr, 10);
            holder_seconds = (v > 0) ? (unsigned)v : 0u;
        }
        else
        {
            std::fprintf(stderr, "PROCESSCONTEXT-AB: unrecognised argument \"%s\"\n", argv[i]);
            return 1;
        }
    }

    if (mode == nullptr)
    {
        std::fprintf(stderr,
                     "PROCESSCONTEXT-AB\n"
                     "  --mode single|dual-held|dual-active|dual-released|table|holder-ti\n"
                     "  --nr-dll <path to nvngx_dlssnr.dll>   (the four arms only; the\n"
                     "                                         data path is dirname of it)\n"
                     "  --data-path <NGX data path>           (OVERRIDE ONLY; the derived\n"
                     "                                         path must contain the DLL)\n"
                     "  --holder-event <name>                 (holder-ti: named stop event)\n"
                     "  --holder-seconds <n>                  (holder-ti: wait cap, 0 = none)\n");
        return 1;
    }

    if (std::strcmp(mode, "table") == 0) return run_table();

    const bool is_holder = (std::strcmp(mode, HOLDER_MODE) == 0);

    int mode_index = -1;
    for (int i = 0; i < 4; ++i)
        if (std::strcmp(mode, MODE_NAMES[i]) == 0) mode_index = i;
    if (mode_index < 0 && !is_holder)
    {
        std::fprintf(stderr, "PROCESSCONTEXT-AB: unknown --mode \"%s\"\n", mode);
        return 1;
    }
    if (nr_dll == nullptr && !is_holder)
    {
        std::fprintf(stderr, "PROCESSCONTEXT-AB: --nr-dll is required for mode \"%s\"\n", mode);
        return 1;
    }

    pcab::log_open(mode);

    // ---- who we are ------------------------------------------------------
    static wchar_t self[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);

    char self_a[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, self, -1, self_a, sizeof self_a, nullptr, nullptr);
    char self_name[MAX_PATH * 2] = "";
    {
        const char *b = std::strrchr(self_a, '\\');
        std::snprintf(self_name, sizeof self_name, "%s", b ? b + 1 : self_a);
    }
    const bool gate_ok = (std::strstr(self_name, "nvngx.dll") != nullptr);

    pcab::logf("================================================================================");
    pcab::logf("PROCESSCONTEXT-AB  mode=%s", mode);
    pcab::logf("================================================================================");
    pcab::logf("[self]  exe    = %s", self_a);
    pcab::logf("[self]  module = %s", self_name);
    pcab::logf("[self]  cmdline= %ls", GetCommandLineW());
    if (gate_ok)
    {
        pcab::logf("[self]  the module file name contains \"nvngx.dll\" - the DLSSNR snippet's "
                   "caller check can be satisfied. (The MGPU add-on deploys the same way, under a "
                   "file name carrying that prefix.)");
    }
    else if (is_holder)
    {
        // The caller gate is about the DLSSNR snippet refusing a caller whose
        // module name does not contain "nvngx.dll". The holder never loads the
        // snippet and never calls into it, so the gate does not apply to it and
        // warning about it here would be noise in holder.log.
        pcab::logf("[self]  the module file name does not contain \"nvngx.dll\". That gate is "
                   "about the DLSSNR snippet's caller, and holder-ti never loads or calls the "
                   "snippet, so it does not apply to this mode.");
    }
    else
    {
        pcab::logf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        pcab::logf("!! WARNING: this module's file name does NOT contain \"nvngx.dll\".");
        pcab::logf("!! The DLSSNR snippet inspects the module name of its caller and refuses a");
        pcab::logf("!! caller that fails this check with 0xBAD00002 FAIL_PlatformError, for a");
        pcab::logf("!! reason that has NOTHING to do with the hypothesis under test.");
        pcab::logf("!! Run this diagnostic through RUN-CONTEXT-AB.cmd, which copies the exe to a");
        pcab::logf("!! name carrying that prefix. Until then, this run's SINGLE arm cannot be");
        pcab::logf("!! read as the reference and the whole diagnostic is INVALID (rule E).");
        pcab::logf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    }
    pcab::logf("");

    // ---- THE DATA PATH, DERIVED FROM --nr-dll ----------------------------
    //
    // NeuralScreen hands NGX the directory that holds the snippet. This
    // diagnostic must hand it THE SAME directory, or the two lanes are not the
    // same lane - and defaulting to this executable's own directory would put
    // the reference experiment's data path somewhere NeuralScreen's never is.
    //
    // So: dirname(--nr-dll), automatically. --data-path survives only as an
    // explicit override, and RUN-CONTEXT-AB.cmd never needs it.
    //
    // This runs BEFORE the hash gate on purpose. It is pure argument
    // validation - two strings and no file content - so a run with an
    // inconsistent pair of paths is refused before anything is hashed or
    // loaded, and the derivation is testable without a correctly-hashed DLL.
    wchar_t derived_data[MAX_PATH * 2]{};
    if (!is_holder)
    {
        wcsncpy(derived_data, nr_dll, MAX_PATH * 2 - 1);
        wchar_t *slash = wcsrchr(derived_data, L'\\');
        {
            wchar_t *fwd = wcsrchr(derived_data, L'/');
            if (fwd != nullptr && (slash == nullptr || fwd > slash)) slash = fwd;
        }
        if (slash != nullptr) *slash = L'\0';
        else                  wcscpy(derived_data, L".");   // a bare file name

        const bool explicit_override = (data_path != nullptr);
        if (!explicit_override) data_path = derived_data;

        pcab::logf("[path]  NR DLL path        = %ls", nr_dll);
        pcab::logf("[path]  derived data path  = %ls%s", data_path,
                   explicit_override ? "   (from an explicit --data-path override)"
                                     : "   (derived from --nr-dll; NeuralScreen's data path)");

        // REQUIRE THE DLL TO ACTUALLY BE INSIDE THAT DIRECTORY. A data path
        // that does not contain the runtime is not the directory the runtime
        // was told about, and NGX would be pointed at the wrong place.
        bool inside = false;
        if (slash != nullptr) inside = (wcscmp(data_path, derived_data) == 0);
        else                  inside = (wcscmp(data_path, L".") == 0 ||
                                        wcscmp(data_path, L"") == 0);
        if (!inside)
        {
            pcab::logf("[path]  the NR DLL's own directory is \"%ls\", but the data path is "
                       "\"%ls\" - the DLL is NOT inside the directory it would be given. "
                       "Refusing to run.", derived_data, data_path);
            pcab::logf("PCAB-RESULT mode=%s valid=NO reason=NR_DLL_NOT_IN_DATA_PATH "
                       "control=NOT_RUN", mode);
            pcab::log_close();
            return 1;
        }
        pcab::logf("[path]  the NR DLL is inside the data path it will be given - OK");
    }
    pcab::logf("");

    // ---- the NR DLL hash gate, BEFORE the load ---------------------------
    // holder-ti never loads the runtime, so it has nothing to gate: it skips
    // both the hash and the data path entirely rather than hashing 165 MB of a
    // file it will not touch.
    if (!is_holder)
    {
        char hex[65] = "";
        unsigned long size = 0;
        if (!pcab::sha256_file(nr_dll, hex, &size))
        {
            pcab::logf("[gate]  the NR DLL could not be hashed - nothing is loaded");
            pcab::logf("PCAB-RESULT mode=%s valid=NO reason=NR_DLL_UNREADABLE control=NOT_RUN",
                       mode);
            pcab::log_close();
            return 1;
        }
        pcab::logf("[gate]  --nr-dll      = %ls", nr_dll);
        pcab::logf("[gate]  NR DLL size   = %lu bytes", size);
        pcab::logf("[gate]  NR DLL SHA256 = %s", hex);
        pcab::logf("[gate]  required      = %s", EXPECTED_NR_SHA256);
        if (!ieq_ascii(hex, EXPECTED_NR_SHA256))
        {
            pcab::logf("[gate]  MISMATCH. This is not the snippet build NeuralScreen v1.15.0 runs,");
            pcab::logf("[gate]  so a comparison against it would be against a different runtime.");
            pcab::logf("[gate]  ABORTING before the load. Nothing has been created or called.");
            pcab::logf("PCAB-RESULT mode=%s valid=NO reason=NR_DLL_SHA256_MISMATCH control=NOT_RUN",
                       mode);
            pcab::log_close();
            return 1;
        }
        pcab::logf("[gate]  hash matches the required snippet build.");
    }
    pcab::logf("");

    // ---- adapters, by PCI device id --------------------------------------
    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *ad_ti = nullptr;
    IDXGIAdapter1 *ad_4070 = nullptr;
    DXGI_ADAPTER_DESC1 d_ti{}, d_4070{};

    {
        const HRESULT fhr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(fhr) || factory == nullptr)
        {
            pcab::logf("[adapter] CreateDXGIFactory1 hr=0x%08X", (unsigned)fhr);
            pcab::logf("PCAB-RESULT mode=%s valid=NO reason=NO_DXGI_FACTORY control=NOT_RUN", mode);
            pcab::log_close();
            return 1;
        }

        pcab::logf("[adapter] enumerating every adapter and identifying each one by PCI device id "
                   "- no ordinal is ever selected");
        for (unsigned i = 0;; ++i)
        {
            IDXGIAdapter1 *a = nullptr;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            if (a == nullptr) break;

            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            const bool is_nv = (d.VendorId == VENDOR_NVIDIA);
            const bool is_ti = is_nv && d.DeviceId == PCI_TI_SUPER;
            const bool is_40 = is_nv && d.DeviceId == PCI_RTX_4070;

            pcab::logf("[adapter] ordinal=%u vendor=0x%04X device=0x%04X luid=%08lX:%08lX "
                       "%ls  <- %s", i, d.VendorId, d.DeviceId,
                       (unsigned long)d.AdapterLuid.HighPart,
                       (unsigned long)d.AdapterLuid.LowPart,
                       d.Description,
                       is_ti ? "RTX 4070 Ti SUPER (the SECOND adapter, the dual variable)"
                             : is_40 ? "RTX 4070 (the CONTROL lane's adapter)"
                                     : "not used by this diagnostic");

            if (is_ti && ad_ti == nullptr) { ad_ti = a; d_ti = d; continue; }
            if (is_40 && ad_4070 == nullptr) { ad_4070 = a; d_4070 = d; continue; }
            a->Release();
        }

        if (ad_ti == nullptr || ad_4070 == nullptr)
        {
            pcab::logf("[adapter] required adapters not both present: RTX 4070 Ti SUPER "
                       "(10DE:%04X) = %s, RTX 4070 (10DE:%04X) = %s",
                       PCI_TI_SUPER, ad_ti ? "FOUND" : "MISSING",
                       PCI_RTX_4070, ad_4070 ? "FOUND" : "MISSING");
            pcab::logf("[adapter] this diagnostic needs BOTH; without the second adapter there is "
                       "no dual arm to measure, and without the 4070 there is no reference lane.");
            pcab::logf("PCAB-RESULT mode=%s valid=NO reason=REQUIRED_ADAPTER_MISSING control=NOT_RUN",
                       mode);
            pcab::log_close();
            return 1;
        }
        pcab::logf("[adapter] RTX 4070 Ti SUPER -> luid=%08lX:%08lX (selected by device id, not "
                   "by ordinal)",
                   (unsigned long)d_ti.AdapterLuid.HighPart, (unsigned long)d_ti.AdapterLuid.LowPart);
        pcab::logf("[adapter] RTX 4070        -> luid=%08lX:%08lX (selected by device id, not by "
                   "ordinal)",
                   (unsigned long)d_4070.AdapterLuid.HighPart,
                   (unsigned long)d_4070.AdapterLuid.LowPart);
    }
    pcab::logf("");

    // ---- holder-ti: PROCESS A of the split-process experiment -------------
    //
    // It stops HERE. Everything below this point is the RTX 4070 NR lane - the
    // NGX core, core Init, AllocateParameters, NvAPI, the architecture patch,
    // the observer, the snippet and CreateFeature - and the holder must have
    // none of it. Returning here is what makes that structural rather than a
    // promise: there is no path from the holder into the lane.
    if (is_holder)
    {
        const int rc = run_holder_ti(ad_ti, d_ti, holder_event, holder_seconds);
        pcab::log_close();
        return rc;
    }

    // ---- the mode's second-adapter state, established BEFORE the lane -----
    ID3D12Device *dev_ti = nullptr;
    ID3D12CommandQueue *queue_ti = nullptr;
    ID3D12DescriptorHeap *heap_ti = nullptr;
    bool mode_state_ok = true;

    if (mode_index == 0)
    {
        pcab::logf("[mode]  single: no second-adapter state is created at all. This process is "
                   "the reference lane and nothing else.");
    }
    else
    {
        HRESULT hr = D3D12CreateDevice(ad_ti, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_ti));
        pcab::logf("[mode]  %s: D3D12CreateDevice(Ti SUPER, FL_11_0) hr=0x%08X device=%p",
                   mode, (unsigned)hr, (void *)dev_ti);
        if (FAILED(hr) || dev_ti == nullptr)
        {
            pcab::logf("[mode]  the second device could not be created - this arm cannot "
                       "establish its variable, so this run answers nothing");
            pcab::logf("PCAB-RESULT mode=%s valid=NO reason=SECOND_DEVICE_FAILED control=NOT_RUN",
                       mode);
            pcab::log_close();
            return 1;
        }
        pcab::logf("[mode]  recording the second device: desc=\"%ls\" vendor=0x%04X device=0x%04X "
                   "luid=%08lX:%08lX ptr=%p",
                   d_ti.Description, d_ti.VendorId, d_ti.DeviceId,
                   (unsigned long)d_ti.AdapterLuid.HighPart,
                   (unsigned long)d_ti.AdapterLuid.LowPart, (void *)dev_ti);
    }

    if (mode_index == 2)      // dual-active
    {
        // HELD STATE, AND NOTHING ELSE:
        //   the Ti SUPER D3D12 device (created above)
        //   a DIRECT command queue
        //   a tiny CBV/SRV/UAV descriptor heap
        //
        // NO committed resource, NO CBV write, NO submission, NO fence, NO NGX
        // and NO swapchain. An earlier revision created a 256-byte committed
        // buffer and wrote a CBV into the heap; that made this arm vary two
        // things instead of one - live device/queue/heap state AND a live
        // resource with a view. It does not any more.
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        HRESULT hr = dev_ti->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_ti));
        pcab::logf("[mode]  dual-active: CreateCommandQueue(DIRECT) hr=0x%08X queue=%p",
                   (unsigned)hr, (void *)queue_ti);
        if (FAILED(hr)) mode_state_ok = false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 8;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev_ti->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_ti));
        pcab::logf("[mode]  dual-active: CreateDescriptorHeap(CBV_SRV_UAV x8, SHADER_VISIBLE) "
                   "hr=0x%08X heap=%p", (unsigned)hr, (void *)heap_ti);
        if (FAILED(hr)) mode_state_ok = false;

        pcab::logf("[mode]  dual-active: held state is the device, the DIRECT queue and the "
                   "descriptor heap ONLY - no committed resource, no CBV written, no "
                   "submission, no fence, no NGX, no swapchain");
        pcab::logf("[mode]  dual-active: second-adapter state %s",
                   mode_state_ok ? "established" : "PARTIALLY established (see above)");
    }
    else if (mode_index == 3)  // dual-released
    {
        pcab::logf("[mode]  dual-released: releasing the second device now and holding NOTHING "
                   "from it. Any effect that survives is process-global and outlives the device.");
        const ULONG left = dev_ti->Release();
        pcab::logf("[mode]  dual-released: Release() returned %lu remaining reference(s); "
                   "device pointer %p is no longer used", (unsigned long)left, (void *)dev_ti);
        dev_ti = nullptr;
        if (left != 0)
        {
            // The variable this arm exists to create - "the second device is
            // GONE" - was not created. Anything measured from here would be
            // measured with the device still alive, so this is not a result
            // about release at all.
            pcab::logf("[mode]  dual-released: the device is STILL REFERENCED by %lu holder(s) "
                       "after Release(). This arm did not establish its variable, so it is "
                       "INVALID and nothing may be concluded from it.", (unsigned long)left);
            pcab::logf("PCAB-RESULT mode=%s valid=NO reason=SECOND_DEVICE_STILL_REFERENCED "
                       "control=NOT_RUN", mode);
            pcab::log_close();
            return 1;
        }
        pcab::logf("[mode]  dual-released: Release() reached zero - the second device is "
                   "genuinely gone before the lane starts");
    }
    pcab::logf("");

    // An arm whose variable was not actually established cannot be read as a
    // measurement of that variable, so it is reported as invalid rather than as
    // a failure of the private calls.
    if (!mode_state_ok)
    {
        pcab::logf("[mode]  the second-adapter state could not be fully established, so this arm "
                   "did not create the variable it exists to test. Nothing is interpreted.");
        pcab::logf("PCAB-RESULT mode=%s valid=NO reason=SECOND_STATE_NOT_ESTABLISHED "
                   "control=NOT_RUN", mode);
        pcab::log_close();
        return 1;
    }

    // ---- the CONTROL lane: the RTX 4070 device ---------------------------
    //
    // NeuralScreen's order is reproduced here: create the device the lane will
    // use, and only then initialise NVAPI and load the modules.
    Lane lane;
    {
        const HRESULT hr = D3D12CreateDevice(ad_4070, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&lane.dev));
        pcab::logf("[lane]  D3D12CreateDevice(RTX 4070, FL_11_0) hr=0x%08X device=%p",
                   (unsigned)hr, (void *)lane.dev);
        if (FAILED(hr) || lane.dev == nullptr)
        {
            pcab::logf("PCAB-RESULT mode=%s valid=NO reason=CONTROL_DEVICE_FAILED control=NOT_RUN",
                       mode);
            pcab::log_close();
            return 1;
        }
        pcab::logf("[lane]  control device: desc=\"%ls\" vendor=0x%04X device=0x%04X "
                   "luid=%08lX:%08lX ptr=%p",
                   d_4070.Description, d_4070.VendorId, d_4070.DeviceId,
                   (unsigned long)d_4070.AdapterLuid.HighPart,
                   (unsigned long)d_4070.AdapterLuid.LowPart, (void *)lane.dev);

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT qhr = lane.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&lane.queue));
        pcab::logf("[lane]  CreateCommandQueue(DIRECT) hr=0x%08X queue=%p",
                   (unsigned)qhr, (void *)lane.queue);
        if (FAILED(qhr)) { lane.queue = nullptr; }
    }
    if (lane.queue == nullptr)
    {
        pcab::logf("[lane]  the control lane has no DIRECT queue, so the recorded list could never "
                   "be executed or drained. The lane cannot run.");
        pcab::logf("PCAB-RESULT mode=%s valid=NO reason=CONTROL_QUEUE_FAILED control=NOT_RUN", mode);
        pcab::log_close();
        return 1;
    }
    pcab::logf("");

    // ---- the NGX core, loaded BEFORE anything NGX is initialised ---------
    //
    // NeuralScreen loads the core first and only then initialises it. NVAPI is
    // NOT initialised yet at this point and must not be: an explicit
    // NvAPI_Initialize before core Init is not NeuralScreen's sequence, and
    // this diagnostic exists to reproduce that sequence rather than to improve
    // on it.
    if (!pcab::ngx_load_core())
    {
        pcab::logf("PCAB-RESULT mode=%s valid=NO reason=NGX_CORE_LOAD_FAILED control=NOT_RUN", mode);
        pcab::log_close();
        return 1;
    }

    // ---- the lane's core entry points ------------------------------------
    HMODULE core = pcab::ngx_core();
    pcab::pf_init          p_init  = (pcab::pf_init)       pick(core, "NVSDK_NGX_D3D12_Init", "core");
    pcab::pf_alloc_params  p_alloc = (pcab::pf_alloc_params) pick(core, "NVSDK_NGX_D3D12_AllocateParameters", "core");
    pcab::logf("");
    if (p_init == nullptr || p_alloc == nullptr)
    {
        pcab::logf("[lane]  a required core entry point is missing; the lane cannot run");
        pcab::logf("PCAB-RESULT mode=%s valid=NO reason=NGX_ENTRYPOINT_MISSING control=NOT_RUN",
                   mode);
        pcab::log_close();
        return 1;
    }

    // The data path was derived from --nr-dll near the top of main(), before
    // the hash gate, and is already logged there. It is NOT this executable's
    // directory: the reference experiment must hand NGX the same directory
    // NeuralScreen hands it.
    pcab::logf("[lane]  data_path=\"%ls\" app_id=0x%016llX sdk_version=NVSDK_NGX_Version_API",
               data_path, (unsigned long long)pcab::NS_APPLICATION_ID);

    // ---- THE REFERENCE SEQUENCE, IN THE ORDER NeuralScreen USES -----------
    //
    //   1. core NVSDK_NGX_D3D12_Init                 app id 0x1000000
    //   2. NVSDK_NGX_D3D12_AllocateParameters        a PRIVATE block this lane owns
    //   3. NvAPI_Initialize
    //   4. nv_set_arch_target + arch_cache_real + install the arch patch
    //   5. install the observational QueryInterface observer
    //   6. load the exact nvngx_dlssnr.dll
    //   7. snippet NVSDK_NGX_D3D12_Init_Ext          app id 0x1000000
    //   8. params->Reset()
    //   9. the full 640x360 creation contract
    //  10. CreateFeature(NVSDK_NGX_Feature_Reserved18)
    //
    // NvAPI_Initialize is AFTER core Init and AllocateParameters, because that
    // is NeuralScreen's working sequence. The observer is installed as LATE as
    // it can be while still being in place before the private runtime loads: it
    // only needs to see the two CUDA ids, so it has no business perturbing core
    // Init or the architecture setup.
    //
    // From the moment the lane starts, a failure is a RESULT about the lane
    // (valid=YES, control=FAIL) rather than an inability to run, so that an
    // experiment can tell "this arm did not reproduce" from "this arm could
    // not be run at all".

    // 1. the core session, with NeuralScreen's FeatureCommonInfo.
    {
        NVSDK_NGX_FeatureCommonInfo common{};
        common.LoggingInfo.LoggingCallback = &ngx_log_discard;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        common.LoggingInfo.DisableOtherLoggingSinks = true;
        pcab::logf("[lane]  FeatureCommonInfo: zero-initialised, logging callback = discard, "
                   "MinimumLoggingLevel = OFF, DisableOtherLoggingSinks = true "
                   "(NeuralScreen's configuration)");
        lane.core_init = (unsigned)p_init(pcab::NS_APPLICATION_ID, data_path, lane.dev, &common,
                                          NVSDK_NGX_Version_API);
        pcab::logf("[lane]  core NVSDK_NGX_D3D12_Init(app_id=0x%016llX) -> 0x%08X (%s)",
                   (unsigned long long)pcab::NS_APPLICATION_ID, lane.core_init,
                   ngx_result_name(lane.core_init));
        if (lane.core_init != 0x1u)
        {
            pcab::logf("[lane]  core Init is not Success. Nothing downstream can be formed on an "
                       "uninitialised session, so the lane stops here and reports FAIL.");
            pcab::logf("PCAB-RESULT mode=%s valid=YES reason=CORE_INIT_NOT_SUCCESS control=FAIL "
                       "core_init=0x%08X alloc=0x%08X snip_init=0x%08X feature=0x%08X "
                       "desc_status=NA desc_calls=0 desc_probes=0 cu_status=NA cu_calls=0 "
                       "cu_probes=0 blob=0 handle=0x0", mode, lane.core_init, 0u, 0u, 0u);
            pcab::log_close();
            return 0;
        }
    }

    // 2. the parameter block, from AllocateParameters. A block this lane OWNS:
    //    it is the one that may be Reset() and filled with the creation
    //    contract. The core's own capability block is shared with every other
    //    consumer in the process, which is exactly the wrong thing to overwrite.
    {
        NVSDK_NGX_Parameter *params = nullptr;
        lane.alloc = (unsigned)p_alloc(&params);
        lane.params = params;
        pcab::logf("[lane]  NVSDK_NGX_D3D12_AllocateParameters -> 0x%08X (%s) params=%p",
                   lane.alloc, ngx_result_name(lane.alloc), (void *)params);
        if (lane.alloc != 0x1u || params == nullptr)
        {
            pcab::logf("[lane]  AllocateParameters did not return Success with a non-null block. "
                       "Without a block there is no creation contract to apply, so the lane "
                       "stops here and reports FAIL.");
            pcab::logf("PCAB-RESULT mode=%s valid=YES reason=ALLOCATE_PARAMETERS_FAILED "
                       "control=FAIL core_init=0x%08X alloc=0x%08X snip_init=0x%08X "
                       "feature=0x%08X desc_status=NA desc_calls=0 desc_probes=0 cu_status=NA "
                       "cu_calls=0 cu_probes=0 blob=0 handle=0x0", mode, lane.core_init,
                       lane.alloc, 0u, 0u);
            pcab::log_close();
            return 0;
        }
    }

    // 3. NvAPI_Initialize. AFTER core Init and AFTER AllocateParameters: this is
    //    NeuralScreen's sequence, and an explicit NvAPI_Initialize before core
    //    Init is not part of it.
    if (!pcab::nv_load_and_initialize())
    {
        pcab::logf("[nvapi] NVAPI could not be brought up - this is an environment result, not a "
                   "context result, and the run answers nothing");
        pcab::logf("PCAB-RESULT mode=%s valid=NO reason=NVAPI_INIT_FAILED control=NOT_RUN", mode);
        pcab::log_close();
        return 1;
    }
    pcab::logf("");

    // 4. the architecture patch. Cached for EVERY GPU first, with nothing
    //    hooked, then installed over exactly one target entry.
    //
    //    Two separate statements on purpose. Writing this as one `a() || b()`
    //    line relies on short-circuit evaluation to keep the order right, which
    //    is correct but invisible - and "cache BEFORE the hook" is the property
    //    the whole patch depends on. Separate statements also let the CI gate
    //    assert the order by line position.
    pcab::nv_set_arch_target(VENDOR_NVIDIA, PCI_RTX_4070);
    const bool arch_cached = pcab::arch_cache_real();
    const bool arch_patched = arch_cached ? pcab::arch_patch_install() : false;
    if (!arch_cached || !arch_patched)
    {
        pcab::logf("[lane]  the NeuralScreen-compatible architecture patch is NOT installed. The "
                   "DLSSNR 310.8.0 runtime is expected to refuse the real Ada architecture, so a "
                   "failure downstream of this has a cause that is NOT the hypothesis under test.");
    }
    pcab::logf("");

    // 5. the observational observer, installed AS LATE AS IT CAN BE while still
    //    being in place before the private runtime is loaded.
    //
    //    It hooks nvapi_QueryInterface to see the two CUDA ids and nothing else,
    //    so it has no business being installed early enough to perturb core
    //    Init or the architecture setup. The lane's device is published to it
    //    here, immediately before installation, because that is the first moment
    //    it needs the identity and the device already exists.
    pcab::observe_set_expected_device((void *)lane.dev);
    const bool observed = pcab::observe_install(mode);
    pcab::logf("[nvapi] observation installed = %s (installed after NVAPI init, the architecture "
               "cache and the arch patch, and before the private runtime loads)",
               observed ? "yes" : "NO");
    pcab::logf("");

    // 6. the exact private runtime, by path, AFTER the patch and AFTER the
    //    observer, and before the snippet session that needs it.
    if (!pcab::ngx_load_snippet(nr_dll))
    {
        pcab::logf("PCAB-RESULT mode=%s valid=NO reason=SNIPPET_LOAD_FAILED control=NOT_RUN", mode);
        pcab::log_close();
        return 1;
    }

    // 6b. the snippet's own entry points, resolved now that it is loaded.
    HMODULE snip = pcab::ngx_snippet();
    pcab::pf_init_ext       p_sinit  = (pcab::pf_init_ext)       pick(snip, "NVSDK_NGX_D3D12_Init_Ext", "snippet");
    pcab::pf_create_feature p_create = (pcab::pf_create_feature) pick(snip, "NVSDK_NGX_D3D12_CreateFeature", "snippet");
    if (p_sinit == nullptr || p_create == nullptr)
    {
        pcab::logf("[lane]  the snippet does not export the entry points this lane needs");
        pcab::logf("PCAB-RESULT mode=%s valid=NO reason=NGX_ENTRYPOINT_MISSING control=NOT_RUN",
                   mode);
        pcab::log_close();
        return 1;
    }

    // 7. the snippet's own Reserved18 session, same app id, with our block.
    lane.snip_init = (unsigned)p_sinit(pcab::NS_APPLICATION_ID, data_path, lane.dev,
                                       NVSDK_NGX_Version_API, lane.params);
    pcab::logf("[lane]  snippet NVSDK_NGX_D3D12_Init_Ext(app_id=0x%016llX) -> 0x%08X (%s)",
               (unsigned long long)pcab::NS_APPLICATION_ID, lane.snip_init,
               ngx_result_name(lane.snip_init));
    if (lane.snip_init != 0x1u)
    {
        // Deliberately NOT fatal: the reference implementation continues so the
        // later results still report, and CreateFeature repeats the failure
        // with its own code. The lane is already a FAIL either way.
        pcab::logf("[lane]  snippet Init_Ext is not Success. Continuing anyway so the CUDA-interop "
                   "and CreateFeature results still report - the lane is a FAIL either way.");
    }

    // 6 + 7. Reset, then the exact NeuralScreen 640x360 creation contract.
    apply_reference_contract(lane.params);

    // 6. a private allocator, an open command list, a fence and an event.
    {
        HRESULT hr = lane.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&lane.allocator));
        pcab::logf("[lane]  CreateCommandAllocator hr=0x%08X", (unsigned)hr);
        if (FAILED(hr)) { pcab::logf("PCAB-RESULT mode=%s valid=NO reason=ALLOCATOR_FAILED control=NOT_RUN", mode); pcab::log_close(); return 1; }

        hr = lane.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lane.allocator, nullptr,
                                        IID_PPV_ARGS(&lane.list));
        pcab::logf("[lane]  CreateCommandList hr=0x%08X (open and recording, never Reset)",
                   (unsigned)hr);
        if (FAILED(hr)) { pcab::logf("PCAB-RESULT mode=%s valid=NO reason=COMMAND_LIST_FAILED control=NOT_RUN", mode); pcab::log_close(); return 1; }

        hr = lane.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lane.fence));
        pcab::logf("[lane]  CreateFence hr=0x%08X", (unsigned)hr);
        if (FAILED(hr)) { pcab::logf("PCAB-RESULT mode=%s valid=NO reason=FENCE_FAILED control=NOT_RUN", mode); pcab::log_close(); return 1; }

        lane.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        pcab::logf("[lane]  CreateEventW -> %p", lane.event);
        if (lane.event == nullptr) { pcab::logf("PCAB-RESULT mode=%s valid=NO reason=EVENT_FAILED control=NOT_RUN", mode); pcab::log_close(); return 1; }
    }

    // 7. THE QUESTION. Everything above is plumbing.
    {
        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        NVSDK_NGX_Handle *handle = nullptr;
        lane.reached_create = true;
        lane.feature = (unsigned)p_create(lane.list, NVSDK_NGX_Feature_Reserved18,
                                         lane.params, &handle);
        QueryPerformanceCounter(&t1);
        lane.handle = handle;
        const double ms = (f.QuadPart > 0)
            ? ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart) : 0.0;
        pcab::logf("[lane]  CreateFeature(NVSDK_NGX_Feature_Reserved18) via the snippet -> "
                   "0x%08X (%s) handle=%p %ux%u elapsed=%.0fms",
                   lane.feature, ngx_result_name(lane.feature), (void *)handle,
                   CTRL_W, CTRL_H, ms);
    }
    pcab::logf("");

    // ---- the drain, exactly as MGPU does it ------------------------------
    //
    // Not branched on whether CreateFeature succeeded: executing a list NGX
    // recorded nothing into is harmless, and executing one it half-recorded and
    // then abandoned is the case that must not be skipped. Releasing a list whose
    // work is still in flight is the documented device-lost pattern.
    {
        const HRESULT chr = lane.list->Close();
        pcab::logf("[drain] Close hr=0x%08X", (unsigned)chr);
        if (SUCCEEDED(chr))
        {
            ID3D12CommandList *const lists[1] = { lane.list };
            lane.queue->ExecuteCommandLists(1, lists);
            const HRESULT shr = lane.queue->Signal(lane.fence, 1);
            pcab::logf("[drain] Signal(fence,1) hr=0x%08X", (unsigned)shr);
            if (SUCCEEDED(shr))
            {
                lane.fence->SetEventOnCompletion(1, lane.event);
                const DWORD wr = WaitForSingleObject(lane.event, 10000);
                lane.drained = (wr == WAIT_OBJECT_0);
                pcab::logf("[drain] fence wait -> 0x%08X (%s)", (unsigned)wr,
                           lane.drained ? "drained" : "TIMED OUT - nothing will be released");
            }
        }
        else
        {
            pcab::logf("[drain] the list was not in a closable state; nothing is released");
        }
    }
    pcab::logf("");

    // ---- the observation --------------------------------------------------
    pcab::ObserveSummary obs = pcab::observe_summary();
    const unsigned desc_real_calls = (obs.descriptor_calls > obs.descriptor_probes)
                                   ? obs.descriptor_calls - obs.descriptor_probes : 0;
    const unsigned cu_real_calls = (obs.cumodule_calls > obs.cumodule_probes)
                                 ? obs.cumodule_calls - obs.cumodule_probes : 0;
    pcab::logf("[cuda]  GetCudaIndependentDescriptorObject (0x0DDAC234): real_calls=%u "
               "probes=%u real_status=%s probe_status=%s",
               desc_real_calls, obs.descriptor_probes,
               obs.real_descriptor_status == -12345 ? "NOT OBSERVED"
                                                    : nv_status_name(obs.real_descriptor_status),
               obs.descriptor_probe_status == -12345 ? "none"
                                                     : nv_status_name(obs.descriptor_probe_status));
    pcab::logf("[cuda]  CreateCuModule (0xAD1A677D): real_calls=%u probes=%u real_status=%s "
               "probe_status=%s",
               cu_real_calls, obs.cumodule_probes,
               obs.real_cumodule_status == -12345 ? "NOT OBSERVED"
                                                  : nv_status_name(obs.real_cumodule_status),
               obs.cumodule_probe_status == -12345 ? "none"
                                                   : nv_status_name(obs.cumodule_probe_status));
    if (obs.mismatch)
        pcab::logf("[cuda]  the resolver returned a second, different pointer for at least one id; "
                   "the new pointer was handed back unwrapped, so no caller saw substituted "
                   "behaviour. The observation below may be partial - read the MISMATCH lines.");
    if (obs.resolver_returned_null)
        pcab::logf("[cuda]  the resolver returned nullptr for at least one of the two ids: that "
                   "interface is ABSENT in this process, the null was returned unchanged, and no "
                   "wrapper was manufactured for it.");
    if (obs.first_blob_size != 0)
        pcab::logf("[cuda]  first CreateCuModule blob size = %llu bytes (the blob itself is never "
                   "read)", obs.first_blob_size);
    pcab::logf("");

    // ---- the verdict for THIS arm ----------------------------------------
    //
    // THE SINGLE REFERENCE GATE, and the same eight conditions apply to every
    // arm - a dual arm that does not reproduce is a failure of that arm for the
    // same reason. AutoLab evaluates the identical eight conditions itself from
    // the PCAB-RESULT line, so agreement between the two is a check on both.
    const bool init_ok  = (lane.core_init == 0x00000001u);
    const bool alloc_ok = (lane.alloc == 0x00000001u);
    const bool snip_ok  = (lane.snip_init == 0x00000001u);
    const bool desc_ok  = (obs.real_descriptor_status == 0);
    const bool cu_ok    = (obs.real_cumodule_status == 0);
    const bool blob_ok  = (obs.first_blob_size == 3944768ULL);
    const bool feat_ok  = (lane.feature == 0x00000001u);
    const bool handle_ok = (lane.handle != nullptr);
    const bool control_ok = init_ok && alloc_ok && snip_ok && desc_ok && cu_ok && blob_ok &&
                            feat_ok && handle_ok;

    pcab::logf("[result] this arm: core_init=%s alloc=%s snip_init=%s descriptor=%s module=%s "
               "blob=%s Reserved18=%s handle=%s  ->  LANE %s",
               init_ok ? "Success" : "not Success",
               alloc_ok ? "Success" : "not Success",
               snip_ok ? "Success" : "not Success",
               desc_ok ? "NVAPI_OK" : "not OK",
               cu_ok ? "NVAPI_OK" : "not OK",
               blob_ok ? "3944768" : "not 3944768",
               feat_ok ? "Success" : "not Success",
               handle_ok ? "non-null" : "NULL",
               control_ok ? "OK" : "FAIL");

    // ---- machine-readable line for --mode table --------------------------
    //
    // One line, fixed field order, signed decimal statuses so that the parser
    // needs no sign handling, and NA where a call was not observed at all. The
    // fields are EXACTLY the eight the reference gate needs, so the gate can be
    // evaluated from this line alone.
    pcab::logf("PCAB-RESULT mode=%s valid=YES reason=%s control=%s core_init=0x%08X "
               "alloc=0x%08X snip_init=0x%08X feature=0x%08X "
               "desc_status=%s desc_calls=%u desc_probes=%u "
               "cu_status=%s cu_calls=%u cu_probes=%u blob=%llu handle=0x%llX",
               mode,
               control_ok ? "LANE_OK" : "LANE_FAIL",
               control_ok ? "OK" : "FAIL",
               lane.core_init, lane.alloc, lane.snip_init, lane.feature,
               obs.real_descriptor_status == -12345 ? "NA" : dec(obs.real_descriptor_status),
               obs.descriptor_calls, obs.descriptor_probes,
               obs.real_cumodule_status == -12345 ? "NA" : dec(obs.real_cumodule_status),
               obs.cumodule_calls, obs.cumodule_probes,
               obs.first_blob_size, (unsigned long long)(uintptr_t)lane.handle);

    pcab::observe_remove();
    pcab::arch_patch_remove();

    // Deliberately NOT released: the NGX session, the feature, the command list,
    // the allocator, the fence and the capability block. MGPU keeps its NGX
    // session for the process lifetime for the same reason - every crash observed
    // in this project has been in teardown, none in use - and this process exits
    // immediately, so the OS reclaims all of it. The second adapter's state is
    // likewise left exactly as the arm defines it.
    pcab::logf("[exit]  nothing is torn down on purpose: this process is one arm of a four-arm "
               "experiment and exits now. mode=%s", mode);
    pcab::log_close();
    return 0;
}
