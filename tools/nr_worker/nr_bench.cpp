// ============================================================================
// nr_transport_bench.exe - the STANDALONE two-GPU round-trip benchmark.
//
// WHAT IT MEASURES
//     The four legs of the transport, on real hardware, with real cross-adapter
//     resources and real fences - and nothing else. NR is not involved: no NGX,
//     no NVAPI, no DLSSNR. That separation is the point. When this benchmark is
//     green, a failure in the neural worker is a failure of the neural worker and
//     not of the transport.
//
//         Ti SUPER local COLOR  --[leg 0]-->  shared cross-adapter
//         shared cross-adapter  --[leg 1]-->  RTX 4070 local COLOR
//         RTX 4070 local COLOR  --[leg 2]-->  shared cross-adapter (OUTPUT)
//         shared cross-adapter  --[leg 3]-->  Ti SUPER local OUTPUT
//
//     A deterministic pattern is seeded into the Ti SUPER's COLOR texture, and
//     the Ti SUPER's OUTPUT texture is read back and hashed at the end. Exact
//     match is the acceptance criterion; the hashes are in the report either way.
//
// WHY IT IS RUNNABLE BY HAND
//     GitHub's runners have no NVIDIA adapter, so this benchmark cannot run in
//     CI. It is built and gated in CI (it must compile, and --list-adapters must
//     refuse to invent an adapter), and RUN-NR-WORKER.cmd runs it on the real
//     machine and writes transport-benchmark.json.
//
// EVERY REFUSAL IS REPORTED WITH ITS HRESULT
//     If D3D12 will not give us a cross-adapter buffer, or a cross-adapter fence,
//     or a GPU-side wait on one, the exact HRESULT and the call that produced it
//     go into the report. A benchmark that cannot measure something says so.
// ============================================================================

#include "nr_json.h"
#include "nr_transport.h"

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
        unsigned width = 0;
        unsigned height = 0;
        bool     all_resolutions = false;
        unsigned frames = 30;
        unsigned slots = 3;
        const char *json = "transport-benchmark.json";
        bool     list_adapters = false;
        bool     no_verify = false;
        bool     help = false;
        nr::TransportOptions::SharedPreference pref =
            nr::TransportOptions::SharedPreference::AUTO;
    };

    void usage()
    {
        std::fprintf(stderr,
            "nr_transport_bench - the standalone cross-adapter round trip.\n"
            "\n"
            "  --resolution <WxH>   one resolution (default: the four acceptance sizes)\n"
            "  --frames <n>         frames per resolution (default 30)\n"
            "  --slots <n>          frame slots in flight (default 3)\n"
            "  --json <path>        the report (default transport-benchmark.json)\n"
            "  --no-verify          skip the readback/hash check (timings only)\n"
            "  --shared-kind <k>    auto|buffer|texture (default auto: buffer, then texture\n"
            "                       ONLY if creation fails - which is not the same as the\n"
            "                       buffer failing to CARRY anything)\n"
            "  --list-adapters      enumerate adapters and exit; no device is created\n"
            "\n"
            "The four acceptance resolutions are 1280x720, 1920x1080, 2560x1440 and\n"
            "3840x2160; each is run when --resolution is not given.\n");
    }

    bool parse_resolution(const char *s, unsigned &w, unsigned &h)
    {
        unsigned a = 0, b = 0;
        if (std::sscanf(s, "%ux%u", &a, &b) != 2) return false;
        if (a < 16 || b < 16 || a > 16384 || b > 16384) return false;
        w = a;
        h = b;
        return true;
    }

    void write_stats(nr::Json &j, const char *key, const nr::Stats &s)
    {
        j.key(key);
        if (s.n == 0)
        {
            // Not "0.000": a stage that produced no sample produced no sample.
            j.value_null();
            return;
        }
        j.begin_object();
        j.kv("n", (std::uint64_t)s.n);
        j.kv("min", s.min);
        j.kv("median", s.median);
        j.kv("p95", s.p95);
        j.kv("max", s.max);
        j.kv("mean", s.mean);
        j.end_object();
    }

    void write_adapters(nr::Json &j, const nr::AdapterReport &a)
    {
        j.begin_object();
        j.kv("found", a.found);
        if (a.found)
        {
            char pci[32];
            std::snprintf(pci, sizeof pci, "%04X:%04X", a.vendor, a.device);
            char luid[32];
            std::snprintf(luid, sizeof luid, "%08X:%08X", a.luid_high, a.luid_low);
            // The wide description is written as UTF-8 in the narrow report.
            char name[256] = "";
            WideCharToMultiByte(CP_UTF8, 0, a.name, -1, name, sizeof name, nullptr, nullptr);
            j.kv("name", name);
            j.kv("pci", pci);
            j.kv("luid", luid);
            j.kv("dedicated_video_memory_mib",
                 (std::uint64_t)(a.dedicated_video_memory / (1024ull * 1024ull)));
            j.kv("cross_node_sharing_tier", (std::uint64_t)a.cross_node_tier);
            j.kv("cross_adapter_row_major_texture", a.row_major_cross_adapter_texture);
        }
        j.end_object();
    }
}

int main(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        const char *s = argv[i];
        if (std::strcmp(s, "--resolution") == 0 && i + 1 < argc)
        {
            if (!parse_resolution(argv[++i], a.width, a.height))
            {
                std::fprintf(stderr, "nr_transport_bench: --resolution wants WxH\n");
                return 2;
            }
        }
        else if (std::strcmp(s, "--frames") == 0 && i + 1 < argc)  a.frames = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(s, "--slots") == 0 && i + 1 < argc)   a.slots = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(s, "--json") == 0 && i + 1 < argc)    a.json = argv[++i];
        else if (std::strcmp(s, "--no-verify") == 0)               a.no_verify = true;
        else if (std::strcmp(s, "--shared-kind") == 0 && i + 1 < argc)
        {
            const char *k = argv[++i];
            if (std::strcmp(k, "buffer") == 0)
                a.pref = nr::TransportOptions::SharedPreference::BUFFER;
            else if (std::strcmp(k, "texture") == 0)
                a.pref = nr::TransportOptions::SharedPreference::TEXTURE;
            else if (std::strcmp(k, "auto") == 0)
                a.pref = nr::TransportOptions::SharedPreference::AUTO;
            else
            {
                std::fprintf(stderr, "nr_transport_bench: --shared-kind wants auto|buffer|texture\n");
                return 2;
            }
        }
        else if (std::strcmp(s, "--list-adapters") == 0)           a.list_adapters = true;
        else if (std::strcmp(s, "--help") == 0 || std::strcmp(s, "-h") == 0) a.help = true;
        else
        {
            std::fprintf(stderr, "nr_transport_bench: unrecognised argument \"%s\"\n", s);
            usage();
            return 2;
        }
    }
    if (a.help) { usage(); return 0; }
    if (a.frames == 0) a.frames = 1;
    if (a.frames > 600) a.frames = 600;
    if (a.slots == 0) a.slots = 3;
    if (a.slots > 3) a.slots = 3;      // v1 is three frames in flight, by design

    if (a.list_adapters)
    {
        std::vector<nr::AdapterReport> adapters;
        std::string err;
        if (!nr::Transport::enumerate_adapters(adapters, err))
        {
            std::fprintf(stderr, "nr_transport_bench: %s\n", err.c_str());
            return 1;
        }
        std::printf("adapters: %u\n", (unsigned)adapters.size());
        unsigned nvidia = 0, ti_super = 0, rtx4070 = 0;
        for (std::size_t i = 0; i < adapters.size(); ++i)
        {
            char name[256] = "";
            WideCharToMultiByte(CP_UTF8, 0, adapters[i].name, -1, name, sizeof name, nullptr, nullptr);
            std::printf("  %04X:%04X  luid=%08X:%08X  %s\n", adapters[i].vendor,
                        adapters[i].device, adapters[i].luid_high, adapters[i].luid_low, name);
            if (adapters[i].vendor == 0x10DEu) ++nvidia;
            if (adapters[i].vendor == 0x10DEu && adapters[i].device == 0x2705u) ++ti_super;
            if (adapters[i].vendor == 0x10DEu && adapters[i].device == 0x2786u) ++rtx4070;
        }
        std::printf("nvidia=%u ti_super(10DE:2705)=%u rtx4070(10DE:2786)=%u\n",
                    nvidia, ti_super, rtx4070);
        std::printf("NR-BENCH-ADAPTERS nvidia=%u ti_super=%u rtx4070=%u\n", nvidia, ti_super,
                    rtx4070);
        // The machine is usable for the benchmark only when BOTH named adapters
        // are present. A different exit code says "this rig is not the rig this
        // benchmark is for", which is what CI asserts on a GPU-less runner.
        return (ti_super >= 1 && rtx4070 >= 1) ? 0 : 3;
    }

    std::vector<std::pair<unsigned, unsigned> > resolutions;
    if (a.width != 0 && a.height != 0)
    {
        resolutions.push_back(std::make_pair(a.width, a.height));
    }
    else
    {
        resolutions.push_back(std::make_pair(1280u, 720u));
        resolutions.push_back(std::make_pair(1920u, 1080u));
        resolutions.push_back(std::make_pair(2560u, 1440u));
        resolutions.push_back(std::make_pair(3840u, 2160u));
    }

    nr::Json report;
    report.begin_object();
    report.kv("tool", "nr_transport_bench");
    report.kv("purpose", "the four legs of the GPU-only cross-adapter transport, measured "
                         "separately; NR is NOT involved in this benchmark");
    report.kv("claim", "transport only: this says nothing about the neural session, the NGX "
                       "runtime, the vendor API or the worker that owns them");
    report.kv("frames_per_resolution", (std::uint64_t)a.frames);
    report.kv("frame_slots", (std::uint64_t)a.slots);
    report.key("legs");
    report.begin_array();
    for (unsigned i = 0; i < 4; ++i) report.value_string(nr::leg_name(i));
    report.end_array();

    report.key("resolutions");
    report.begin_array();

    int rc = 0;
    for (std::size_t r = 0; r < resolutions.size(); ++r)
    {
        const unsigned w = resolutions[r].first;
        const unsigned h = resolutions[r].second;
        std::printf("=== %ux%u ===\n", w, h);
        std::fflush(stdout);

        nr::TransportOptions opt;
        opt.width = w;
        opt.height = h;
        opt.frames = a.frames;
        opt.slot_count = a.slots;
        opt.verify = !a.no_verify;
        opt.preference = a.pref;

        nr::Transport transport;
        nr::TransportResult res;
        std::string err;

        const bool opened = transport.open(opt, err);
        const bool ran = opened && transport.run(opt, res);
        if (!opened)
        {
            // THE REFUSALS ARE THE RESULT. When the shared surfaces cannot be
            // created, the report has to carry the HRESULT and the call that
            // produced it - otherwise it says "it did not work" and nothing else,
            // which is not a measurement. The adapter reports are collected on the
            // same principle: whatever was learned before the failure.
            res.unsupported = transport.failures();
            transport.adapter_reports(res.game, res.worker);
            res.note = res.note.empty()
                ? "the transport could not be OPENED, so nothing was measured"
                : res.note;
        }
        transport.close();

        report.begin_object();
        report.kv("width", (std::uint64_t)w);
        report.kv("height", (std::uint64_t)h);
        report.kv("opened", opened);
        report.kv("ran", ran);
        report.kv("supported", res.supported);
        report.kv("shared_kind", nr::shared_kind_name(res.kind));
        report.kv("fence_mode", res.fence_mode.empty() ? "unknown" : res.fence_mode.c_str());
        report.kv("slot_waits_gpu", (std::uint64_t)res.slot_waits_gpu);
        report.kv("slot_waits_cpu", (std::uint64_t)res.slot_waits_cpu);
        report.kv("frames_requested", (std::uint64_t)res.frames_requested);
        report.kv("frames_completed", (std::uint64_t)res.frames_completed);
        report.kv("payload_bytes", res.payload_bytes);
        report.kv("cross_adapter_heap_bytes_for_this_resolution", res.shared_bytes_per_slot);
        if (!res.note.empty()) report.kv("note", res.note.c_str());
        report.key("game_adapter");
        write_adapters(report, res.game);
        report.key("worker_adapter");
        write_adapters(report, res.worker);

        report.key("stages_ms");
        report.begin_object();
        for (unsigned i = 0; i < 4; ++i) write_stats(report, nr::leg_name(i), res.leg[i]);
        report.end_object();
        write_stats(report, "end_to_end_ms", res.total);

        report.kv("seed_sha256", res.seed_sha256.c_str());
        report.kv("returned_sha256", res.returned_sha256.c_str());
        report.kv("exact_match", res.exact_match);
        report.kv("verified_frames", (std::uint64_t)res.verified_frames);

        // The chain, joint by joint. `first_broken_joint` is empty when the round
        // trip is intact, and names the leg that failed when it is not.
        report.key("chain_sha256");
        report.begin_object();
        report.kv("seed_landed_local_color", res.seed_landed_sha256.c_str());
        report.kv("ingress_shared", res.ingress_shared_sha256.c_str());
        report.kv("egress_shared", res.egress_shared_sha256.c_str());
        report.kv("returned_local_output", res.returned_sha256.c_str());
        report.end_object();
        report.kv("first_broken_joint", res.first_broken_joint.c_str());

        report.key("untimed_slot_checks");
        report.begin_array();
        for (std::size_t i = 0; i < res.untimed_slot_checks.size(); ++i)
        {
            report.value_string(res.untimed_slot_checks[i].c_str());
        }
        report.end_array();

        report.key("unsupported");
        report.begin_array();
        for (std::size_t i = 0; i < res.unsupported.size(); ++i)
        {
            report.begin_object();
            report.kv("stage", res.unsupported[i].stage.c_str());
            report.kv("hr", res.unsupported[i].hr);
            report.kv("hr_hex", res.unsupported[i].hr_hex().c_str());
            report.kv("note", res.unsupported[i].note.c_str());
            report.end_object();
        }
        report.end_array();

        if (!opened)
        {
            report.kv("error", err.c_str());
            rc = 1;
        }
        else if (!ran)
        {
            report.kv("error", "the transport opened but the round trip did not run");
            rc = 1;
        }
        else if (!res.exact_match && opt.verify)
        {
            // A mismatch is the acceptance failure: the bytes that came back are
            // not the bytes that went in.
            report.kv("error", "the returned bytes are NOT the seeded bytes");
            rc = 1;
        }
        report.end_object();

        // Console summary: the four legs, median and p95, and the verdict.
        for (unsigned i = 0; i < 4; ++i)
        {
            std::printf("  %-26s median %8.3f ms   p95 %8.3f ms   max %8.3f ms   n=%u\n",
                        nr::leg_name(i), res.leg[i].median, res.leg[i].p95, res.leg[i].max,
                        res.leg[i].n);
        }
        std::printf("  %-26s median %8.3f ms   p95 %8.3f ms   max %8.3f ms   n=%u\n",
                    "end_to_end", res.total.median, res.total.p95, res.total.max, res.total.n);
        std::printf("  shared=%s fence=%s frames=%u/%u exact_match=%s\n",
                    nr::shared_kind_name(res.kind),
                    res.fence_mode.empty() ? "unknown" : res.fence_mode.c_str(),
                    res.frames_completed, res.frames_requested,
                    opt.verify ? (res.exact_match ? "YES" : "NO") : "(not verified)");
        if (opt.verify && !res.first_broken_joint.empty())
        {
            std::printf("  seed     =%s\n  returned =%s\n", res.seed_sha256.c_str(),
                        res.returned_sha256.c_str());
            std::printf("  FIRST BROKEN JOINT: %s\n", res.first_broken_joint.c_str());
        }
        for (std::size_t i = 0; i < res.unsupported.size(); ++i)
        {
            std::printf("  UNSUPPORTED %s hr=%s  %s\n", res.unsupported[i].stage.c_str(),
                        res.unsupported[i].hr_hex().c_str(), res.unsupported[i].note.c_str());
        }
        std::fflush(stdout);
    }

    report.end_array();
    report.kv("ok", rc == 0);
    report.end_object();

    std::string werr;
    if (!report.well_formed())
    {
        std::fprintf(stderr, "nr_transport_bench: refusing to write a malformed report\n");
        return 1;
    }
    if (!nr::write_text_file(a.json, report.str() + "\n", werr))
    {
        std::fprintf(stderr, "nr_transport_bench: %s\n", werr.c_str());
        return 1;
    }
    std::printf("nr_transport_bench: wrote %s\n", a.json);
    return rc;
}
