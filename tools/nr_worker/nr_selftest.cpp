// ============================================================================
// nr_selftest.exe - the tests that need NO GPU and NO game.
//
// WHY THIS EXISTS, AND WHAT IT IS ALLOWED TO CLAIM
//     GitHub's Windows runners have no NVIDIA adapter, so the Reserved18 proof
//     and the two-GPU transport benchmark cannot run in CI. Everything that does
//     not need a GPU can, and does, run on every build - and the most valuable
//     of those is not a unit test at all: it is a REAL named-pipe handshake
//     between two endpoints over a real \\.\pipe\MGPU_NR_<pid> pipe, exercising
//     every message v1 defines, plus the two failure modes that matter (a peer
//     that disappears mid-message, and a peer speaking a different protocol).
//
// WHAT IT DELIBERATELY DOES NOT TEST
//     Anything about D3D12, NVAPI, NGX or the transport. Those are exercised by
//     the worker proof and the transport benchmark on a machine with both
//     adapters, and a green self-test is never evidence about them. The report
//     says so in a field, so a reader cannot mistake one for the other.
// ============================================================================

#include "nr_json.h"
#include "nr_pipe.h"
#include "nr_proto.h"
#include "nr_slots.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int         g_pass = 0;
    int         g_fail = 0;
    nr::Json    g_tests;

    void check(bool ok, const char *name, const std::string &detail = std::string())
    {
        if (ok)
        {
            ++g_pass;
            std::printf("PASS %s\n", name);
            g_tests.begin_object();
            g_tests.kv("name", name);
            g_tests.kv("passed", true);
            g_tests.end_object();
        }
        else
        {
            ++g_fail;
            std::printf("FAIL %s :: %s\n", name, detail.c_str());
            g_tests.begin_object();
            g_tests.kv("name", name);
            g_tests.kv("passed", false);
            g_tests.kv("detail", detail.c_str());
            g_tests.end_object();
        }
        std::fflush(stdout);
    }

    std::string hex32(std::uint32_t v)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08X", v);
        return buf;
    }

    // ------------------------------------------------------------------ proto
    void test_protocol()
    {
        check(nr::magic_bytes_are_mgpu(),
              "protocol/magic reads MGPU in memory order");

        struct Expect { nr::Kind kind; std::uint32_t size; const char *name; };
        const Expect expected[] = {
            { nr::Kind::HELLO,          (std::uint32_t)sizeof(nr::Hello),          "HELLO" },
            { nr::Kind::WORKER_READY,   (std::uint32_t)sizeof(nr::WorkerReady),    "WORKER_READY" },
            { nr::Kind::CONFIG,         (std::uint32_t)sizeof(nr::Config),         "CONFIG" },
            { nr::Kind::FRAME_SUBMIT,   (std::uint32_t)sizeof(nr::FrameSubmit),    "FRAME_SUBMIT" },
            { nr::Kind::FRAME_COMPLETE, (std::uint32_t)sizeof(nr::FrameComplete),  "FRAME_COMPLETE" },
            { nr::Kind::SHUTDOWN,       (std::uint32_t)sizeof(nr::Shutdown),       "SHUTDOWN" },
            { nr::Kind::ERROR,          (std::uint32_t)sizeof(nr::Error),          "ERROR" },
        };
        for (const Expect &e : expected)
        {
            const bool size_ok = (nr::size_for(e.kind) == e.size) && (e.size > sizeof(nr::Header));
            const bool name_ok = (std::strcmp(nr::kind_name(e.kind), e.name) == 0);
            check(size_ok && name_ok,
                  (std::string("protocol/size and name for ") + e.name).c_str(),
                  std::string("size_for=") + hex32(nr::size_for(e.kind)) +
                      " sizeof=" + hex32(e.size));
        }

        nr::Hello hello;
        nr::init(hello, nr::Kind::HELLO, 7u);
        const bool good = nr::header_ok(hello.h, nr::Kind::HELLO) &&
                          hello.h.magic == nr::PROTO_MAGIC &&
                          hello.h.version == nr::PROTO_VERSION &&
                          hello.h.size == sizeof(nr::Hello) &&
                          hello.h.frame_id == 7u;
        check(good, "protocol/a fresh header is valid and carries sizeof, not a constant");

        nr::Hello bad = hello;
        bad.h.magic = 0x12345678u;
        check(!nr::header_ok(bad.h, nr::Kind::HELLO), "protocol/a foreign magic is refused");

        bad = hello;
        bad.h.version = 2u;
        check(!nr::header_ok(bad.h, nr::Kind::HELLO),
              "protocol/a different protocol version is refused");

        bad = hello;
        bad.h.kind = (std::uint16_t)nr::Kind::CONFIG;
        check(!nr::header_ok(bad.h, nr::Kind::HELLO),
              "protocol/a message of the wrong kind is refused where HELLO was expected");

        bad = hello;
        bad.h.size = sizeof(nr::Hello) + 4u;
        check(!nr::header_ok(bad.h, nr::Kind::HELLO),
              "protocol/a struct size that is not this build's is refused");

        check(nr::size_for((nr::Kind)99) == 0u,
              "protocol/an unknown kind has no size and cannot be decoded");
    }

    // ------------------------------------------------------------------- pipe
    // A real handshake over a real named pipe, driven by two threads in this
    // process. It is the same code the worker and the game side will run.
    struct PipeOutcome
    {
        bool     server_ok = false;
        bool     client_ok = false;
        std::uint32_t hello_frame = 0;
        std::uint32_t ready_adapter_device = 0;
        std::uint32_t config_slot = 0;
        std::uint32_t submit_slot = 0;
        std::uint64_t submit_fence = 0;
        std::uint32_t complete_slot = 0;
        std::int32_t  complete_hr = 0;
        std::uint32_t shutdown_reason = 0;
        std::string  problem;
    };

    void test_pipe_handshake()
    {
        PipeOutcome out;
        wchar_t name[128];
        nr::pipe_name_for_pid((std::uint32_t)GetCurrentProcessId(), name, 128);

        std::thread server([&]() {
            nr::Pipe srv;
            std::string err;
            if (!srv.listen_and_accept(name, 5000, err))
            {
                out.problem = "server listen: " + err;
                return;
            }

            nr::Hello hello{};
            std::string rerr;
            if (srv.read_message(hello, 5000, rerr) != nr::IoResult::OK ||
                !nr::header_ok(hello.h, nr::Kind::HELLO))
            {
                out.problem = "server HELLO: " + rerr + nr::header_problem(hello.h, nr::Kind::HELLO);
                return;
            }
            out.hello_frame = hello.h.frame_id;

            nr::WorkerReady ready;
            nr::init(ready, nr::Kind::WORKER_READY, hello.h.frame_id);
            ready.reserved18 = 1;
            ready.descriptor_status = 0;
            ready.cumodule_status = 0;
            ready.adapter_device = 0x2786;
            ready.first_blob_size = 3944768ull;
            nr::set_text(ready.worker_name, nr::PROTO_NAME_MAX, "selftest-worker");
            if (srv.write_message(ready, 5000, rerr) != nr::IoResult::OK)
            {
                out.problem = "server WORKER_READY: " + rerr;
                return;
            }

            nr::Config cfg{};
            if (srv.read_message(cfg, 5000, rerr) != nr::IoResult::OK ||
                !nr::header_ok(cfg.h, nr::Kind::CONFIG))
            {
                out.problem = "server CONFIG: " + rerr + nr::header_problem(cfg.h, nr::Kind::CONFIG);
                return;
            }
            out.config_slot = cfg.input_slot;

            nr::FrameSubmit sub{};
            if (srv.read_message(sub, 5000, rerr) != nr::IoResult::OK ||
                !nr::header_ok(sub.h, nr::Kind::FRAME_SUBMIT))
            {
                out.problem = "server FRAME_SUBMIT: " + rerr +
                              nr::header_problem(sub.h, nr::Kind::FRAME_SUBMIT);
                return;
            }
            out.submit_slot = sub.slot;
            out.submit_fence = sub.ingress_fence_value;

            nr::FrameComplete done;
            nr::init(done, nr::Kind::FRAME_COMPLETE, sub.h.frame_id);
            done.slot = sub.slot;
            done.hr = 0;
            done.egress_fence_value = 42u;
            if (srv.write_message(done, 5000, rerr) != nr::IoResult::OK)
            {
                out.problem = "server FRAME_COMPLETE: " + rerr;
                return;
            }

            nr::Shutdown sd{};
            if (srv.read_message(sd, 5000, rerr) != nr::IoResult::OK ||
                !nr::header_ok(sd.h, nr::Kind::SHUTDOWN))
            {
                out.problem = "server SHUTDOWN: " + rerr +
                              nr::header_problem(sd.h, nr::Kind::SHUTDOWN);
                return;
            }
            out.shutdown_reason = sd.reason;
            out.server_ok = true;
        });

        std::thread client([&]() {
            nr::Pipe cli;
            std::string err;
            if (!cli.connect(name, 5000, err))
            {
                out.problem = "client connect: " + err;
                return;
            }

            nr::Hello hello;
            nr::init(hello, nr::Kind::HELLO, 11u);
            hello.client_pid = (std::uint32_t)GetCurrentProcessId();
            hello.client_protocol_version = nr::PROTO_VERSION;
            nr::set_text(hello.client_name, nr::PROTO_NAME_MAX, "selftest-client");
            if (cli.write_message(hello, 5000, err) != nr::IoResult::OK)
            {
                out.problem = "client HELLO: " + err;
                return;
            }

            nr::WorkerReady ready{};
            if (cli.read_message(ready, 5000, err) != nr::IoResult::OK ||
                !nr::header_ok(ready.h, nr::Kind::WORKER_READY))
            {
                out.problem = "client WORKER_READY: " + err +
                              nr::header_problem(ready.h, nr::Kind::WORKER_READY);
                return;
            }
            out.ready_adapter_device = ready.adapter_device;

            nr::Config cfg;
            nr::init(cfg, nr::Kind::CONFIG, 11u);
            cfg.input_slot = (std::uint32_t)nr::InputSlot::MOTION_VECTORS;
            cfg.width = 1920;
            cfg.height = 1080;
            cfg.dxgi_format = 28;          // DXGI_FORMAT_R8G8B8A8_UNORM
            cfg.row_pitch = 1920 * 4;
            cfg.slice_pitch = 1920 * 4 * 1080;
            cfg.frame_slots = nr::FRAME_SLOT_COUNT;
            nr::set_text(cfg.shared_name, nr::PROTO_NAME_MAX, "MGPU_NR_SELFTEST");
            if (cli.write_message(cfg, 5000, err) != nr::IoResult::OK)
            {
                out.problem = "client CONFIG: " + err;
                return;
            }

            nr::FrameSubmit sub;
            nr::init(sub, nr::Kind::FRAME_SUBMIT, 11u);
            sub.slot = 1;
            sub.input_mask = 0x7u;
            sub.ingress_fence_value = 7u;
            if (cli.write_message(sub, 5000, err) != nr::IoResult::OK)
            {
                out.problem = "client FRAME_SUBMIT: " + err;
                return;
            }

            nr::FrameComplete done{};
            if (cli.read_message(done, 5000, err) != nr::IoResult::OK ||
                !nr::header_ok(done.h, nr::Kind::FRAME_COMPLETE))
            {
                out.problem = "client FRAME_COMPLETE: " + err +
                              nr::header_problem(done.h, nr::Kind::FRAME_COMPLETE);
                return;
            }
            out.complete_slot = done.slot;
            out.complete_hr = done.hr;

            nr::Shutdown sd;
            nr::init(sd, nr::Kind::SHUTDOWN, 11u);
            sd.reason = 1u;
            if (cli.write_message(sd, 5000, err) != nr::IoResult::OK)
            {
                out.problem = "client SHUTDOWN: " + err;
                return;
            }
            out.client_ok = true;
        });

        server.join();
        client.join();

        check(out.server_ok && out.client_ok, "pipe/a full v1 handshake over a real named pipe",
              out.problem.empty() ? std::string("one side did not finish") : out.problem);
        check(out.hello_frame == 11u && out.ready_adapter_device == 0x2786u &&
                  out.config_slot == (std::uint32_t)nr::InputSlot::MOTION_VECTORS &&
                  out.submit_slot == 1u && out.submit_fence == 7u &&
                  out.complete_slot == 1u && out.complete_hr == 0 &&
                  out.shutdown_reason == 1u,
              "pipe/every field of every message survived the round trip");
    }

    // A peer that vanishes mid-message must be a DISCONNECT, not a hang and not
    // a partial message that looks complete.
    void test_pipe_truncated_peer()
    {
        wchar_t name[128];
        nr::pipe_name_for_pid((std::uint32_t)GetCurrentProcessId() + 1u, name, 128);

        bool got_disconnect = false;
        std::string detail;

        std::thread server([&]() {
            nr::Pipe srv;
            std::string err;
            if (!srv.listen_and_accept(name, 5000, err))
            {
                detail = "server listen: " + err;
                return;
            }
            nr::FrameSubmit sub{};
            std::string rerr;
            const nr::IoResult r = srv.read_message(sub, 2000, rerr);
            got_disconnect = (r == nr::IoResult::DISCONNECTED);
            if (!got_disconnect) detail = std::string("read_message returned ") + nr::io_result_name(r) + ": " + rerr;
        });

        std::thread client([&]() {
            nr::Pipe cli;
            std::string err;
            if (!cli.connect(name, 5000, err))
            {
                detail = "client connect: " + err;
                return;
            }
            // Half a header, then gone.
            const std::uint32_t half = 7u;
            cli.write_all(&half, sizeof half, 2000, err);
            cli.close();
        });

        server.join();
        client.join();

        check(got_disconnect,
              "pipe/a peer that disappears mid-message is reported as DISCONNECTED",
              detail.empty() ? std::string("the reader did not report a disconnect") : detail);
    }

    // A read with no peer at all must return on its deadline.
    void test_pipe_read_timeout()
    {
        wchar_t name[128];
        nr::pipe_name_for_pid((std::uint32_t)GetCurrentProcessId() + 2u, name, 128);

        bool timed_out = false;
        std::string detail;

        std::thread server([&]() {
            nr::Pipe srv;
            std::string err;
            if (!srv.listen_and_accept(name, 5000, err))
            {
                detail = "server listen: " + err;
                return;
            }
            nr::Shutdown sd{};
            std::string rerr;
            const std::uint64_t t0 = nr::now_ms();
            const nr::IoResult r = srv.read_message(sd, 300, rerr);
            const std::uint64_t dt = nr::now_ms() - t0;
            timed_out = (r == nr::IoResult::TIMEOUT) && (dt < 3000u);
            if (!timed_out)
            {
                detail = std::string("read_message returned ") + nr::io_result_name(r) +
                         " after " + std::to_string((unsigned long long)dt) + " ms: " + rerr;
            }
        });

        std::thread client([&]() {
            nr::Pipe cli;
            std::string err;
            if (!cli.connect(name, 5000, err))
            {
                detail = "client connect: " + err;
                return;
            }
            // Connect, say nothing, stay connected until the server gives up.
            nr::sleep_ms(1500);
            cli.close();
        });

        server.join();
        client.join();

        check(timed_out,
              "pipe/a silent peer produces a bounded TIMEOUT on the reader's deadline",
              detail.empty() ? std::string("the reader did not time out") : detail);
    }

    // ------------------------------------------------------------------ slots
    void test_slots()
    {
        nr::SlotRing ring(3);
        check(ring.count() == 3u && ring.all_free() && ring.pick_free() == 0,
              "slots/a fresh ring of three has slot 0 free");

        // Three frames in flight at once: the point of the ring.
        bool ok = true;
        for (std::uint32_t i = 0; i < 3u; ++i)
        {
            ok = ok && ring.begin_game_copy(i, 100u + i, 1u);
            ok = ok && ring.set_geometry(i, 1920, 1080, 28, 7680, 7680u * 1080u, 0u);
            ok = ok && ring.game_copy_done(i, 1u);
        }
        check(ok && ring.free_count() == 0u && ring.pick_free() == -1,
              "slots/three frames can be in flight at once and the ring then reports full");

        check(!ring.begin_game_copy(0, 200u, 1u),
              "slots/a fourth frame is refused while all three slots are in flight");
        check(ring.illegal_transitions() == 1u,
              "slots/the refused transition was counted, not ignored",
              "illegal=" + std::to_string(ring.illegal_transitions()));

        ok = true;
        for (std::uint32_t i = 0; i < 3u; ++i)
        {
            // The worker PROMISES fence value 2 but has only reached 1. That
            // gap is the whole point of the release precondition below.
            ok = ok && ring.worker_begin(i, 2u);
            ok = ok && ring.worker_done(i, 1u);
            ok = ok && ring.begin_game_copyback(i);
        }
        check(ok, "slots/the worker can take all three and publish them back");

        // THE RULE. The worker has not reached its required fence value yet, so
        // slot 0 must NOT become reusable even though the game reports it done.
        const bool refused = !ring.release(0, 1u, 1u);
        check(refused, "slots/release is refused while the WORKER is short of its fence");
        check(ring.at(0).state == nr::SlotState::GAME_COPYING_BACK,
              "slots/the refused slot was left in GAME_COPYING_BACK, not freed");

        ok = ring.release(0, 4096u, 2u);
        check(ok && ring.at(0).state == nr::SlotState::FREE,
              "slots/release succeeds once BOTH devices are known to be complete");

        check(!ring.release(1, 4096u, 2u - 1u) &&
                  ring.at(1).state == nr::SlotState::GAME_COPYING_BACK,
              "slots/a slot whose worker fence is still short stays held");

        // Wrong order: the worker cannot start before the game published.
        nr::SlotRing r2(2);
        check(r2.begin_game_copy(0, 1u, 1u), "slots/second ring: game copy begins");
        check(!r2.worker_begin(0, 1u),
              "slots/the worker cannot begin before the game finished copying");
        check(r2.game_copy_done(0, 1u) && r2.worker_begin(0, 1u) && r2.worker_done(0, 1u),
              "slots/the legal order is accepted");
        check(!r2.begin_game_copyback(1) ,
              "slots/a slot the worker never ran cannot be copied back");
        check(!r2.set_geometry(9u, 1, 1, 1, 1, 1, 1),
              "slots/geometry for a slot that does not exist is refused");

        // The GAME side of the same rule, which is what makes it symmetric: a
        // slot the game itself has not finished with cannot be handed back.
        nr::SlotRing r3(1);
        const bool begun = r3.begin_game_copy(0, 1u, 500u);   // promised fence 500
        const bool copied = r3.game_copy_done(0, 10u);        // reached only 10
        const bool ran = r3.worker_begin(0, 1u) && r3.worker_done(0, 1u) &&
                         r3.begin_game_copyback(0);
        check(begun && copied && ran, "slots/third ring: the pipeline ran to the copy back");
        check(!r3.release(0, 10u, 1u),
              "slots/release is refused while the GAME is short of its fence");
        check(r3.release(0, 500u, 1u),
              "slots/the same slot is released once the game fence reaches what it promised");
    }

    // ------------------------------------------------------------------- json
    void test_json()
    {
        nr::Json j;
        j.begin_object();
        j.kv("transport", "CROSS_ADAPTER_BUFFER");
        j.kv("frames", (std::uint64_t)3);
        j.kv("ok", true);
        j.kv_null("unsupported");
        j.key("stages");
        j.begin_object();
        j.kv("ingress", 0.5);
        j.end_object();
        j.key("list");
        j.begin_array();
        j.value_int(1);
        j.value_int(2);
        j.end_array();
        j.end_object();

        const std::string want =
            "{\"transport\":\"CROSS_ADAPTER_BUFFER\",\"frames\":3,\"ok\":true,"
            "\"unsupported\":null,\"stages\":{\"ingress\":0.500},\"list\":[1,2]}";
        check(j.str() == want, "json/the writer emits exactly the expected document",
              "got: " + j.str());
        check(j.well_formed(), "json/a closed document is well formed");

        nr::Json bad;
        bad.begin_object();
        bad.kv("a", 1);
        check(!bad.well_formed(), "json/an unclosed object is reported as malformed");

        check(nr::Json::escape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd",
              "json/escaping covers quotes, backslashes and newlines");

        std::vector<double> s;
        for (int i = 1; i <= 100; ++i) s.push_back((double)i);
        const nr::Stats st = nr::summarise(s);
        const bool stats_ok = st.n == 100u && st.min == 1.0 && st.max == 100.0 &&
                              st.median == 50.0 && st.p95 == 95.0;
        check(stats_ok, "json/nearest-rank statistics on 1..100",
              "n=" + std::to_string(st.n) + " median=" + std::to_string(st.median) +
                  " p95=" + std::to_string(st.p95));

        const nr::Stats empty = nr::summarise(std::vector<double>());
        check(empty.n == 0u && empty.median == 0.0,
              "json/an empty sample reports n=0 rather than a zero-latency result");
    }

    void test_names()
    {
        wchar_t name[128];
        nr::pipe_name_for_pid(4242u, name, 128);
        check(std::wstring(name) == L"\\\\.\\pipe\\MGPU_NR_4242",
              "names/the control pipe is \\\\.\\pipe\\MGPU_NR_<pid>");

        check(std::strcmp(nr::input_slot_name(nr::InputSlot::MOTION_VECTORS), "MOTION_VECTORS") == 0 &&
                  std::strcmp(nr::slot_state_name(nr::SlotState::READY_FOR_WORKER),
                              "READY_FOR_WORKER") == 0 &&
                  std::strcmp(nr::input_slot_metadata_name(nr::InputSlot::COLOR),
                              "MGPU_NR_INPUT_COLOR") == 0,
              "names/the four input slots and six states keep their names");
    }
}

int main(int argc, char **argv)
{
    const char *json_path = "nr-selftest.json";
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc)
        {
            json_path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::printf("nr_selftest - GPU-free tests for the MGPU NR worker control\n"
                        "              protocol, pipe framing and frame-slot pipeline.\n"
                        "  --json <path>   write the machine-readable report (default\n"
                        "                  nr-selftest.json)\n");
            return 0;
        }
        else
        {
            std::fprintf(stderr, "nr_selftest: unrecognised argument \"%s\"\n", argv[i]);
            return 2;
        }
    }

    std::printf("nr_selftest - GPU-free. Nothing here is evidence about D3D12, NVAPI,\n");
    std::printf("             NGX, Reserved18 or the transport; those need both adapters.\n");
    std::fflush(stdout);

    // The nesting has to be built as we go, so the test recorder is an array that
    // is opened here and closed at the end.
    g_tests.begin_object();
    g_tests.kv("tool", "nr_selftest");
    g_tests.kv("requires_gpu", false);
    g_tests.kv("claims", "control protocol v1, named-pipe framing and the frame-slot "
                         "pipeline only - never the D3D12 or NGX paths");
    g_tests.key("tests");
    g_tests.begin_array();

    test_protocol();
    test_pipe_handshake();
    test_pipe_truncated_peer();
    test_pipe_read_timeout();
    test_slots();
    test_json();
    test_names();

    g_tests.end_array();
    g_tests.kv("passed", (std::uint64_t)g_pass);
    g_tests.kv("failed", (std::uint64_t)g_fail);
    g_tests.kv("ok", g_fail == 0);
    g_tests.end_object();

    std::string err;
    const std::string text = g_tests.str() + "\n";
    if (!nr::write_text_file(json_path, text, err))
    {
        std::fprintf(stderr, "nr_selftest: %s\n", err.c_str());
    }

    std::printf("nr_selftest: %d passed, %d failed\n", g_pass, g_fail);
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
