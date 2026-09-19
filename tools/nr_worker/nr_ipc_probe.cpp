// ============================================================================
// nr_ipc_probe.exe - the client side of the control protocol.
//
// WHAT IT IS FOR
//     Two things, in this order:
//
//       1. It is how CI exercises the REAL worker: the workflow starts
//          mgpu_nr_worker.exe --serve --protocol-only and runs this probe
//          against it, so every build proves the handshake between the two real
//          binaries across a real process boundary - on a runner with no NVIDIA
//          adapter.
//
//       2. It is the shape the game-side add-on will use: connect, HELLO, read
//          WORKER_READY, describe the four input surfaces, submit frames, wait
//          for FRAME_COMPLETE, SHUTDOWN. The game side will replace the pixel
//          half with shared cross-adapter surfaces; the control half is this.
//
// WHAT IT REFUSES TO DO
//     Send pixel data. There is no payload here beyond the fixed structs, and
//     the frame path is a submit/complete ping-pong carrying ids and HRESULTs.
//
// IT REPORTS A CONTROL-CHANNEL LATENCY
//     Per-message round trip, median/p95/max, because a control channel that is
//     fast on average and stalls at p95 is a stutter generator. This is NOT the
//     transport latency; that is nr_transport_bench.exe's job.
// ============================================================================

#include "nr_json.h"
#include "nr_pipe.h"
#include "nr_proto.h"
// InputSlot and its names live here. The four inputs are a SLOT concept, not a
// protocol one: the protocol carries the number, nr_slots.h gives it a name.
#include "nr_slots.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct Args
    {
        const char *pipe = nullptr;          // \\.\pipe\... name; null = the default pattern
        std::uint32_t pid_of_worker = 0;     // used when --pipe is not given
        unsigned     frames = 3;
        bool         expect_protocol_only = false;
        const char  *json = "nr-ipc-probe.json";
        bool         help = false;
    };

    void usage()
    {
        std::fprintf(stderr,
            "nr_ipc_probe - the client side of the MGPU NR control protocol.\n"
            "\n"
            "  --pipe <name>        the worker's pipe, e.g. \\\\.\\pipe\\MGPU_NR_1234\n"
            "  --worker-pid <n>     build the default pipe name from this pid\n"
            "  --frames <n>         how many frames to submit (default 3)\n"
            "  --expect-protocol-only\n"
            "                       accept reserved18=-1 (a worker started with\n"
            "                       --protocol-only has no neural session, on purpose)\n"
            "  --json <path>        write the report (default nr-ipc-probe.json)\n");
    }

    double now_ms()
    {
        LARGE_INTEGER f{}, c{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        if (f.QuadPart <= 0) return 0.0;
        return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
    }
}

int main(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        const char *s = argv[i];
        if (std::strcmp(s, "--pipe") == 0 && i + 1 < argc)              a.pipe = argv[++i];
        else if (std::strcmp(s, "--worker-pid") == 0 && i + 1 < argc)   a.pid_of_worker = (std::uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(s, "--frames") == 0 && i + 1 < argc)       a.frames = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(s, "--json") == 0 && i + 1 < argc)         a.json = argv[++i];
        else if (std::strcmp(s, "--expect-protocol-only") == 0)         a.expect_protocol_only = true;
        else if (std::strcmp(s, "--help") == 0 || std::strcmp(s, "-h") == 0) a.help = true;
        else
        {
            std::fprintf(stderr, "nr_ipc_probe: unrecognised argument \"%s\"\n", s);
            usage();
            return 2;
        }
    }
    if (a.help) { usage(); return 0; }
    if (a.frames == 0) a.frames = 1;
    if (a.frames > 300) a.frames = 300;

    wchar_t pipe_name[256] = L"";
    if (a.pipe != nullptr)
    {
        // The name arrives as ASCII; the pipe namespace is wide.
        for (int i = 0; i < 255 && a.pipe[i] != '\0'; ++i) pipe_name[i] = (wchar_t)a.pipe[i];
    }
    else
    {
        nr::pipe_name_for_pid(a.pid_of_worker, pipe_name, 256);
    }

    std::printf("nr_ipc_probe -> %ls\n", pipe_name);
    std::fflush(stdout);

    nr::Json report;
    report.begin_object();
    report.kv("tool", "nr_ipc_probe");
    report.kv("protocol_version", (std::uint64_t)nr::PROTO_VERSION);
    report.key("steps");
    report.begin_array();

    int rc = 0;
    //: A worker that was started with --protocol-only has no neural session ON
    //: PURPOSE. That is a different statement from "the session failed", so it is
    //: tracked separately and only downgrades an otherwise clean run.
    bool reserved18_not_a_session = false;
    std::uint32_t steps_ok = 0;
    std::uint32_t steps_failed = 0;
    std::uint32_t frames_completed = 0;
    std::int32_t reserved18 = -999;
    std::vector<double> control_rtt;

    auto step = [&](const char *name, bool ok, const std::string &detail) {
        if (ok) ++steps_ok; else ++steps_failed;
        report.begin_object();
        report.kv("name", name);
        report.kv("ok", ok);
        if (!detail.empty()) report.kv("detail", detail.c_str());
        report.end_object();
        std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                    detail.empty() ? "" : " :: ", detail.c_str());
        std::fflush(stdout);
    };

    nr::Pipe cli;
    std::string err;

    if (!cli.connect(pipe_name, 10000, err))
    {
        step("connect", false, err);
        rc = 3;
    }
    else
    {
        step("connect", true, "");

        // ---- HELLO ----------------------------------------------------------
        nr::Hello hello;
        nr::init(hello, nr::Kind::HELLO, 0);
        hello.client_pid = (std::uint32_t)GetCurrentProcessId();
        hello.client_protocol_version = nr::PROTO_VERSION;
        hello.client_build = 1;
        hello.wants_transport = 0;      // this probe moves no pixels
        nr::set_text(hello.client_name, nr::PROTO_NAME_MAX, "nr_ipc_probe");

        double t0 = now_ms();
        nr::IoResult r = cli.write_message(hello, 5000, err);
        nr::WorkerReady ready{};
        if (r == nr::IoResult::OK)
        {
            r = cli.read_message(ready, 10000, err);
        }
        control_rtt.push_back(now_ms() - t0);

        if (r != nr::IoResult::OK || !nr::header_ok(ready.h, nr::Kind::WORKER_READY))
        {
            step("hello->worker_ready", false,
                 std::string(nr::io_result_name(r)) + " " + err +
                     (r == nr::IoResult::OK ? nr::header_problem(ready.h, nr::Kind::WORKER_READY) : ""));
            rc = 1;
        }
        else
        {
            reserved18 = ready.reserved18;
            const bool neural_ok = (ready.reserved18 == 1);
            const bool acceptable = neural_ok || (a.expect_protocol_only && ready.reserved18 == -1);
            if (!acceptable) rc = 4;
            if (ready.reserved18 == -1) reserved18_not_a_session = true;
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "reserved18=%d descriptor=%d cumodule=%d blob=%llu adapter=%04X:%04X "
                          "luid=%08X:%08X worker=\"%s\"",
                          ready.reserved18, ready.descriptor_status, ready.cumodule_status,
                          (unsigned long long)ready.first_blob_size, ready.adapter_vendor,
                          ready.adapter_device, ready.luid_high, ready.luid_low,
                          ready.worker_name);
            step("hello->worker_ready", acceptable, buf);

            report.kv("worker_reserved18", (std::int64_t)ready.reserved18);
            report.kv("worker_descriptor_status", (std::int64_t)ready.descriptor_status);
            report.kv("worker_cumodule_status", (std::int64_t)ready.cumodule_status);
            report.kv("worker_first_blob_size", (std::uint64_t)ready.first_blob_size);
            report.kv("worker_adapter_vendor", (std::uint64_t)ready.adapter_vendor);
            report.kv("worker_adapter_device", (std::uint64_t)ready.adapter_device);
            report.kv("worker_name", ready.worker_name);
        }

        // ---- the four input surfaces, metadata only -------------------------
        if (rc == 0 || rc == 4)
        {
            bool cfg_ok = true;
            std::string cfg_detail;
            for (std::uint32_t s = 0; s < (std::uint32_t)nr::InputSlot::COUNT; ++s)
            {
                nr::Config cfg;
                nr::init(cfg, nr::Kind::CONFIG, 0);
                cfg.input_slot = s;
                cfg.width = 1920;
                cfg.height = 1080;
                cfg.dxgi_format = 28;                 // DXGI_FORMAT_R8G8B8A8_UNORM
                cfg.row_pitch = 1920u * 4u;
                cfg.slice_pitch = 1920u * 4u * 1080u;
                cfg.resource_offset = 0;
                cfg.frame_slots = nr::FRAME_SLOT_COUNT;
                // The name a real client will have created; the probe only states it.
                char name[64];
                std::snprintf(name, sizeof name, "%s_PROBE", nr::input_slot_metadata_name(
                                  (nr::InputSlot)s));
                nr::set_text(cfg.shared_name, nr::PROTO_NAME_MAX, name);
                if (cli.write_message(cfg, 5000, err) != nr::IoResult::OK)
                {
                    cfg_ok = false;
                    cfg_detail = std::string("CONFIG ") + nr::input_slot_name((nr::InputSlot)s) +
                                 ": " + err;
                    break;
                }
            }
            step("config/four_input_slots", cfg_ok, cfg_detail);
            if (!cfg_ok) rc = 1;
        }

        // ---- frames ---------------------------------------------------------
        if (rc == 0 || rc == 4)
        {
            bool frames_ok = true;
            std::string frames_detail;
            for (unsigned f = 0; f < a.frames; ++f)
            {
                nr::FrameSubmit sub;
                nr::init(sub, nr::Kind::FRAME_SUBMIT, 1000u + f);
                sub.slot = f % nr::FRAME_SLOT_COUNT;
                sub.input_mask = 0x7u;               // COLOR | DEPTH | MOTION_VECTORS
                sub.width = 1920;
                sub.height = 1080;
                sub.dxgi_format = 28;
                sub.row_pitch = 1920u * 4u;
                sub.slice_pitch = 1920u * 4u * 1080u;
                sub.resource_offset = 0;
                sub.ingress_fence_value = (std::uint64_t)(f + 1);

                t0 = now_ms();
                if (cli.write_message(sub, 5000, err) != nr::IoResult::OK)
                {
                    frames_ok = false;
                    frames_detail = std::string("FRAME_SUBMIT ") + std::to_string(f) + ": " + err;
                    break;
                }
                nr::FrameComplete done{};
                const nr::IoResult rr = cli.read_message(done, 5000, err);
                control_rtt.push_back(now_ms() - t0);
                if (rr != nr::IoResult::OK || !nr::header_ok(done.h, nr::Kind::FRAME_COMPLETE))
                {
                    frames_ok = false;
                    frames_detail = std::string("FRAME_COMPLETE ") + std::to_string(f) + ": " +
                                    nr::io_result_name(rr) + " " + err;
                    break;
                }
                if (done.slot != sub.slot)
                {
                    frames_ok = false;
                    frames_detail = "FRAME_COMPLETE came back for slot " +
                                    std::to_string(done.slot) + ", sent for " +
                                    std::to_string(sub.slot);
                    break;
                }
                // E_NOTIMPL is the honest answer until the transport is wired to
                // the worker, so it is recorded rather than treated as a failure
                // of the CHANNEL - which is what this probe is testing.
                if (done.hr == 0) ++frames_completed;
            }
            {
                char buf[160];
                std::snprintf(buf, sizeof buf,
                              "%u frame(s) submitted, %u completed with hr=0, "
                              "E_NOTIMPL replies are expected until the transport is wired",
                              a.frames, frames_completed);
                step("frames/submit_complete", frames_ok,
                     frames_ok ? buf : frames_detail);
            }
            if (!frames_ok) rc = 1;
        }

        // ---- SHUTDOWN -------------------------------------------------------
        {
            nr::Shutdown sd;
            nr::init(sd, nr::Kind::SHUTDOWN, 0);
            sd.reason = 1;
            const bool ok = (cli.write_message(sd, 5000, err) == nr::IoResult::OK);
            step("shutdown", ok, ok ? "" : err);
            if (!ok && rc == 0) rc = 1;
        }
        cli.close();
    }

    report.end_array();

    const nr::Stats rtt = nr::summarise(control_rtt);
    report.key("control_rtt_ms");
    report.begin_object();
    report.kv("n", (std::uint64_t)rtt.n);
    report.kv("min", rtt.min);
    report.kv("median", rtt.median);
    report.kv("p95", rtt.p95);
    report.kv("max", rtt.max);
    report.end_object();

    report.kv("steps_ok", (std::uint64_t)steps_ok);
    report.kv("steps_failed", (std::uint64_t)steps_failed);
    report.kv("reserved18", (std::int64_t)reserved18);
    report.kv("worker_had_a_neural_session", !reserved18_not_a_session);
    report.kv("ok", rc == 0);
    report.end_object();

    std::string werr;
    if (!nr::write_text_file(a.json, report.str() + "\n", werr))
    {
        std::fprintf(stderr, "nr_ipc_probe: %s\n", werr.c_str());
    }

    std::printf("nr_ipc_probe: %u passed, %u failed, control rtt median=%.3f ms p95=%.3f ms "
                "max=%.3f ms\n", steps_ok, steps_failed, rtt.median, rtt.p95, rtt.max);
    std::fflush(stdout);
    return rc;
}
