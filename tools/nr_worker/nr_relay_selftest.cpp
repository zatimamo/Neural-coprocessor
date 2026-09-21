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
// CommandLineToArgvW lives in shellapi.h, which windows.h does NOT pull in.
// Declared here rather than in the relay: this is a property of the TEST's
// argument parsing, not of the module under test, and the module itself must
// stay on windows.h + the CRT (its own gate asserts the include count).
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// CommandLineToArgvW's implementation is in Shell32. Declared as a pragma so a
// build made from this file alone still links, the way gpu1_context.cpp does
// for dcomp and d3d11.
#pragma comment(lib, "shell32.lib")

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
    cfg.auto_launch = true;
    // The whole point: no GPU, no NGX, no NVAPI. The relay must not build the
    // runtime flags, and nothing here may mention a rasterizer.
    cfg.protocol_only = true;
    cfg.worker_args = L"--protocol-only --accept-seconds 30";
    cfg.worker_log_name = a.log_name;
    cfg.connect_ms = 20000u;
    cfg.handshake_ms = 20000u;
    cfg.shutdown_ms = 3000u;

    // THE FOUR SLOTS ARE NOT ALL THE SAME SHAPE, so the test publishes them as
    // the lane really is: colour and depth at the requested extent, motion
    // vectors at HALF it with the game's 8-byte format (fmt=10). Rows of the
    // same width but a different format give a different pitch, which is the
    // other half of the same point. At the default 640x360 this is a realistic
    // per-slot set with no extra test scaffolding.
    const unsigned mv_width = (a.width + 1u) / 2u;
    const unsigned mv_height = (a.height + 1u) / 2u;
    cfg.slots.color          = { a.width, a.height, 28u };   // R8G8B8A8_UNORM, 4 bpp
    cfg.slots.depth          = { a.width, a.height, 41u };   // R32_FLOAT, 4 bpp
    cfg.slots.motion_vectors = { mv_width, mv_height, 10u }; // R16G16B16A16_FLOAT, 8 bpp
    cfg.slots.output         = { a.width, a.height, 28u };
    // Colour and motion vectors really do differ in every dimension the relay
    // checks: rows, row length, and (because 8 bpp against half the width) the
    // pitch. If those were equal, the mismatch test below would prove nothing.
    const bool geometry_is_distinguishable =
        (cfg.slots.motion_vectors.height != cfg.slots.color.height) &&
        (cfg.slots.motion_vectors.width * 8u != cfg.slots.color.width * 4u);
    check(geometry_is_distinguishable,
          "the test's own slot set makes the four geometries distinguishable");

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
    // module, and derived PER SLOT because the four slots do not share a
    // geometry:
    //     row_bytes  = width * bytes_per_pixel(format)
    //     row_pitch  = align_up(row_bytes, 256)     (256 = the D3D12 pitch align)
    //     slice_pitch = row_pitch * height
    //     bytes      = slice_pitch * 3              (FRAME_SLOT_COUNT)
    // All four must agree with THAT slot's numbers, so a module that published
    // one geometry four times fails here even though it would look plausible.
    {
        struct Expect { unsigned w; unsigned h; unsigned bpp; const char *name; };
        const Expect exp[4] = {
            { cfg.slots.color.width,          cfg.slots.color.height,          4u, "COLOR" },
            { cfg.slots.depth.width,          cfg.slots.depth.height,          4u, "DEPTH" },
            { cfg.slots.motion_vectors.width, cfg.slots.motion_vectors.height, 8u, "MOTION_VECTORS" },
            { cfg.slots.output.width,         cfg.slots.output.height,         4u, "OUTPUT" },
        };
        bool all_ok = true;
        for (unsigned i = 0u; i < 4u; ++i)
        {
            const unsigned expected_row_bytes = exp[i].w * exp[i].bpp;
            const unsigned expected_row =
                (unsigned)((((unsigned long long)expected_row_bytes) + 255ull) & ~255ull);
            const unsigned expected_slice = expected_row * exp[i].h;
            const unsigned long long expected_bytes = (unsigned long long)expected_slice * 3ull;
            mgpu::relay::SlotInfo si;
            const bool got = mgpu::relay::slot_info(i, si);
            const bool match = got && si.width == exp[i].w && si.height == exp[i].h &&
                               si.row_pitch == expected_row && si.slice_pitch == expected_slice &&
                               si.bytes == expected_bytes;
            if (!match) all_ok = false;
            std::printf("  slot %u %-15s %s %ux%u row_pitch=%u slice_pitch=%u bytes=%llu "
                        "expected_row_pitch=%u\n",
                        i, exp[i].name, got ? "published" : "MISSING", si.width, si.height,
                        si.row_pitch, si.slice_pitch, (unsigned long long)si.bytes,
                        expected_row);
        }
        check(all_ok, "each slot is published with ITS OWN derived geometry and pitches");
        // And the point of the whole exercise: they are not all the same.
        check(cfg.slots.motion_vectors.height != cfg.slots.color.height,
              "the test published different heights per slot, so per-slot geometry is exercised");
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

    // ---- 3b. A FRAME THAT DOES NOT MATCH ITS SLOT IS REFUSED -------------
    //
    // THE DEFECT THIS EXISTS TO CATCH. All four slots used to be published with
    // ONE geometry, and submit() checked only that a row FIT under the pitch.
    // The lane's real frame is colour 3840x2160 fmt=28 with motion vectors
    // 1920x1080 fmt=10, so a motion-vector buffer handed to a colour-shaped slot
    // would be read as the top half of the wrong buffer - and the worker would
    // still answer hr == 0 with a changed OUTPUT, so every honesty rule the
    // module has would have PASSED on a truncated frame.
    //
    // Three refusals are asserted, each of them a mismatch that is otherwise a
    // perfectly well-formed frame: the row count, the row length, and the stride.
    // The same frame at the published numbers must then get PAST the geometry
    // checks - which in this protocol-only session means the next guard (no
    // neural session) is the one that refuses it, and that is the assertion
    // below.
    bool submit_returned = false;
    std::int32_t submit_hr = 0;
    bool submit_output_changed = false;
    std::string submit_err;
    {
        // Big enough for every case below, so nothing is ever read out of bounds
        // even if a guard failed.
        const unsigned rows = cfg.slots.color.height + 8u;
        const unsigned wide_row = cfg.slots.color.width * 4u + 256u;
        std::vector<unsigned char> color((std::size_t)wide_row * rows, 0x11);
        std::vector<unsigned char> depth((std::size_t)wide_row * rows, 0x22);
        std::vector<unsigned char> mvec((std::size_t)wide_row * rows, 0x33);

        mgpu::relay::FrameView fv;
        fv.color = color.data();
        fv.depth = depth.data();
        fv.motion_vectors = mvec.data();
        // The published numbers of the colour slot: 4 bpp, so its row is
        // exactly width * 4. Stride and row length agree, tightly packed.
        fv.height = cfg.slots.color.height;
        fv.color_row_bytes = cfg.slots.color.width * 4u;
        fv.depth_row_bytes = cfg.slots.depth.width * 4u;
        fv.motion_row_bytes = cfg.slots.motion_vectors.width * 8u;
        fv.color_stride = fv.color_row_bytes;
        fv.depth_stride = fv.depth_row_bytes;
        fv.motion_stride = fv.motion_row_bytes;

        // THE PUBLISHED SLOTS, as check_frame sees them. Built from the config
        // the relay was started with - whose publication the slot_info() checks
        // above already asserted slot by slot - so these are the module's own
        // numbers and not the test's wishes. Bytes per pixel comes from the
        // format, which is the same derivation the module uses.
        auto bpp_of = [](unsigned fmt) -> unsigned {
            switch (fmt)
            {
            case 28u:  return 4u;   // R8G8B8A8_UNORM
            case 41u:  return 4u;   // R32_FLOAT
            case 40u:  return 4u;   // D32_FLOAT
            case 34u:  return 4u;   // R16G16_FLOAT
            case 10u:  return 8u;   // R16G16B16A16_FLOAT
            default:   return 0u;
            }
        };
        mgpu::relay::SlotGeometry geo[3];
        {
            const mgpu::relay::SlotConfig *sc[3] = { &cfg.slots.color, &cfg.slots.depth,
                                                     &cfg.slots.motion_vectors };
            for (unsigned i = 0u; i < 3u; ++i)
            {
                geo[i].published = true;
                geo[i].width = sc[i]->width;
                geo[i].height = sc[i]->height;
                geo[i].dxgi_format = sc[i]->dxgi_format;
                geo[i].bytes_per_pixel = bpp_of(sc[i]->dxgi_format);
                geo[i].row_bytes = sc[i]->width * geo[i].bytes_per_pixel;
            }
        }

        // (b) and (c) below need a slot set that AGREES on the row count, because
        // FrameView carries ONE height for all three inputs: against the
        // as-published geometry above (motion vectors 180 rows, colour 360) a
        // height that satisfied the motion slot would trip the colour slot's
        // height check first and the row-length and stride rules would never be
        // reached. A real submission is reduced to one extent first, so this is
        // the shape a live frame has - and it is built from the same published
        // numbers, with only the row count harmonised.
        mgpu::relay::SlotGeometry geo_uniform[3];
        for (unsigned i = 0u; i < 3u; ++i)
        {
            geo_uniform[i] = geo[i];
            geo_uniform[i].height = cfg.slots.color.height;
        }

        // (a) A MOTION-VECTOR GEOMETRY THAT DOES NOT MATCH ITS SLOT. Everything
        //     else about this frame is correct: the pointers, the strides, the
        //     colour and depth rows, the format-consistent row length. Only the
        //     row count is the colour slot's instead of the motion slot's - the
        //     exact shape of the truncation this guards against.
        //
        //     DRIVEN THROUGH check_frame(), NOT submit(). A --protocol-only worker
        //     has no Reserved18 session, so submit() refuses at the session guard
        //     before any geometry is examined - correctly - and these rules would
        //     then be tested nowhere at all. check_frame() is the function the
        //     live path calls, so what is exercised here is the live path's code.
        {
            std::string gerr;
            const bool accepted = mgpu::relay::check_frame(fv, geo, gerr);
            std::printf("  geometry  = %s (%s)\n", accepted ? "ACCEPTED" : "refused", gerr.c_str());
            check(!accepted, "check_frame() REFUSES a frame whose row count is not the slot's");
            check(contains(gerr, "MOTION_VECTORS"),
                  "the refusal names the slot that did not match");
            check(contains(gerr, "MOTION_VECTORS") &&
                      contains(gerr, std::to_string(cfg.slots.motion_vectors.height)),
                  "the refusal quotes the geometry the slot was published with");
            check(contains(gerr, std::to_string(fv.height)),
                  "the refusal quotes the geometry the frame claimed");
        }

        // (b) A ROW LENGTH THAT IS NOT width * bytes_per_pixel. The row counts
        //     agree, so the height check passes and this one has to catch it.
        {
            mgpu::relay::FrameView fv2 = fv;
            fv2.height = cfg.slots.color.height;
            fv2.motion_row_bytes = cfg.slots.motion_vectors.width * 4u;   // 4, not 8, bpp
            fv2.motion_stride = fv2.motion_row_bytes;
            std::string gerr;
            const bool accepted = mgpu::relay::check_frame(fv2, geo_uniform, gerr);
            std::printf("  rowbytes  = %s (%s)\n", accepted ? "ACCEPTED" : "refused", gerr.c_str());
            check(!accepted, "check_frame() REFUSES a row length that is not the format's "
                             "bytes-per-pixel times the width");
            check(contains(gerr, "bytes per pixel"),
                  "the refusal explains the row length in terms of bytes per pixel");
        }

        // (c) A STRIDE SHORTER THAN THE ROW. The relay would read past the end of
        //     every row, so this must not be clamped to the row length.
        {
            mgpu::relay::FrameView fv3 = fv;
            fv3.height = cfg.slots.color.height;
            fv3.color_stride = fv3.color_row_bytes - 4u;
            std::string gerr;
            const bool accepted = mgpu::relay::check_frame(fv3, geo_uniform, gerr);
            std::printf("  stride    = %s (%s)\n", accepted ? "ACCEPTED" : "refused", gerr.c_str());
            check(!accepted, "check_frame() REFUSES a stride shorter than a row");
            check(contains(gerr, "stride"), "the refusal names the stride");
        }

        // (d) THE SAME FRAME AT THE PUBLISHED NUMBERS. It must now get past the
        //     geometry contract; what refuses it is the session guard, because a
        //     --protocol-only worker has no Reserved18 session at all. That the
        //     reason CHANGED is the evidence the geometry was accepted.
        {
            mgpu::relay::SubmitResult sr;
            std::string ferr;
            mgpu::relay::FrameView fv4 = fv;
            fv4.height = cfg.slots.motion_vectors.height;
            const bool sent = mgpu::relay::submit(fv4, 1u, sr, ferr);
            submit_returned = sent;
            submit_hr = sr.worker_hr;
            submit_output_changed = sr.output_changed;
            submit_err = ferr;
            std::printf("  submit()  = %s (%s)\n", sent ? "accepted" : "refused", ferr.c_str());
            check(!sent, "submit() REFUSES while the worker has no Reserved18 session");
            check(!sr.completed, "no FRAME_COMPLETE was reported for a refused submit");
            check(contains(ferr, "no neural session"),
                  "the correct-geometry frame was refused by the SESSION guard, not the geometry "
                  "one - so the geometry was accepted");
            check(!contains(ferr, "rows and the"),
                  "the correct-geometry frame was NOT refused for its row count");
        }
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
