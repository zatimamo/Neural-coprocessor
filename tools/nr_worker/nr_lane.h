// ============================================================================
// MGPU NR worker - THE PROVEN INITIALIZATION LANE.
//
// WHAT "PROVEN" MEANS HERE
//     This is the sequence AutoLab established as the one that succeeds on the
//     RTX 4070 in a process with no second-adapter state:
//
//         Descriptor = 0    CuModule = 0    blob = 3944768    Reserved18 = Success
//
//     It is not a new interpretation of that sequence. The NVAPI layer, the
//     architecture patch, the private-call observation and the NGX module
//     handling are the PROCESSCONTEXT-AB translation units, compiled verbatim
//     into this worker:
//
//         ../processcontext_ab/nv.cpp        NVAPI, arch cache/patch, log, hash
//         ../processcontext_ab/observe.cpp   the two private CUDA-interop calls
//
//     Those files are frozen by the processcontext-ab workflow. This worker
//     consumes them; it does not fork them, and a CI gate fails if either tree's
//     copy of them changes.
//
// WHY THE LANE LIVES IN THE WORKER AND NOT IN THE GAME
//     SPLITPROCESS_ISOLATION proved that the failure follows the *active D3D12
//     state of the second adapter inside the process that runs the lane*. So the
//     RTX 4070 device, the NGX session and the Reserved18 feature must never be
//     created inside Cyberpunk.exe: they are created here, in mgpu_nr_worker.exe,
//     and the game talks to them over a control pipe and shared GPU memory.
//
// THE ORDER IS PART OF THE REPRODUCTION, NOT AN IMPLEMENTATION DETAIL
//     1.  select the adapter by PCI identity (never by ordinal)
//     2.  D3D12CreateDevice FL_11_0 + a DIRECT queue
//     3.  load the NGX core
//     4.  core NVSDK_NGX_D3D12_Init          app id 0x1000000
//     5.  NVSDK_NGX_D3D12_AllocateParameters a private block this process owns
//     6.  NvAPI_Initialize                   ^ after core Init, NeuralScreen's order
//     7.  nv_set_arch_target + arch_cache_real + install the arch patch
//     8.  install the observational QueryInterface observer
//     9.  load the exact nvngx_dlssnr.dll
//     10. snippet NVSDK_NGX_D3D12_Init_Ext   app id 0x1000000
//     11. params->Reset() and the 640x360 creation contract
//     12. CreateFeature(NVSDK_NGX_Feature_Reserved18)
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace nr
{
    //: The RTX 4070, by PCI identity. The same id the diagnostic uses.
    const unsigned NR_VENDOR_NVIDIA = 0x10DEu;
    const unsigned NR_DEVICE_RTX_4070 = 0x2786u;

    //: The proven control geometry, unchanged: NeuralScreen creates Reserved18
    //: at 640x360 and the worker reproduces that rather than inventing a size.
    const unsigned NR_CTRL_W = 640u;
    const unsigned NR_CTRL_H = 360u;

    //: The blob size the working runtime produces. A different value means the
    //: runtime, the architecture patch or the device is not the proven one.
    const unsigned long long NR_EXPECTED_BLOB = 3944768ull;

    struct LaneOptions
    {
        const wchar_t *nr_dll = nullptr;        // the exact nvngx_dlssnr.dll
        const wchar_t *data_path = nullptr;     // dirname(--nr-dll)
        const char    *expected_nr_sha256 = nullptr;   // empty = do not gate on it
        unsigned       adapter_vendor = NR_VENDOR_NVIDIA;
        unsigned       adapter_device = NR_DEVICE_RTX_4070;
        bool           require_arch_patch = true;
    };

    struct LaneResult
    {
        //: Which process this is, for the record: the worker's own pid.
        unsigned long pid = 0;

        bool device_ok = false;
        bool core_loaded = false;
        bool snippet_loaded = false;
        bool arch_patched = false;
        bool observer_installed = false;

        unsigned core_init = 0;
        unsigned alloc = 0;
        unsigned snip_init = 0;
        unsigned feature = 0;                   // the CreateFeature(Reserved18) result

        int descriptor_status = -12345;         // -12345 = not observed
        int cumodule_status = -12345;
        unsigned long long first_blob_size = 0;
        unsigned long long feature_handle = 0;

        unsigned adapter_vendor = 0;
        unsigned adapter_device = 0;
        unsigned luid_high = 0;
        unsigned luid_low = 0;
        wchar_t  adapter_name[128] = L"";

        //: The full eight-condition result, evaluated here so that a caller has
        //: one boolean to check rather than a private copy of the rules.
        bool proven() const
        {
            return core_init == 0x1u && alloc == 0x1u && snip_init == 0x1u &&
                   descriptor_status == 0 && cumodule_status == 0 &&
                   first_blob_size == NR_EXPECTED_BLOB && feature == 0x1u &&
                   feature_handle != 0;
        }

        //: The conditions that did NOT hold, in words, for the log.
        std::string failing_conditions() const;

        std::string error;                      // why the lane could not run at all
    };

    //: Run the lane. Returns false only when the lane could not be RUN (no
    //: adapter, no device, no core, a refused DLL); a lane that ran and failed
    //: to create Reserved18 returns true with `proven()` false, because that is
    //: a RESULT rather than an inability to measure.
    bool lane_run(const LaneOptions &opt, LaneResult &res);

    //: The device and queue the lane left alive, for the transport that will use
    //: them. Null before lane_run succeeded.
    ID3D12Device *lane_device();
    ID3D12CommandQueue *lane_queue();

    //: Deliberately not a teardown: the lane's session stays alive for as long as
    //: this process does, exactly as the proven diagnostic does. It exists so the
    //: shutdown path can SAY that, rather than leave it implied.
    void lane_note_shutdown();
}
