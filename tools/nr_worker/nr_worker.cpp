// ============================================================================
// mgpu_nr_worker.exe - PROCESS A of the validated out-of-process NR architecture.
//
// WHY THIS PROCESS EXISTS
//     SPLITPROCESS_ISOLATION proved the failure follows the second adapter's
//     active D3D12 state INSIDE the process that runs the neural lane:
//
//         same process, dual-active   Descriptor -1   CuModule -1   Reserved18 BAD00002
//         split process               Descriptor  0   CuModule  0   Reserved18 Success
//
//     So the RTX 4070 device, the NGX core, the architecture patch, the DLSSNR
//     runtime and the Reserved18 session live HERE, and never inside
//     Cyberpunk.exe. The game side will own capture and transport only.
//
// WHAT THIS BUILD DOES
//     --prove    run the lane and report the eight conditions. This is the
//                Reserved18 proof the milestone asks for.
//     --serve    run the lane, then serve the v1 control protocol on
//                \\.\pipe\MGPU_NR_<pid> until SHUTDOWN, the deadline, or the peer
//                going away. Pixel data never crosses this channel.
//     --protocol-only
//                serve the protocol WITHOUT touching NVAPI, NGX or D3D12, so the
//                handshake can be exercised on a machine with no NVIDIA adapter
//                - which is what CI does on every build.
//
// WHAT IT DOES NOT DO YET
//     It does not open a shared surface or copy a pixel. FRAME_SUBMIT is
//     answered with FRAME_COMPLETE and E_NOTIMPL, which is the honest answer
//     until the transport is wired to this process. The transport prototype is
//     nr_transport_bench.exe and is proven separately, on two real adapters.
//
// SAFETY
//     Every pipe operation has a deadline. A silent, dead or protocol-mismatched
//     peer turns into a logged disconnect and a clean exit; it can never hang
//     this process, and this process can never hang the game, because the game
//     side is not blocked on anything here.
// ============================================================================

#include "nr_lane.h"
#include "nr_pipe.h"
#include "nr_proto.h"
#include "nr_slots.h"

#include "../processcontext_ab/nv.h"

#include <windows.h>
// WIN32_LEAN_AND_MEAN keeps <shellapi.h> out of <windows.h>, and
// CommandLineToArgvW lives there. The command line is read as WIDE text because
// the arguments name filesystem paths, so the narrow argv is not usable.
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace
{
    //: The NeuralScreen v1.15.0 reference runtime the proven lane runs against.
    //: The same build AutoLab hash-gates its PROCESSCONTEXT runs on. It is the
    //: DEFAULT rather than a hard requirement: --nr-dll-sha256 states a different
    //: expectation explicitly, and "" disables the gate on purpose.
    const char *NS_REFERENCE_NR_SHA256 =
        "dcc0dc2414aedec4a8e084647070383be068554042587180c20c784d4772d36f";

    struct Args
    {
        const wchar_t *nr_dll = nullptr;
        const char    *nr_sha256 = NS_REFERENCE_NR_SHA256;
        const char    *log_name = "nr-worker";
        const char    *pipe = nullptr;          // full \\.\pipe\... name, or null for the pid form
        bool           prove = false;
        bool           serve = false;
        bool           protocol_only = false;
        unsigned       accept_seconds = 30;
        unsigned       serve_seconds = 0;       // 0 = until SHUTDOWN or disconnect
        bool           help = false;
        unsigned long long ngx_version = 0;     // 0 = the header's default stamp
        bool           ngx_version_sweep = false;
    };

    void usage()
    {
        std::fprintf(stderr,
            "mgpu_nr_worker - the out-of-process RTX 4070 neural lane.\n"
            "\n"
            "  --nr-dll <path>          the exact nvngx_dlssnr.dll (required for the lane)\n"
            "  --nr-dll-sha256 <hex>    the runtime this build insists on; \"\" disables the gate\n"
            "  --ngx-version <hex>      the NGX SDK version stamp this application declares.\n"
            "                           The driver refuses a stamp older than it requires with\n"
            "                           0xBAD0000C FAIL_OutOfDate; the pinned headers are old,\n"
            "                           so a current driver needs this set.\n"
            "  --ngx-version-sweep      ask the runtime which stamp it accepts, by trying a\n"
            "                           ladder of real NGX versions ascending and keeping the\n"
            "                           lowest one that returns Success. Run once, then pass\n"
            "                           the answer with --ngx-version.\n"
            "  --prove                  run the lane and report the eight conditions, then exit\n"
            "  --serve                  run the lane, then serve the control protocol\n"
            "  --protocol-only          serve WITHOUT touching NVAPI, NGX or D3D12\n"
            "  --pipe <name>            use this pipe name (default \\\\.\\pipe\\MGPU_NR_<pid>)\n"
            "  --accept-seconds <n>     how long to wait for the game side (default 30)\n"
            "  --serve-seconds <n>      cap on serving; 0 = until SHUTDOWN or disconnect\n"
            "  --log-name <name>        the log file is context-<name>.log (default nr-worker)\n"
            "\n"
            "The lane reports its result as a machine-readable line:\n"
            "  NR-WORKER-RESULT proven=YES|NO core_init=... alloc=... snip_init=...\n"
            "                   descriptor=... cumodule=... blob=... reserved18=... handle=...\n");
    }

    bool parse_args(int argc, wchar_t **argv, Args &a, std::string &err)
    {
        for (int i = 1; i < argc; ++i)
        {
            const wchar_t *w = argv[i];
            auto next_w = [&](const wchar_t **out) {
                if (i + 1 >= argc) return false;
                *out = argv[++i];
                return true;
            };
            if (std::wcscmp(w, L"--nr-dll") == 0)
            {
                if (!next_w(&a.nr_dll)) { err = "--nr-dll needs a path"; return false; }
            }
            else if (std::wcscmp(w, L"--prove") == 0)         a.prove = true;
            else if (std::wcscmp(w, L"--serve") == 0)         a.serve = true;
            else if (std::wcscmp(w, L"--protocol-only") == 0) a.protocol_only = true;
            else if (std::wcscmp(w, L"--help") == 0 || std::wcscmp(w, L"-h") == 0) a.help = true;
            else
            {
                // The narrow-valued options are converted once, here, so that the
                // rest of the program deals in char* only.
                char narrow[512] = "";
                WideCharToMultiByte(CP_UTF8, 0, w, -1, narrow, sizeof narrow, nullptr, nullptr);
                if (std::strcmp(narrow, "--nr-dll-sha256") == 0 && i + 1 < argc)
                {
                    const wchar_t *v = argv[++i];
                    char vn[128] = "";
                    WideCharToMultiByte(CP_UTF8, 0, v, -1, vn, sizeof vn, nullptr, nullptr);
                    a.nr_sha256 = (vn[0] == '\0') ? "" : NS_REFERENCE_NR_SHA256;
                    if (vn[0] != '\0')
                    {
                        static std::string store;
                        store = vn;
                        a.nr_sha256 = store.c_str();
                    }
                }
                else if (std::strcmp(narrow, "--pipe") == 0 && i + 1 < argc)
                {
                    const wchar_t *v = argv[++i];
                    static std::string store;
                    char vn[512] = "";
                    WideCharToMultiByte(CP_UTF8, 0, v, -1, vn, sizeof vn, nullptr, nullptr);
                    store = vn;
                    a.pipe = store.c_str();
                }
                else if (std::strcmp(narrow, "--log-name") == 0 && i + 1 < argc)
                {
                    const wchar_t *v = argv[++i];
                    static std::string store;
                    char vn[128] = "";
                    WideCharToMultiByte(CP_UTF8, 0, v, -1, vn, sizeof vn, nullptr, nullptr);
                    store = vn;
                    a.log_name = store.c_str();
                }
                else if (std::strcmp(narrow, "--ngx-version") == 0 && i + 1 < argc)
                {
                    a.ngx_version = std::wcstoull(argv[++i], nullptr, 0);
                }
                else if (std::strcmp(narrow, "--ngx-version-sweep") == 0)
                {
                    a.ngx_version_sweep = true;
                }
                else if (std::strcmp(narrow, "--accept-seconds") == 0 && i + 1 < argc)
                {
                    a.accept_seconds = (unsigned)std::wcstoul(argv[++i], nullptr, 10);
                }
                else if (std::strcmp(narrow, "--serve-seconds") == 0 && i + 1 < argc)
                {
                    a.serve_seconds = (unsigned)std::wcstoul(argv[++i], nullptr, 10);
                }
                else
                {
                    err = std::string("unrecognised argument \"") + narrow + "\"";
                    return false;
                }
            }
        }
        return true;
    }

    void print_lane_report(const nr::LaneResult &r)
    {
        pcab::logf("[worker] adapter      = \"%ls\" vendor=0x%04X device=0x%04X "
                   "luid=%08lX:%08lX",
                   r.adapter_name, r.adapter_vendor, r.adapter_device,
                   (unsigned long)r.luid_high, (unsigned long)r.luid_low);
        pcab::logf("[worker] descriptor   = %d", r.descriptor_status);
        pcab::logf("[worker] CuModule     = %d  first blob = %llu",
                   r.cumodule_status, r.first_blob_size);
        pcab::logf("[worker] Reserved18   = 0x%08X handle=%s",
                   r.feature, (r.feature_handle != 0) ? "non-zero" : "NULL");
        pcab::logf("[worker] core Init=0x%08X alloc=0x%08X snippet Init_Ext=0x%08X "
                   "arch_patched=%s observer=%s",
                   r.core_init, r.alloc, r.snip_init, r.arch_patched ? "yes" : "NO",
                   r.observer_installed ? "yes" : "NO");
        pcab::logf("[worker] NGX SDK version declared = 0x%016llX%s",
                   r.ngx_version_used,
                   r.ngx_version_swept ? " (found by the sweep)" : "");

        // ONE machine-readable line, so a caller does not have to parse prose.
        pcab::logf("NR-WORKER-RESULT proven=%s core_init=0x%08X alloc=0x%08X "
                   "snip_init=0x%08X descriptor=%d cumodule=%d blob=%llu reserved18=0x%08X "
                   "handle=0x%llX",
                   r.proven() ? "YES" : "NO", r.core_init, r.alloc, r.snip_init,
                   r.descriptor_status, r.cumodule_status, r.first_blob_size, r.feature,
                   r.feature_handle);
    }

    //: The control loop. Returns the process exit code.
    int serve(const Args &a, const wchar_t *pipe_name, const nr::LaneResult *lane)
    {
        nr::Pipe srv;
        std::string err;
        pcab::logf("[ipc]    listening on %ls (protocol version %u, one client)",
                   pipe_name, (unsigned)nr::PROTO_VERSION);

        const unsigned accept_ms = (a.accept_seconds == 0u) ? 60000u : a.accept_seconds * 1000u;
        if (!srv.listen_and_accept(pipe_name, accept_ms, err))
        {
            pcab::logf("WORKER_FAILED reason=IPC_NO_CLIENT detail=\"%s\"", err.c_str());
            pcab::logf("[ipc]    no game side connected within %u ms - exiting rather than "
                       "holding a neural session for nobody", accept_ms);
            return 2;
        }
        pcab::logf("[ipc]    a client connected");

        nr::SlotRing ring(nr::FRAME_SLOT_COUNT);
        const std::uint64_t deadline = (a.serve_seconds == 0u)
            ? 0ull : (nr::now_ms() + (std::uint64_t)a.serve_seconds * 1000ull);
        int rc = 0;

        for (;;)
        {
            const unsigned wait = (a.serve_seconds == 0u) ? 3600000u : 1000u;

            // Every kind is read through the same fixed-size buffer - Error is the
            // largest message v1 defines - and then re-read through the struct the
            // kind actually is. header_ok is what makes that safe: the sender's
            // header must declare exactly this build's size for that kind, so a
            // memcpy of sizeof(that struct) can never read past what arrived.
            nr::Error inbound;
            const nr::IoResult r = srv.read_message(inbound, wait, err);
            if (r == nr::IoResult::TIMEOUT)
            {
                if (deadline != 0ull && nr::now_ms() >= deadline)
                {
                    pcab::logf("[ipc]    the serving deadline passed; stopping");
                    break;
                }
                continue;
            }
            if (r != nr::IoResult::OK)
            {
                pcab::logf("[ipc]    %s: %s", nr::io_result_name(r), err.c_str());
                pcab::logf("[ipc]    the control channel is gone - the game side must disable "
                           "neural mode and continue without it");
                rc = 3;
                break;
            }

            const nr::Kind kind = (nr::Kind)inbound.h.kind;
            pcab::logf("[ipc]    <- %s frame_id=%u size=%u", nr::kind_name(kind),
                       inbound.h.frame_id, inbound.h.size);

            if (kind == nr::Kind::SHUTDOWN)
            {
                // Re-read through the struct this kind actually IS. `inbound` is
                // only a buffer - the largest message v1 defines - and its header
                // has already proved that the real size is sizeof(Shutdown).
                nr::Shutdown sd;
                std::memcpy(&sd, &inbound, sizeof(sd));
                pcab::logf("[ipc]    SHUTDOWN requested (reason=%u)", sd.reason);
                break;
            }
            if (kind == nr::Kind::HELLO)
            {
                nr::Hello hello;
                std::memcpy(&hello, &inbound, sizeof(hello));
                // `inbound` was zeroed before the read, so the bytes past the
                // HELLO the peer sent are zero rather than stale stack.
                if (!nr::header_ok(hello.h, nr::Kind::HELLO))
                {
                    nr::Error e;
                    nr::init(e, nr::Kind::ERROR_MESSAGE, inbound.h.frame_id);
                    e.stage = 1;
                    e.hr = (std::int32_t)0x8007000DL;   // E_INVALIDARG
                    nr::set_text(e.text, nr::PROTO_TEXT_MAX, nr::header_problem(hello.h, nr::Kind::HELLO));
                    srv.write_message(e, 2000, err);
                    pcab::logf("[ipc]    refused a malformed HELLO: %s",
                               nr::header_problem(hello.h, nr::Kind::HELLO));
                    rc = 4;
                    break;
                }
                if (hello.client_protocol_version != nr::PROTO_VERSION)
                {
                    nr::Error e;
                    nr::init(e, nr::Kind::ERROR_MESSAGE, inbound.h.frame_id);
                    e.stage = 2;
                    e.hr = (std::int32_t)0x8007000DL;
                    nr::set_text(e.text, nr::PROTO_TEXT_MAX,
                                 "the client speaks a different protocol version");
                    srv.write_message(e, 2000, err);
                    pcab::logf("[ipc]    protocol version mismatch: client=%u worker=%u",
                               hello.client_protocol_version, (unsigned)nr::PROTO_VERSION);
                    rc = 4;
                    break;
                }
                pcab::logf("[ipc]    HELLO from pid=%u name=\"%s\" build=%u wants_transport=%u",
                           hello.client_pid, hello.client_name, hello.client_build,
                           hello.wants_transport);

                nr::WorkerReady ready;
                nr::init(ready, nr::Kind::WORKER_READY, inbound.h.frame_id);
                if (lane != nullptr)
                {
                    ready.reserved18 = (std::int32_t)lane->feature;
                    ready.descriptor_status = (std::int32_t)lane->descriptor_status;
                    ready.cumodule_status = (std::int32_t)lane->cumodule_status;
                    ready.adapter_vendor = lane->adapter_vendor;
                    ready.adapter_device = lane->adapter_device;
                    ready.luid_high = lane->luid_high;
                    ready.luid_low = lane->luid_low;
                    ready.reserved18_handle_nonzero = (lane->feature_handle != 0) ? 1u : 0u;
                    ready.first_blob_size = lane->first_blob_size;
                }
                else
                {
                    // -1 is "the lane was not run", which is a different statement
                    // from "the lane ran and Reserved18 failed".
                    ready.reserved18 = -1;
                    ready.descriptor_status = -12345;
                    ready.cumodule_status = -12345;
                }
                nr::set_text(ready.worker_name, nr::PROTO_NAME_MAX,
                             (lane != nullptr) ? "mgpu_nr_worker:lane" : "mgpu_nr_worker:protocol-only");
                if (srv.write_message(ready, 2000, err) != nr::IoResult::OK)
                {
                    pcab::logf("[ipc]    could not send WORKER_READY: %s", err.c_str());
                    rc = 3;
                    break;
                }
                pcab::logf("[ipc]    -> WORKER_READY reserved18=%d descriptor=%d cumodule=%d "
                           "blob=%llu", ready.reserved18, ready.descriptor_status,
                           ready.cumodule_status, ready.first_blob_size);
                continue;
            }

            if (kind == nr::Kind::CONFIG)
            {
                nr::Config cfg;
                std::memcpy(&cfg, &inbound, sizeof(cfg));
                const nr::InputSlot which = (nr::InputSlot)cfg.input_slot;
                if (cfg.input_slot >= (std::uint32_t)nr::InputSlot::COUNT)
                {
                    nr::Error e;
                    nr::init(e, nr::Kind::ERROR_MESSAGE, inbound.h.frame_id);
                    e.stage = 3;
                    e.hr = (std::int32_t)0x8007000DL;
                    nr::set_text(e.text, nr::PROTO_TEXT_MAX, "CONFIG names an unknown input slot");
                    srv.write_message(e, 2000, err);
                    pcab::logf("[ipc]    CONFIG refused: input slot %u is not one of four",
                               cfg.input_slot);
                    rc = 4;
                    break;
                }
                pcab::logf("[ipc]    CONFIG %s: %ux%u format=%u row_pitch=%u slice_pitch=%u "
                           "offset=%u shared=\"%s\" frame_slots=%u",
                           nr::input_slot_metadata_name(which), cfg.width, cfg.height,
                           cfg.dxgi_format, cfg.row_pitch, cfg.slice_pitch,
                           cfg.resource_offset, cfg.shared_name, cfg.frame_slots);
                pcab::logf("[ipc]    (metadata only - no pixel data crosses this channel, ever)");
                continue;
            }

            if (kind == nr::Kind::FRAME_SUBMIT)
            {
                nr::FrameSubmit sub;
                std::memcpy(&sub, &inbound, sizeof(sub));
                nr::FrameComplete done;
                nr::init(done, nr::Kind::FRAME_COMPLETE, sub.h.frame_id);
                done.slot = sub.slot;
                done.egress_fence_value = 0;

                if (sub.slot >= ring.count())
                {
                    done.hr = (std::int32_t)0x8007000DL;      // E_INVALIDARG
                    pcab::logf("[ipc]    FRAME_SUBMIT refused: frame slot %u does not exist",
                               sub.slot);
                }
                else if (!ring.begin_game_copy(sub.slot, sub.h.frame_id, sub.ingress_fence_value))
                {
                    // THE RING GUARD: the game submitted a slot the worker still
                    // owns. Refusing here is what stops the two devices from
                    // copying into the same surface at the same time.
                    done.hr = (std::int32_t)0x8007000BL;      // E_FAIL
                    pcab::logf("[ipc]    FRAME_SUBMIT refused: %s", ring.last_refusal().c_str());
                }
                else
                {
                    ring.set_geometry(sub.slot, sub.width, sub.height, sub.dxgi_format,
                                      sub.row_pitch, sub.slice_pitch, sub.resource_offset);
                    ring.game_copy_done(sub.slot, sub.ingress_fence_value);
                    ring.worker_begin(sub.slot, sub.ingress_fence_value);

                    // NO TRANSPORT YET, and the reply says so rather than
                    // pretending the frame was processed.
                    done.hr = (std::int32_t)0x80004001L;      // E_NOTIMPL
                    ring.worker_done(sub.slot, sub.ingress_fence_value);
                    ring.begin_game_copyback(sub.slot);
                    ring.release(sub.slot, sub.ingress_fence_value, sub.ingress_fence_value);

                    pcab::logf("[ipc]    FRAME_SUBMIT frame_id=%u slot=%u %ux%u format=%u "
                               "mask=0x%X -> E_NOTIMPL: the transport is not wired to the worker "
                               "yet, so no pixels moved and no fence was signalled",
                               sub.h.frame_id, sub.slot, sub.width, sub.height,
                               sub.dxgi_format, sub.input_mask);
                }

                if (srv.write_message(done, 2000, err) != nr::IoResult::OK)
                {
                    pcab::logf("[ipc]    could not send FRAME_COMPLETE: %s", err.c_str());
                    rc = 3;
                    break;
                }
                continue;
            }

            // Anything else is a protocol error, and a mis-framed message on a
            // channel that will carry shared-memory handles is fatal.
            {
                nr::Error e;
                nr::init(e, nr::Kind::ERROR_MESSAGE, inbound.h.frame_id);
                e.stage = 4;
                e.hr = (std::int32_t)0x8007000DL;
                nr::set_text(e.text, nr::PROTO_TEXT_MAX,
                             "this message kind is not valid in this direction");
                srv.write_message(e, 2000, err);
                pcab::logf("[ipc]    ERROR: %s is not valid here", nr::kind_name(kind));
                rc = 4;
                break;
            }
        }

        pcab::logf("[ipc]    session over: %u frame(s) started, %u completed, %u illegal "
                   "transition(s) refused, %u slot(s) still in flight",
                   ring.frames_started(), ring.frames_completed(), ring.illegal_transitions(),
                   ring.in_flight_count());
        if (!ring.all_free())
        {
            pcab::logf("[ipc]    NOTE: slots are still in flight. That is what a peer that "
                       "vanished mid-frame leaves behind, and it is reported rather than hidden.");
        }
        return rc;
    }
}

int main(int argc, char **argv)
{
    (void)argv;

    // The command line is read as wide text: the arguments name filesystem paths.
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv == nullptr)
    {
        std::fprintf(stderr, "mgpu_nr_worker: CommandLineToArgvW failed\n");
        return 1;
    }

    Args args;
    std::string err;
    if (!parse_args(wargc, wargv, args, err))
    {
        std::fprintf(stderr, "mgpu_nr_worker: %s\n", err.c_str());
        usage();
        LocalFree(wargv);
        return 1;
    }
    if (args.help)
    {
        usage();
        LocalFree(wargv);
        return 0;
    }
    if (!args.prove && !args.serve)
    {
        std::fprintf(stderr, "mgpu_nr_worker: choose --prove or --serve "
                             "(or both: --serve runs the lane first)\n");
        usage();
        LocalFree(wargv);
        return 1;
    }

    pcab::log_open(args.log_name);

    // The lane is run first, so that WORKER_READY is never printed by a worker
    // whose Reserved18 session does not exist.
    nr::LaneResult lane;
    bool lane_ran = false;
    if (!args.protocol_only)
    {
        nr::LaneOptions opt;
        opt.nr_dll = args.nr_dll;
        opt.expected_nr_sha256 = args.nr_sha256;
        opt.ngx_version = args.ngx_version;
        opt.ngx_version_sweep = args.ngx_version_sweep;
        lane_ran = nr::lane_run(opt, lane);
        if (!lane_ran)
        {
            pcab::logf("WORKER_FAILED reason=LANE_NOT_RUN detail=\"%s\"", lane.error.c_str());
            pcab::logf("[worker] the lane could not be run, so this worker cannot serve a "
                       "neural frame. The game side must disable neural mode and continue.");
            pcab::log_close();
            LocalFree(wargv);
            return 1;
        }
        if (!lane.proven())
        {
            print_lane_report(lane);
            pcab::logf("WORKER_FAILED reason=RESERVED18_NOT_PROVEN failing=\"%s\"",
                       lane.failing_conditions().c_str());
            pcab::logf("[worker] Reserved18 was not created by the proven path, so this worker "
                       "is NOT ready and will not claim to be.");
            pcab::log_close();
            LocalFree(wargv);
            return 1;
        }
        // The token goes out FIRST, because a parent gates on it, and everything
        // below is the evidence for the claim it just made.
        pcab::logf("WORKER_READY");
        print_lane_report(lane);
    }
    else
    {
        pcab::logf("[worker] --protocol-only: NVAPI, NGX, D3D12 and the Reserved18 session are "
                   "NOT touched by this run. This proves the CONTROL CHANNEL and nothing else, "
                   "and the WORKER_READY it sends over the pipe carries reserved18=-1 so no "
                   "client can mistake it for a neural session.");
        pcab::logf("WORKER_READY");
    }
    std::fflush(stdout);

    if (!args.serve)
    {
        if (lane_ran) nr::lane_note_shutdown();
        pcab::log_close();
        LocalFree(wargv);
        return 0;
    }

    wchar_t pipe_name[256];
    if (args.pipe != nullptr)
    {
        MultiByteToWideChar(CP_UTF8, 0, args.pipe, -1, pipe_name, 256);
    }
    else
    {
        nr::pipe_name_for_pid((std::uint32_t)GetCurrentProcessId(), pipe_name, 256);
    }

    const int rc = serve(args, pipe_name, args.protocol_only ? nullptr : &lane);
    if (lane_ran) nr::lane_note_shutdown();
    pcab::log_close();
    LocalFree(wargv);
    return rc;
}
