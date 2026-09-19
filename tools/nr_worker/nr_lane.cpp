// ============================================================================
// MGPU NR worker - the proven initialization lane. See nr_lane.h.
//
// The NVAPI, architecture-patch, observation and NGX-module code is
// ../processcontext_ab/nv.cpp and ../processcontext_ab/observe.cpp, compiled
// VERBATIM into this target. Nothing in this file re-implements them; what is
// here is the ORDER they are called in, and the creation contract, both taken
// from the frozen PROCESSCONTEXT-AB lane and asserted equal to it by a CI gate.
// ============================================================================

#include "nr_lane.h"

#include "../processcontext_ab/nv.h"
#include "../processcontext_ab/observe.hpp"

#include <windows.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace nr
{
    namespace
    {
        struct LaneState
        {
            ID3D12Device              *dev = nullptr;
            ID3D12CommandQueue        *queue = nullptr;
            ID3D12CommandAllocator    *allocator = nullptr;
            ID3D12GraphicsCommandList *list = nullptr;
            ID3D12Fence               *fence = nullptr;
            HANDLE                     event = nullptr;
            NVSDK_NGX_Parameter       *params = nullptr;
            bool                       list_submitted = false;
        };

        LaneState g_lane;

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

        FARPROC pick(HMODULE m, const char *name, const char *who)
        {
            FARPROC p = (m != nullptr) ? GetProcAddress(m, name) : nullptr;
            pcab::logf("[ngx]    %-40s (%-7s) -> %p", name, who, (void *)p);
            return p;
        }

        // ---- THE CREATION CONTRACT ------------------------------------------
        //
        // Character for character the contract the proven lane applies, with the
        // same keys, the same values and the same order. The generic Width/Height
        // pair is deliberately NOT set: NeuralScreen does not set it, and adding
        // it would make this worker differ from the reference it reproduces in
        // exactly the place that matters.
        //
        // A CI gate compares this function's params->Set(...) sequence against
        // the frozen ../processcontext_ab/main.cpp one, key for key and value for
        // value, in order, so the two cannot drift apart silently.
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

            params->Set("DLSSNR.Width",  (unsigned)NR_CTRL_W);
            params->Set("DLSSNR.Height", (unsigned)NR_CTRL_H);
            params->Set("DLSSNR.InputWidth",  (unsigned)NR_CTRL_W);
            params->Set("DLSSNR.InputHeight", (unsigned)NR_CTRL_H);
            params->Set("DLSSNR.OutputWidth",  (unsigned)NR_CTRL_W);
            params->Set("DLSSNR.OutputHeight", (unsigned)NR_CTRL_H);
            params->Set("DLSSNR.Output.Width",  (unsigned)NR_CTRL_W);
            params->Set("DLSSNR.Output.Height", (unsigned)NR_CTRL_H);
            pcab::logf("[contract] DLSSNR.Width/Height=%u/%u Input=%u/%u Output=%u/%u "
                       "DLSSNR.Output.Width/Height=%u/%u",
                       NR_CTRL_W, NR_CTRL_H, NR_CTRL_W, NR_CTRL_H, NR_CTRL_W, NR_CTRL_H,
                       NR_CTRL_W, NR_CTRL_H);

            params->Set("DLSSNR.Upscaling", 0u);
            params->Set("DLSSNR.Scale", 1.0f);
            params->Set("DLSSNR.ScalingRatio", 1.0f);
            pcab::logf("[contract] DLSSNR.Upscaling=0 DLSSNR.Scale=1.0 DLSSNR.ScalingRatio=1.0");

            params->Set("DLSSNR.Hint.Render.Preset", 0u);
            params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0);
            pcab::logf("[contract] DLSSNR.Hint.Render.Preset=0 DLSS.Feature.Create.Flags=0");

            pcab::logf("[contract] NO generic Width/Height is set - NeuralScreen does not set them, "
                       "and adding them here would make this lane differ from the reference it "
                       "reproduces");
        }

        // ---- the data path, derived exactly as the diagnostic derives it -----
        void derive_data_path(const wchar_t *nr_dll, wchar_t *out, size_t out_chars)
        {
            if (out_chars == 0) return;
            out[0] = L'\0';
            if (nr_dll == nullptr) return;
            std::wcsncpy(out, nr_dll, out_chars - 1);
            out[out_chars - 1] = L'\0';
            wchar_t *slash = std::wcsrchr(out, L'\\');
            wchar_t *fwd = std::wcsrchr(out, L'/');
            if (fwd != nullptr && (slash == nullptr || fwd > slash)) slash = fwd;
            if (slash != nullptr) *slash = L'\0';
            else                  std::wcscpy(out, L".");
        }

        bool path_contains(const wchar_t *dir, const wchar_t *file)
        {
            if (dir == nullptr || file == nullptr) return false;
            const size_t n = std::wcslen(dir);
            return std::wcsncmp(dir, file, n) == 0 &&
                   (file[n] == L'\\' || file[n] == L'/');
        }

        // ---- the adapter, by PCI identity -----------------------------------
        bool find_adapter(IDXGIFactory1 *factory, unsigned vendor, unsigned device,
                          IDXGIAdapter1 **out_adapter, DXGI_ADAPTER_DESC1 *out_desc,
                          std::string &why)
        {
            for (UINT i = 0;; ++i)
            {
                IDXGIAdapter1 *candidate = nullptr;
                if (factory->EnumAdapters1(i, &candidate) != S_OK) break;
                if (candidate == nullptr) continue;

                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(candidate->GetDesc1(&d)) &&
                    (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                    d.VendorId == vendor && d.DeviceId == device)
                {
                    *out_adapter = candidate;
                    *out_desc = d;
                    return true;
                }
                candidate->Release();
            }
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "no adapter with vendor 0x%04X device 0x%04X was enumerated",
                          vendor, device);
            why = buf;
            return false;
        }
    }

    ID3D12Device *lane_device() { return g_lane.dev; }
    ID3D12CommandQueue *lane_queue() { return g_lane.queue; }

    void lane_note_shutdown()
    {
        pcab::logf("[exit]  nothing is torn down on purpose: this process owns the NGX session "
                   "and the Reserved18 feature, and exits with them - the same choice the proven "
                   "reference lane makes. mode=nr-worker");
    }

    std::string LaneResult::failing_conditions() const
    {
        std::string out;
        char buf[160];
        struct Item { const char *name; bool ok; };
        const Item items[] = {
            { "core Init Success", core_init == 0x1u },
            { "AllocateParameters Success", alloc == 0x1u },
            { "snippet Init_Ext Success", snip_init == 0x1u },
            { "Descriptor 0", descriptor_status == 0 },
            { "CuModule 0", cumodule_status == 0 },
            { "blob 3944768", first_blob_size == NR_EXPECTED_BLOB },
            { "Reserved18 Success", feature == 0x1u },
            { "handle != 0", feature_handle != 0 },
        };
        for (const Item &it : items)
        {
            if (it.ok) continue;
            if (!out.empty()) out += "; ";
            out += it.name;
        }
        std::snprintf(buf, sizeof buf, " [core_init=0x%08X alloc=0x%08X snip_init=0x%08X "
                      "feature=0x%08X desc=%d cu=%d blob=%llu handle=0x%llX]",
                      core_init, alloc, snip_init, feature, descriptor_status,
                      cumodule_status, first_blob_size, feature_handle);
        out += buf;
        return out;
    }

    bool lane_run(const LaneOptions &opt, LaneResult &res)
    {
        res.pid = (unsigned long)GetCurrentProcessId();

        pcab::logf("================================================================================");
        pcab::logf("mgpu_nr_worker  -  the neural lane, in a process that is not the game");
        pcab::logf("================================================================================");
        pcab::logf("[worker] pid      = %lu", res.pid);
        pcab::logf("[worker] cmdline  = %ls", GetCommandLineW());

        // ---- 0. THE CALLER GATE, STATED BEFORE ANYTHING IS LOADED -----------
        //
        // The snippet inspects the module name of its caller and refuses one that
        // does not contain "nvngx.dll" with 0xBAD00002, for a reason that has
        // nothing to do with the work. Production launches this worker from a
        // file name carrying that prefix; running the artefact under its plain
        // name would produce a Reserved18 failure that means nothing.
        {
            wchar_t self[MAX_PATH * 2] = L"";
            GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
            const wchar_t *base = std::wcsrchr(self, L'\\');
            base = (base != nullptr) ? base + 1 : self;
            const bool gate_ok = (std::wcsstr(base, L"nvngx.dll") != nullptr);
            if (gate_ok)
            {
                pcab::logf("[self]   module = %ls - carries the nvngx.dll caller-gate prefix",
                           base);
            }
            else
            {
                pcab::logf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                pcab::logf("!! WARNING: this module's file name does NOT contain \"nvngx.dll\".");
                pcab::logf("!! The snippet inspects the name of its caller and will refuse with");
                pcab::logf("!! 0xBAD00002 FAIL_PlatformError for a reason that has NOTHING to do");
                pcab::logf("!! with the neural work. Run this through RUN-NR-WORKER.cmd, which");
                pcab::logf("!! makes a prefixed copy, or expect Reserved18 to fail.");
                pcab::logf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
            }
        }

        if (opt.nr_dll == nullptr || opt.nr_dll[0] == L'\0')
        {
            res.error = "--nr-dll was not given, so there is no runtime to load";
            return false;
        }

        wchar_t data_path[MAX_PATH * 2] = L"";
        derive_data_path(opt.nr_dll, data_path, MAX_PATH * 2);
        pcab::logf("[path]   NR DLL path        = %ls", opt.nr_dll);
        pcab::logf("[path]   derived data path  = %ls   (derived from --nr-dll; "
                   "NeuralScreen's data path)", data_path);
        if (!path_contains(data_path, opt.nr_dll))
        {
            res.error = "the NR DLL is NOT inside the data path it would be given";
            pcab::logf("[path]   \"%ls\" is not inside \"%ls\" - refusing to run, because the "
                       "snippet must be handed the directory it actually lives in.",
                       opt.nr_dll, data_path);
            return false;
        }
        pcab::logf("[path]   the NR DLL is inside the data path it will be given - OK");

        // ---- the hash gate, before ANYTHING is loaded -----------------------
        if (opt.expected_nr_sha256 != nullptr && opt.expected_nr_sha256[0] != '\0')
        {
            char hex[65] = "";
            unsigned long size = 0;
            if (!pcab::sha256_file(opt.nr_dll, hex, &size))
            {
                res.error = "the NR DLL could not be hashed - nothing is loaded";
                pcab::logf("[gate]   the NR DLL could not be hashed - nothing is loaded");
                return false;
            }
            pcab::logf("[gate]   --nr-dll      = %ls", opt.nr_dll);
            pcab::logf("[gate]   NR DLL size   = %lu bytes", size);
            pcab::logf("[gate]   NR DLL SHA256 = %s", hex);
            pcab::logf("[gate]   required      = %s", opt.expected_nr_sha256);
            if (_stricmp(hex, opt.expected_nr_sha256) != 0)
            {
                res.error = "NR_DLL_SHA256_MISMATCH";
                pcab::logf("[gate]   MISMATCH. This is not the runtime the proven lane runs, so a");
                pcab::logf("[gate]   comparison against it would be against a different runtime.");
                pcab::logf("[gate]   ABORTING before the load. Nothing has been created or called.");
                return false;
            }
            pcab::logf("[gate]   hash matches the required runtime build.");
        }

        // ---- 1 + 2. the factory, the adapter, the device, the queue ---------
        IDXGIFactory1 *factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        pcab::logf("[adapter] CreateDXGIFactory1 hr=0x%08X", (unsigned)hr);
        if (FAILED(hr) || factory == nullptr)
        {
            res.error = "CreateDXGIFactory1 failed";
            return false;
        }
        pcab::logf("[adapter] every adapter is identified by PCI id - no ordinal is ever selected");

        IDXGIAdapter1 *adapter = nullptr;
        DXGI_ADAPTER_DESC1 desc{};
        if (!find_adapter(factory, opt.adapter_vendor, opt.adapter_device, &adapter, &desc,
                          res.error))
        {
            factory->Release();
            pcab::logf("[adapter] %s", res.error.c_str());
            return false;
        }

        // Both COM objects are released by this, on every path out - including
        // the "the lane ran and failed" paths, which are results and not errors.
        struct ReleaseOnExit
        {
            IDXGIAdapter1 *adapter;
            IDXGIFactory1 *factory;
            ~ReleaseOnExit() { adapter->Release(); factory->Release(); }
        } release_on_exit{ adapter, factory };

        res.adapter_vendor = desc.VendorId;
        res.adapter_device = desc.DeviceId;
        res.luid_high = (unsigned)desc.AdapterLuid.HighPart;
        res.luid_low = (unsigned)desc.AdapterLuid.LowPart;
        std::wcsncpy(res.adapter_name, desc.Description, 127);
        pcab::logf("[adapter] the neural adapter: desc=\"%ls\" vendor=0x%04X device=0x%04X "
                   "luid=%08lX:%08lX", desc.Description, desc.VendorId, desc.DeviceId,
                   (unsigned long)desc.AdapterLuid.HighPart,
                   (unsigned long)desc.AdapterLuid.LowPart);

        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_lane.dev));
        pcab::logf("[lane]   D3D12CreateDevice(RTX 4070, FL_11_0) hr=0x%08X device=%p",
                   (unsigned)hr, (void *)g_lane.dev);
        if (FAILED(hr) || g_lane.dev == nullptr)
        {
            res.error = "D3D12CreateDevice failed on the RTX 4070";
            return false;
        }
        res.device_ok = true;

        {
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            const HRESULT qhr = g_lane.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_lane.queue));
            pcab::logf("[lane]   CreateCommandQueue(DIRECT) hr=0x%08X queue=%p",
                       (unsigned)qhr, (void *)g_lane.queue);
            if (FAILED(qhr)) g_lane.queue = nullptr;
        }
        if (g_lane.queue == nullptr)
        {
            res.error = "the lane has no DIRECT queue, so a recorded list could never be executed";
            return false;
        }
        pcab::logf("");

        // ---- 3 + 4. the NGX core, loaded BEFORE anything NGX is initialised --
        if (!pcab::ngx_load_core())
        {
            res.error = "the NGX core could not be loaded";
            return false;
        }
        res.core_loaded = true;

        HMODULE core = pcab::ngx_core();
        pcab::pf_init         p_init  = (pcab::pf_init)pick(core, "NVSDK_NGX_D3D12_Init", "core");
        pcab::pf_alloc_params p_alloc = (pcab::pf_alloc_params)pick(
            core, "NVSDK_NGX_D3D12_AllocateParameters", "core");
        pcab::logf("");
        if (p_init == nullptr || p_alloc == nullptr)
        {
            res.error = "a required core entry point is missing";
            return false;
        }

        pcab::logf("[lane]   data_path=\"%ls\" app_id=0x%016llX sdk_version=NVSDK_NGX_Version_API",
                   data_path, (unsigned long long)pcab::NS_APPLICATION_ID);

        // 4. the core session, with NeuralScreen's FeatureCommonInfo.
        {
            NVSDK_NGX_FeatureCommonInfo common{};
            common.LoggingInfo.LoggingCallback = &ngx_log_discard;
            common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
            common.LoggingInfo.DisableOtherLoggingSinks = true;
            pcab::logf("[lane]   FeatureCommonInfo: zero-initialised, logging callback = discard, "
                       "MinimumLoggingLevel = OFF, DisableOtherLoggingSinks = true");

            unsigned long long stamp = opt.ngx_version ? opt.ngx_version
                                                      : (unsigned long long)NVSDK_NGX_Version_API;

            if (opt.ngx_version_sweep)
            {
                // THE LADDER. Ascending, so the answer is the LOWEST stamp the
                // installed runtime accepts rather than the highest that happens
                // to work. Each entry is a real NGX SDK version number; the
                // encoding is 0xMMmmppbb, which is why they read as they do.
                static const unsigned long long ladder[] = {
                    0x0000000000020000ull,   // 2.0   - the header default era
                    0x0000000000020200ull,   // 2.2
                    0x0000000000020301ull,   // 2.3.1
                    0x0000000000020401ull,   // 2.4.1
                    0x0000000000020501ull,   // 2.5.1
                    0x0000000000030100ull,   // 3.1
                    0x0000000000030502ull,   // 3.5.2
                    0x0000000000030700ull,   // 3.7
                    0x0000000000030800ull,   // 3.8
                    0x0000000000030900ull,   // 3.9
                    0x0000000000040000ull,   // 4.0
                    0x0000000000050000ull,   // 5.0
                    0x0000000000060000ull,   // 6.0 - past anything documented, kept so
                                             //       the ladder's END is visible too
                };
                pcab::logf("[ngxver] sweeping %u candidate SDK version stamps, ascending",
                           (unsigned)(sizeof ladder / sizeof ladder[0]));
                unsigned long long accepted = 0;
                unsigned accepted_result = 0;
                for (unsigned k = 0; k < sizeof ladder / sizeof ladder[0]; ++k)
                {
                    const unsigned r = (unsigned)p_init(pcab::NS_APPLICATION_ID, data_path,
                                                        g_lane.dev, &common,
                                                        (NVSDK_NGX_Version)ladder[k]);
                    pcab::logf("[ngxver] 0x%016llX -> 0x%08X (%s)", ladder[k], r,
                               ngx_result_name(r));
                    if (r == 0x1u) { accepted = ladder[k]; accepted_result = r; break; }
                }
                if (accepted != 0)
                {
                    pcab::logf("[ngxver] ACCEPTED 0x%016llX - this runtime wants a stamp at least "
                               "this new", accepted);
                    stamp = accepted;
                    res.core_init = accepted_result;
                    res.ngx_version_accepted = accepted;
                    res.ngx_version_swept = true;
                }
                else
                {
                    pcab::logf("[ngxver] NO candidate was accepted. The runtime refused every "
                               "stamp in the ladder, so this is not simply an out-of-date "
                               "declaration and the sweep says nothing more than that.");
                    res.core_init = 0xBAD0000Cu;
                    res.ngx_version_swept = true;
                    return true;
                }
            }
            else
            {
                pcab::logf("[lane]   declaring SDK version stamp 0x%016llX", stamp);
                res.core_init = (unsigned)p_init(pcab::NS_APPLICATION_ID, data_path, g_lane.dev,
                                                 &common, (NVSDK_NGX_Version)stamp);
            }
            pcab::logf("[lane]   core NVSDK_NGX_D3D12_Init(app_id=0x%016llX) -> 0x%08X (%s)",
                       (unsigned long long)pcab::NS_APPLICATION_ID, res.core_init,
                       ngx_result_name(res.core_init));
            if (res.core_init != 0x1u)
            {
                pcab::logf("[lane]   core Init is not Success. Nothing downstream can be formed on "
                           "an uninitialised session, so the lane stops here.");
                if (!opt.ngx_version_sweep)
                {
                    pcab::logf("[lane]   FAIL_OutOfDate here means the declared SDK version is "
                               "older than the installed runtime requires. Re-run with "
                               "--ngx-version-sweep to ask the runtime which stamp it accepts, "
                               "then pass that value with --ngx-version.");
                }
                // The lane RAN; it just did not succeed. That is a result.
                return true;
            }
            // The snippet's Init_Ext is given the SAME stamp: the two halves of one
            // session must agree about which SDK they are speaking.
            res.ngx_version_used = stamp;
        }

        // 5. the parameter block, from AllocateParameters. A block this lane OWNS.
        {
            NVSDK_NGX_Parameter *params = nullptr;
            res.alloc = (unsigned)p_alloc(&params);
            g_lane.params = params;
            pcab::logf("[lane]   NVSDK_NGX_D3D12_AllocateParameters -> 0x%08X (%s) params=%p",
                       res.alloc, ngx_result_name(res.alloc), (void *)params);
            if (res.alloc != 0x1u || params == nullptr)
            {
                pcab::logf("[lane]   AllocateParameters did not return Success with a non-null "
                           "block, so there is no creation contract to apply.");
                return true;
            }
        }

        // 6. NvAPI_Initialize, AFTER core Init and AFTER AllocateParameters.
        if (!pcab::nv_load_and_initialize())
        {
            res.error = "NVAPI could not be brought up - an environment result, not a lane result";
            pcab::logf("[nvapi]  NVAPI could not be brought up - this is an environment result, "
                       "and the run answers nothing");
            return false;
        }

        // 7. the architecture patch: target named, EVERY GPU cached with nothing
        //    hooked, then the patch installed over exactly one target entry.
        pcab::nv_set_arch_target(opt.adapter_vendor, opt.adapter_device);
        const bool arch_cached = pcab::arch_cache_real();
        const bool arch_installed = arch_cached ? pcab::arch_patch_install() : false;
        res.arch_patched = arch_cached && arch_installed;
        if (!res.arch_patched)
        {
            pcab::logf("[lane]   the NeuralScreen-compatible architecture patch is NOT installed. "
                       "The DLSSNR 310.8.0 runtime is expected to refuse the real Ada "
                       "architecture, so a failure downstream has a cause that is NOT the "
                       "question this worker exists to answer.");
        }
        pcab::logf("");

        // 8. the observational observer, as late as it can be while still being in
        //    place before the private runtime loads.
        pcab::observe_set_expected_device((void *)g_lane.dev);
        res.observer_installed = pcab::observe_install("nr-worker");
        pcab::logf("[nvapi]  observation installed = %s",
                   res.observer_installed ? "yes" : "NO");
        pcab::logf("");

        // 9. the exact private runtime, by path, after the patch and the observer.
        if (!pcab::ngx_load_snippet(opt.nr_dll))
        {
            res.error = "the snippet could not be loaded";
            return false;
        }
        res.snippet_loaded = true;

        HMODULE snip = pcab::ngx_snippet();
        pcab::pf_init_ext       p_sinit  = (pcab::pf_init_ext)pick(
            snip, "NVSDK_NGX_D3D12_Init_Ext", "snippet");
        pcab::pf_create_feature p_create = (pcab::pf_create_feature)pick(
            snip, "NVSDK_NGX_D3D12_CreateFeature", "snippet");
        if (p_sinit == nullptr || p_create == nullptr)
        {
            res.error = "the snippet does not export the entry points this lane needs";
            return false;
        }

        // 10. the snippet's own Reserved18 session, same app id, with our block.
        res.snip_init = (unsigned)p_sinit(pcab::NS_APPLICATION_ID, data_path, g_lane.dev,
                                          (NVSDK_NGX_Version)res.ngx_version_used,
                                          g_lane.params);
        pcab::logf("[lane]   snippet NVSDK_NGX_D3D12_Init_Ext(app_id=0x%016llX) -> 0x%08X (%s)",
                   (unsigned long long)pcab::NS_APPLICATION_ID, res.snip_init,
                   ngx_result_name(res.snip_init));
        if (res.snip_init != 0x1u)
        {
            pcab::logf("[lane]   snippet Init_Ext is not Success. Continuing anyway so the "
                       "CUDA-interop and CreateFeature results still report.");
        }

        // 11. Reset, then the exact NeuralScreen 640x360 creation contract.
        apply_reference_contract(g_lane.params);

        // 12. a private allocator, an open command list, a fence and an event.
        {
            HRESULT ahr = g_lane.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&g_lane.allocator));
            pcab::logf("[lane]   CreateCommandAllocator hr=0x%08X", (unsigned)ahr);
            if (FAILED(ahr)) { res.error = "CreateCommandAllocator failed"; return false; }

            ahr = g_lane.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               g_lane.allocator, nullptr,
                                               IID_PPV_ARGS(&g_lane.list));
            pcab::logf("[lane]   CreateCommandList hr=0x%08X (open and recording)", (unsigned)ahr);
            if (FAILED(ahr)) { res.error = "CreateCommandList failed"; return false; }

            ahr = g_lane.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_lane.fence));
            pcab::logf("[lane]   CreateFence hr=0x%08X", (unsigned)ahr);
            if (FAILED(ahr)) { res.error = "CreateFence failed"; return false; }

            g_lane.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            pcab::logf("[lane]   CreateEventW -> %p", (void *)g_lane.event);
            if (g_lane.event == nullptr) { res.error = "CreateEventW failed"; return false; }
        }

        // 13. THE QUESTION.
        {
            LARGE_INTEGER f{}, t0{}, t1{};
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            NVSDK_NGX_Handle *handle = nullptr;
            res.feature = (unsigned)p_create(g_lane.list, NVSDK_NGX_Feature_Reserved18,
                                             g_lane.params, &handle);
            QueryPerformanceCounter(&t1);
            res.feature_handle = (unsigned long long)(std::uintptr_t)handle;
            const double ms = (f.QuadPart > 0)
                ? ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart) : 0.0;
            pcab::logf("[lane]   CreateFeature(NVSDK_NGX_Feature_Reserved18) via the snippet -> "
                       "0x%08X (%s) handle=%p %ux%u elapsed=%.0fms",
                       res.feature, ngx_result_name(res.feature), (void *)handle,
                       NR_CTRL_W, NR_CTRL_H, ms);
        }
        pcab::logf("");

        // 14. the drain, exactly as MGPU does it: close, execute, signal, wait.
        {
            const HRESULT chr = g_lane.list->Close();
            pcab::logf("[drain]  Close hr=0x%08X", (unsigned)chr);
            if (SUCCEEDED(chr))
            {
                ID3D12CommandList *const lists[1] = { g_lane.list };
                g_lane.queue->ExecuteCommandLists(1, lists);
                const HRESULT shr = g_lane.queue->Signal(g_lane.fence, 1);
                pcab::logf("[drain]  Signal(fence,1) hr=0x%08X", (unsigned)shr);
                if (SUCCEEDED(shr))
                {
                    if (g_lane.fence->SetEventOnCompletion(1, g_lane.event) == S_OK)
                    {
                        const DWORD w = WaitForSingleObject(g_lane.event, 10000);
                        pcab::logf("[drain]  fence wait -> %s",
                                   (w == WAIT_OBJECT_0) ? "signalled" : "TIMED OUT");
                    }
                }
            }
            else
            {
                pcab::logf("[drain]  the list was not in a closable state; nothing is released");
            }
        }
        pcab::logf("");

        // 15. the observed private-call results.
        {
            const pcab::ObserveSummary obs = pcab::observe_summary();
            res.descriptor_status = obs.real_descriptor_status;
            res.cumodule_status = obs.real_cumodule_status;
            res.first_blob_size = obs.first_blob_size;
            pcab::logf("[cuda]   GetCudaIndependentDescriptorObject (0x0DDAC234): real_calls=%u "
                       "probes=%u real_status=%d", obs.descriptor_calls, obs.descriptor_probes,
                       obs.real_descriptor_status);
            pcab::logf("[cuda]   CreateCuModule (0xAD1A677D): real_calls=%u probes=%u "
                       "real_status=%d", obs.cumodule_calls, obs.cumodule_probes,
                       obs.real_cumodule_status);
            pcab::logf("[cuda]   first CreateCuModule blob size = %llu bytes (the blob itself is "
                       "never read)", obs.first_blob_size);
        }
        pcab::logf("");

        (void)release_on_exit;
        return true;
    }
}
