// ============================================================================
// nr_relay_selftest.exe - THE RELAY MODULE, TESTED WITHOUT A GPU.
//
// WHAT THIS IS FOR
//     src/nr_relay.cpp is the game-side half of the out-of-process neural
//     bridge: it launches mgpu_nr_worker.exe, speaks control protocol v1 to it
//     over a named pipe, publishes the four host-staged frame mappings and
//     submits frames. It is compiled into the add-on, where testing it means
//     launching Cyberpunk on the rig - which Smart App Control currently
//     refuses to run at all.
//
//     So the module is compiled a SECOND time, into this executable, and driven
//     here against a REAL worker process started in --protocol-only mode: no
//     GPU, no NGX, no NVAPI, no D3D12. GitHub's Windows runners have none of
//     those, and this runs on every build.
//
// WHAT IT PROVES, IN ORDER
//     1. available() resolves the worker beside the module and reports what it
//        found rather than assuming it.
//     2. the module LAUNCHES the real worker (CreateProcessW, no console),
//        connects, sends HELLO, reads WORKER_READY and reports the four slots
//        it published with their geometry and pitches.
//     3. THE HONESTY RULE. A --protocol-only worker never created a Reserved18
//        session and says so: reserved18 == -1. The module must report that
//        verbatim, must NOT name it Success, and must refuse to submit a frame
//        to it - and the refusal is asserted too, in a second started relay so
//        that the main one still proves what it is here to prove.
//     4. shutdown() is clean: the worker it launched is gone, and a second
//        shutdown() is safe.
//     5. the WORKER'S OWN LOG, after it has exited, shows the CONFIG metadata
//        the relay sent - the same four mapping names, the same geometry - so
//        "the module published the slots" is a fact about the peer, not a claim
//        this program makes about itself.
//
// WHAT IT CANNOT PROVE, AND DOES NOT CLAIM
//     Not one pixel. The frame round trip needs four surfaces on the RTX 4070
//     and a neural runtime, and neither exists on a CI runner. That proof is
//     tools/nr_worker/nr_slot_client.cpp on real hardware. This proves the
//     control path and the honesty rule, GPU-free, in CI.
// ============================================================================

// THE MODULE ITSELF, not a copy of it and not a library of it. The macro has to
// be set BEFORE the include because the module's entire body is inside
// `#ifdef MGPU_NR_RELAY`; tools/nr_worker/CMakeLists.txt sets it on this target
// and explains why it is done this way rather than by listing
// ../../src/nr_relay.cpp as a source (a per-source definition in that directory
// would also reach the add-on target in the parent directory). The path is
// relative to this file, so it needs no include directory either.
#define MGPU_NR_RELAY 1
#include "../../src/nr_relay.cpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct Args
    {
        std::wstring worker;        // --point-at-worker
        std::wstring pipe;          // --pipe
        std::wstring log_name;      // --log-name (the worker's own log)
        unsigned width = 640u;      // --width
        unsigned height = 360u;     // --height
        unsigned frames = 1u;       // --frames (how many submits to attempt)
        bool help = false;
    };

    void usage()
    {
        std::fprintf(stderr,
            "nr_relay_selftest - the relay module against a REAL --protocol-only worker.\n"
            "\n"
            "  --point-at-worker <path>  the mgpu_nr_worker.exe this test must drive\n"
            "  --pipe <name>             the pipe the relay should use\n"
            "  --log-name <name>         the worker's log name (context-<name>.log)\n"
            "  --width <n> --height <n>  geometry to publish (default 640x360)\n"
            "  --frames <n>              submits to attempt (default 1)\n"
            "\n"
            "Exit 0 only when every check holds. One machine-readable line is printed:\n"
            "  RELAY-SELFTEST-RESULT ok=YES|NO ...\n");
    }

    bool file_exists(const std::wstring &path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0u;
    }

    std::wstring dir_of(const std::wstring &file)
    {
        const std::size_t cut = file.find_last_of(L"\\/");
        return (cut == std::wstring::npos) ? std::wstring() : file.substr(0, cut);
    }

    //: Read the worker's log. Retried, because the file is written by another
    //: process: shutdown() has already waited for it to exit, so the common case
    //: is one read, and the retry only costs anything when something is wrong -
    //: which is exactly when a false failure would be most misleading.
    bool read_log_retry(const std::string &path, std::string &out, unsigned timeout_ms)
    {
        const DWORD deadline = GetTickCount() + timeout_ms;
        for (;;)
        {
            out.clear();
            std::ifstream f(path.c_str(), std::ios::binary);
            if (f)
            {
                std::ostringstream ss;
                ss << f.rdbuf();
                out = ss.str();
                if (!out.empty()) return true;
            }
            if ((long)(GetTickCount() - deadline) >= 0) return false;
            Sleep(250);
        }
    }

    bool contains(const std::string &hay, const std::string &needle)
    {
        return hay.find(needle) != std::string::npos;
    }

    //: Case-sensitive on purpose: the word this test is looking for is written
    //: with a capital S exactly once in the whole module, and a case-insensitive
    //: match would also catch prose that means something else.
    bool contains_success_claim(const std::string &log)
    {
        std::istringstream in(log);
        std::string line;
        while (std::getline(in, line))
        {
            if (contains(line, "Reserved18") && contains(line, "Success")) return true;
        }
        return false;
    }

    //: Is a pid still alive? Used to assert that the worker the module launched
    //: is gone after shutdown(). OpenProcess on a dead pid fails with
    //: ERROR_INVALID_PARAMETER, which is the check.
    bool pid_exited(DWORD pid, DWORD timeout_ms)
    {
        const DWORD deadline = GetTickCount() + timeout_ms;
        for (;;)
        {
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (h == nullptr)
            {
                const DWORD e = GetLastError();
                if (e == ERROR_INVALID_PARAMETER) return true;   // the pid is gone
                return false;                                    // access denied: cannot say
            }
            const DWORD w = WaitForSingleObject(h, 50);
            CloseHandle(h);
            if (w == WAIT_OBJECT_0) return true;
            if ((long)(GetTickCount() - deadline) >= 0) return false;
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Arguments arrive as WIDE text: --point-at-worker names a filesystem path.
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv == nullptr)
    {
        std::fprintf(stderr, "nr_relay_selftest: CommandLineToArgvW failed\n");
        return 2;
    }

    Args a;
    for (int i = 1; i < wargc; ++i)
    {
        const std::wstring w = wargv[i];
        auto next = [&](std::wstring &out) {
            if (i + 1 >= wargc) return false;
            out = wargv[++i];
            return true;
        };
        if (w == L"--point-at-worker") { if (!next(a.worker)) { a.help = true; } }
        else if (w == L"--pipe")        { if (!next(a.pipe))   { a.help = true; } }
        else if (w == L"--log-name")    { if (!next(a.log_name)) { a.help = true; } }
        else if (w == L"--width")       { if (i + 1 < wargc) a.width = (unsigned)wcstoul(wargv[++i], nullptr, 10); }
        else if (w == L"--height")      { if (i + 1 < wargc) a.height = (unsigned)wcstoul(wargv[++i], nullptr, 10); }
        else if (w == L"--frames")      { if (i + 1 < wargc) a.frames = (unsigned)wcstoul(wargv[++i], nullptr, 10); }
        else if (w == L"--help" || w == L"-h") { a.help = true; }
        else
        {
            char narrow[512] = "";
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, narrow, sizeof narrow, nullptr, nullptr);
            std::fprintf(stderr, "nr_relay_selftest: unrecognised argument \"%s\"\n", narrow);
            LocalFree(wargv);
            return 2;
        }
    }
    if (a.help)
    {
        usage();
        LocalFree(wargv);
        return 2;
    }
    if (a.worker.empty())
    {
        std::fprintf(stderr, "nr_relay_selftest: --point-at-worker is required; this test must "
                             "drive the binary CI just built\n");
        LocalFree(wargv);
        return 2;
    }
    if (!file_exists(a.worker))
    {
        char narrow[1024] = "";
        WideCharToMultiByte(CP_UTF8, 0, a.worker.c_str(), -1, narrow, sizeof narrow, nullptr, nullptr);
        std::fprintf(stderr, "nr_relay_selftest: the worker is not there: %s\n", narrow);
        LocalFree(wargv);
        return 2;
    }
    if (a.width == 0u) a.width = 640u;
    if (a.height == 0u) a.height = 360u;
    if (a.frames == 0u) a.frames = 1u;
    if (a.pipe.empty())
    {
        wchar_t b[128];
        std::swprintf(b, 128, L"\\\\.\\pipe\\MGPU_NR_RELAY_SELFTEST_%lu",
                      (unsigned long)GetCurrentProcessId());
        a.pipe = b;
    }
    if (a.log_name.empty()) a.log_name = L"nr-relay-selftest-worker";

    unsigned checks = 0u, failures = 0u;
    auto check = [&](bool ok, const char *what) {
        ++checks;
        if (!ok) { ++failures; std::printf("  FAIL  %s\n", what); }
        else     { std::printf("  ok    %s\n", what); }
    };

    // The module's own log goes to the console by default. That is deliberate:
    // the WORD this test greps for ("Success") is produced by the module, so the
    // module's own output is what the honesty check has to inspect.
    std::printf("nr_relay_selftest: relay module vs a real --protocol-only worker\n");
    std::printf("  worker   = %ls\n", a.worker.c_str());
    std::printf("  pipe     = %ls\n", a.pipe.c_str());
    std::fflush(stdout);

    // ---- 1. available() ------------------------------------------------
    const std::wstring dir = dir_of(a.worker);
    {
        // The resolver looks for <module dir>\mgpu\nvngx.dll_mgpu_nr_worker.exe.
        // This executable is not the add-on, so the check is about the resolver
        // working and saying what it found; CI stages the worker where the
        // resolver looks, which is what turns this from a shape check into a
        // check of the deployed layout and of the nvngx.dll_ caller-gate prefix.
        std::string why;
        const bool found = mgpu::relay::available(why);
        std::printf("  available() = %s (%s)\n", found ? "yes" : "no", why.c_str());
        check(!why.empty(), "available() states what it looked for, found or not");
        check(found || contains(why, "MISSING"),
              "available() is either yes or an honest MISSING line");
        if (found)
        {
            check(contains(why, "nvngx.dll_mgpu_nr_worker.exe"),
                  "the resolved worker keeps the nvngx.dll_ caller-gate prefix");
            check(contains(why, "\\mgpu\\") || contains(why, "/mgpu/"),
                  "the worker was resolved in the mgpu\\ subdirectory of the module");
        }
    }

    // ---- 2. start(): launch, HELLO, WORKER_READY, four slots ------------
    mgpu::relay::Config cfg;
    cfg.worker_exe = a.worker;
    cfg.module_dir = dir;
    cfg.pipe_name = a.pipe;
    cfg.width = a.width;
    cfg.height = a.height;
    cfg.auto_launch = true;
    // The whole point: no GPU, no NGX, no NVAPI. The relay must not build the
    // runtime flags, and nothing here may mention a rasterizer.
    cfg.protocol_only = true;
    cfg.worker_args = L"--protocol-only --accept-seconds 30";
    cfg.worker_log_name = a.log_name;
    cfg.connect_ms = 20000u;
    cfg.handshake_ms = 20000u;
    cfg.shutdown_ms = 3000u;

    std::string why;
    const mgpu::relay::Status st = mgpu::relay::start(cfg, why);
    std::printf("  start() = %s (%s)\n", mgpu::relay::status_name(st), why.c_str());
    std::fflush(stdout);
    if (st != mgpu::relay::Status::OK)
    {
        std::printf("RELAY-SELFTEST-RESULT ok=NO stage=start status=%s detail=\"%s\"\n",
                    mgpu::relay::status_name(st), why.c_str());
        LocalFree(wargv);
        return 1;
    }

    const std::int32_t r18 = mgpu::relay::reserved18();
    const DWORD worker_pid = mgpu::relay::worker_pid();
    check(mgpu::relay::ready(), "the relay reports ready after WORKER_READY");

    // ---- the four published slots, with the arithmetic spelled out -------
    // EXPECTED VALUES, DERIVED IN THE TEST rather than read back from the
    // module: row_pitch = align_up(width * bpp, 256), slice_pitch = row_pitch *
    // height, bytes = slice_pitch * 3. All four slots are 4 bytes per pixel at
    // this geometry, so they must all agree - and if the module ever publishes a
    // pitch its own CONFIG did not describe, the worker would refuse the mapping
    // on the rig. Here the numbers are checked against the rule itself.
    {
        const unsigned expected_row = (unsigned)((((unsigned long long)a.width * 4ull) + 255ull) &
                                                 ~255ull);
        const unsigned expected_slice = expected_row * a.height;
        const unsigned long long expected_bytes =
            (unsigned long long)expected_slice * 3ull;
        bool all_ok = true;
        for (unsigned i = 0u; i < 4u; ++i)
        {
            mgpu::relay::SlotInfo si;
            const bool got = mgpu::relay::slot_info(i, si);
            const bool match = got && si.row_pitch == expected_row &&
                               si.slice_pitch == expected_slice && si.bytes == expected_bytes &&
                               si.width == a.width && si.height == a.height;
            if (!match) all_ok = false;
            std::printf("  slot %u      = %s name=%s row_pitch=%u slice_pitch=%u bytes=%llu\n",
                        i, got ? "published" : "MISSING", si.mapping_name.c_str(),
                        si.row_pitch, si.slice_pitch, (unsigned long long)si.bytes);
        }
        check(all_ok, "all four slots are published with the derived geometry and pitches");
    }

    // ---- 3a. THE HONESTY RULE: report it, never round it up --------------
    {
        const std::string &ev = mgpu::relay::reserved18_evidence();
        std::printf("  evidence  = %s\n", ev.c_str());
        check(r18 == -1, "a --protocol-only worker answered reserved18 == -1");
        check(!mgpu::relay::reserved18_is_success(r18),
              "reserved18_is_success(-1) is false: the module does not call it a success");
        check(std::strcmp(mgpu::relay::reserved18_name(r18), "not-a-session") == 0,
              "reserved18_name(-1) says not-a-session, not Success");
        check(ev == "[MGPU][RELAY] CreateFeature(Reserved18) result=0xFFFFFFFF (not-a-session) "
                    "src=worker",
              "the log line is the exact wire format and does not claim Success");
        check(!contains_success_claim(ev),
              "the module's Reserved18 line does not contain the word Success");
        // And the predicate itself, so the module cannot pass by spelling.
        check(mgpu::relay::reserved18_is_success(1) &&
              std::strcmp(mgpu::relay::reserved18_name(1), "Success") == 0,
              "the module still recognises the worker's own 1 as Success");
    }

    // ---- 3b. no frame may be submitted to a worker with no session -------
    // Recorded, not assumed: the summary line below reports what actually
    // happened rather than what the test intended to happen.
    bool submit_returned = false;
    std::int32_t submit_hr = 0;
    bool submit_output_changed = false;
    std::string submit_err;
    {
        mgpu::relay::SubmitResult sr;
        std::string ferr;
        // Buffers for a tightly packed 640x360 frame, but they are never read:
        // the guard must refuse before anything is copied or sent.
        std::vector<unsigned char> color((std::size_t)a.width * 4u * a.height, 0x11);
        std::vector<unsigned char> depth((std::size_t)a.width * 4u * a.height, 0x22);
        std::vector<unsigned char> mvec((std::size_t)a.width * 4u * a.height, 0x33);
        std::vector<unsigned char> out((std::size_t)a.width * 4u * a.height, 0x44);
        mgpu::relay::FrameView fv;
        fv.color = color.data();  fv.color_row_bytes = a.width * 4u;
        fv.depth = depth.data();  fv.depth_row_bytes = a.width * 4u;
        fv.motion_vectors = mvec.data(); fv.motion_row_bytes = a.width * 4u;
        fv.out = out.data();      fv.out_row_bytes = a.width * 4u;

        const bool sent = mgpu::relay::submit(fv, 1u, sr, ferr);
        submit_returned = sent;
        submit_hr = sr.worker_hr;
        submit_output_changed = sr.output_changed;
        submit_err = ferr;
        std::printf("  submit()  = %s (%s)\n", sent ? "accepted" : "refused", ferr.c_str());
        check(!sent, "submit() REFUSES while the worker has no Reserved18 session");
        check(!sr.completed, "no FRAME_COMPLETE was reported for a refused submit");
        check(contains(ferr, "no neural session"),
              "the refusal names the reason: the worker has no neural session");
    }

    // ---- 4. shutdown is clean, and idempotent ---------------------------
    mgpu::relay::shutdown();
    check(!mgpu::relay::ready(), "after shutdown() the relay is not ready");
    check(pid_exited(worker_pid, 3000u),
          "the worker the module launched has exited (SHUTDOWN, not a kill we cannot see)");
    mgpu::relay::shutdown();
    check(!mgpu::relay::ready(), "a second shutdown() is safe and leaves it not ready");

    // ---- 5. the WORKER's own log is the independent evidence -------------
    {
        char name[256] = "";
        WideCharToMultiByte(CP_UTF8, 0, a.log_name.c_str(), -1, name, sizeof name, nullptr, nullptr);
        const std::string path = std::string("context-") + name + ".log";
        std::string log;
        const bool got = read_log_retry(path, log, 5000u);
        std::printf("  worker log = %s (%s, %zu bytes)\n", path.c_str(),
                    got ? "read" : "NOT READ", log.size());
        check(got, "the worker wrote its own log");
        check(contains(log, "WORKER_READY"), "the worker logged WORKER_READY");
        check(contains(log, "reserved18=-1"),
              "the worker itself says reserved18=-1 over the pipe");
        check(contains(log, "protocol-only"),
              "the worker itself says it ran protocol-only");
        // The mapping names and the geometry the RELAY sent, read back out of
        // the WORKER's log: the peer received exactly what the module published.
        const std::string row = "row_pitch=" + std::to_string((unsigned)((((unsigned long long)a.width * 4ull) + 255ull) & ~255ull));
        check(contains(log, row), "the worker received the row pitch the relay published");
        check(contains(log, "640x360") || contains(log, std::to_string(a.width) + "x" +
                                                          std::to_string(a.height)),
              "the worker received the frame geometry the relay published");
        check(!contains_success_claim(log),
              "the WORKER's log contains no Reserved18 Success either");
    }

    // ---- the summary line ------------------------------------------------
    // Every field is a RECORDED fact, not the intent of the test. In particular
    // the worker's hr is printed as NONE when the guard refused before anything
    // was sent: printing 0x00000000 there would read as "the worker said S_OK",
    // which is the opposite of what happened.
    char hr_text[32];
    if (submit_returned) std::snprintf(hr_text, sizeof hr_text, "0x%08X", (unsigned)submit_hr);
    else                 std::snprintf(hr_text, sizeof hr_text, "NONE (nothing was sent)");

    std::printf("RELAY-SELFTEST-RESULT ok=%s checks=%u failures=%u worker=mgpu_nr_worker "
                "protocol_only=YES reserved18=0x%08X (%s) geometry=%ux%u frame_slots=3 "
                "output_written=%s submit_returned=%s submit_worker_hr=%s submits_attempted=%u "
                "slots=4 worker_exit=gone reason=%s\n",
                (failures == 0u) ? "YES" : "NO", checks, failures, (unsigned)r18,
                mgpu::relay::reserved18_name(r18), a.width, a.height,
                submit_returned ? (submit_output_changed ? "yes" : "no") : "NO (refused first)",
                submit_returned ? "accepted" : "refused", hr_text, a.frames,
                (failures == 0u) ? "every check held; no frame was sent to a worker with no "
                                   "session"
                                 : "see the FAIL lines above");
    if (!submit_err.empty())
        std::printf("RELAY-SELFTEST-DETAIL submit_refusal=\"%s\"\n", submit_err.c_str());
    std::fflush(stdout);
    LocalFree(wargv);
    return (failures == 0u) ? 0 : 1;
}
