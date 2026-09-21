// ============================================================================
// nr_slot_client.exe - a CLIENT THAT ACTUALLY MOVES PIXELS THROUGH THE WORKER.
//
// WHY IT EXISTS
//     `nr_ipc_probe` proves the control channel and deliberately sends no
//     payload; `--prove-frames` evaluates a frame the WORKER invents. Neither
//     proves the thing the game needs: a frame that crosses from a client
//     process into the worker, gets evaluated, and comes back. This tool is that
//     proof, and it is the shape the game-side add-on will copy - which is why it
//     lives in this tree rather than being written twice.
//
// THE CARRIER IS HOST MEMORY, AND THE PROTOCOL'S PROSE IS STALE ABOUT IT
//     nr_proto.h describes the surfaces as cross-adapter shared GPU resources.
//     That is the design v1 was written for and it cannot work here: both
//     adapters report CrossNodeSharingTier = 0 and a copy INTO a shared
//     cross-adapter surface carries nothing (transport-benchmark.json, ok=false).
//     So the four slots are NAMED FILE MAPPINGS - the same names, the same
//     geometry fields, the same CONFIG and FRAME_SUBMIT messages - and the
//     payload crosses through system memory. A memcpy cannot fail to carry bytes.
//
// WHAT IT CHECKS, IN ORDER
//     1. connect, HELLO, WORKER_READY  - and print the worker's Reserved18 result
//     2. CONFIG for all four slots     - the names the worker will open
//     3. per frame: fill the slot with a frame-specific pattern, submit, wait for
//        FRAME_COMPLETE, then hash the OUTPUT slot. The output is pre-filled with
//        a SENTINEL before the submit, so "the worker wrote the output" is
//        distinguishable from "the output still holds what the client put there".
//     4. SHUTDOWN
//
// WHAT IT DOES NOT DO
//     It does not present, it does not know about a game, and it does not judge
//     image quality. It answers one question: does a frame cross and come back.
// ============================================================================

#include "nr_pipe.h"
#include "nr_proto.h"
#include "nr_slots.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    const unsigned PITCH_ALIGN = 256u;   // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

    struct Args
    {
        const char   *pipe = nullptr;
        std::uint32_t worker_pid = 0;
        unsigned      frames = 4;
        unsigned      width = 640;
        unsigned      height = 360;
        const char   *json = nullptr;
        const char   *keep_names = nullptr;   // prefix for the mapping names
        bool          expect_protocol_only = false;
        bool          help = false;
    };

    void usage()
    {
        std::fprintf(stderr,
            "nr_slot_client - drive the worker's host-staged frame path end to end.\n"
            "\n"
            "  --pipe <name>       the worker's pipe, e.g. \\\\.\\pipe\\MGPU_NR_1234\n"
            "  --worker-pid <n>    build the default pipe name from this pid\n"
            "  --frames <n>        frames to submit (default 4)\n"
            "  --width <n>         frame width (default 640 - the proven control size)\n"
            "  --height <n>        frame height (default 360)\n"
            "  --names <prefix>    mapping name prefix (default Local\\MGPU_NR_SLOT_<pid>)\n"
            "  --expect-protocol-only\n"
            "                      the worker has no Reserved18 session on purpose\n"
            "                      (--protocol-only), so stop cleanly at the gate\n"
            "                      instead of calling that a failure. This is what\n"
            "                      makes the client's control path testable with no\n"
            "                      GPU at all.\n");
    }

    struct Slot
    {
        nr::InputSlot  which = nr::InputSlot::COLOR;
        unsigned       dxgi_format = 0;
        unsigned       bytes_per_pixel = 0;
        unsigned       row_pitch = 0;
        unsigned       slice_pitch = 0;
        unsigned long long bytes = 0;
        char           name[64] = {};
        HANDLE         mapping = nullptr;
        unsigned char *view = nullptr;
    };

    unsigned bytes_per_pixel(unsigned fmt)
    {
        switch (fmt)
        {
        case 28u: return 4u;   // DXGI_FORMAT_R8G8B8A8_UNORM   - COLOR and OUTPUT
        case 40u: return 4u;   // DXGI_FORMAT_D32_FLOAT         - DEPTH
        case 34u: return 4u;   // DXGI_FORMAT_R16G16_FLOAT      - MOTION_VECTORS
        default:  return 0u;
        }
    }

    unsigned long long fnv1a_rows(const unsigned char *base, unsigned pitch, unsigned row_bytes,
                                  unsigned height)
    {
        unsigned long long acc = 1469598103934665603ull;
        for (unsigned y = 0; y < height; ++y)
        {
            const unsigned char *row = base + (std::size_t)y * pitch;
            for (unsigned i = 0; i < row_bytes; ++i)
            {
                acc ^= (unsigned long long)row[i];
                acc *= 1099511628211ull;
            }
        }
        return acc;
    }

    bool make_slot(Slot &s, const char *prefix, unsigned w, unsigned h, std::string &err)
    {
        s.bytes_per_pixel = bytes_per_pixel(s.dxgi_format);
        if (s.bytes_per_pixel == 0u) { err = "no bytes-per-pixel for the slot format"; return false; }
        s.row_pitch = (w * s.bytes_per_pixel + PITCH_ALIGN - 1u) & ~(PITCH_ALIGN - 1u);
        s.slice_pitch = s.row_pitch * h;
        s.bytes = (unsigned long long)s.slice_pitch * nr::FRAME_SLOT_COUNT;

        std::snprintf(s.name, sizeof s.name, "%s_%s", prefix,
                      nr::input_slot_metadata_name(s.which));

        // A pagefile-backed section, named, so the worker opens the SAME bytes.
        wchar_t wide[128] = L"";
        for (int i = 0; i < 127 && s.name[i] != '\0'; ++i) wide[i] = (wchar_t)s.name[i];

        s.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       (DWORD)(s.bytes >> 32), (DWORD)(s.bytes & 0xFFFFFFFFull),
                                       wide);
        if (s.mapping == nullptr)
        {
            err = "CreateFileMappingW failed for a slot";
            return false;
        }
        s.view = (unsigned char *)MapViewOfFile(s.mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                               0, 0, 0);
        if (s.view == nullptr) { err = "MapViewOfFile failed for a slot"; return false; }
        std::memset(s.view, 0, (std::size_t)s.bytes);
        return true;
    }

    //: A pattern that differs per frame and per class, so a stale read, a wrong
    //: offset and a wrong pitch all produce a different hash rather than the same
    //: plausible-looking one.
    void fill_slot(const Slot &s, unsigned frame_index, unsigned char seed)
    {
        for (unsigned slot = 0; slot < nr::FRAME_SLOT_COUNT; ++slot)
        {
            unsigned char *base = s.view + (std::size_t)slot * s.slice_pitch;
            for (unsigned y = 0; y < s.slice_pitch / s.row_pitch; ++y)
            {
                unsigned char *row = base + (std::size_t)y * s.row_pitch;
                for (unsigned x = 0; x < s.row_pitch; ++x)
                {
                    row[x] = (unsigned char)((x * 31u + y * 17u + frame_index * 7u + seed * 53u) &
                                             0xFFu);
                }
            }
        }
    }

    void fill_sentinel(const Slot &s, unsigned slot_index)
    {
        std::memset(s.view + (std::size_t)slot_index * s.slice_pitch, 0xA5,
                    (std::size_t)s.slice_pitch);
    }

    //: The hash of a region that is uniformly 0xA5, over its LOGICAL bytes only.
    //: Comparison target for "did the worker write the output at all": every row of
    //: the sentinel holds the same byte, so the pitch does not enter into it.
    unsigned long long sentinel_hash(unsigned long long logical_bytes)
    {
        unsigned long long acc = 1469598103934665603ull;
        for (unsigned long long i = 0; i < logical_bytes; ++i)
        {
            acc ^= 0xA5ull;
            acc *= 1099511628211ull;
        }
        return acc;
    }
}

int main(int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        const char *s = argv[i];
        if (std::strcmp(s, "--pipe") == 0 && i + 1 < argc)        a.pipe = argv[++i];
        else if (std::strcmp(s, "--worker-pid") == 0 && i + 1 < argc)
            a.worker_pid = (std::uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(s, "--frames") == 0 && i + 1 < argc)
            a.frames = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(s, "--width") == 0 && i + 1 < argc)
            a.width = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(s, "--height") == 0 && i + 1 < argc)
            a.height = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(s, "--names") == 0 && i + 1 < argc)   a.keep_names = argv[++i];
        else if (std::strcmp(s, "--expect-protocol-only") == 0)    a.expect_protocol_only = true;
        else if (std::strcmp(s, "--help") == 0 || std::strcmp(s, "-h") == 0) a.help = true;
        else
        {
            std::fprintf(stderr, "nr_slot_client: unrecognised argument \"%s\"\n", s);
            usage();
            return 2;
        }
    }
    if (a.help) { usage(); return 0; }
    if (a.frames == 0u) a.frames = 1u;
    if (a.frames > 1000u) a.frames = 1000u;
    if (a.width == 0u) a.width = 640u;
    if (a.height == 0u) a.height = 360u;

    wchar_t pipe_name[256] = L"";
    if (a.pipe != nullptr)
    {
        for (int i = 0; i < 255 && a.pipe[i] != '\0'; ++i) pipe_name[i] = (wchar_t)a.pipe[i];
    }
    else
    {
        nr::pipe_name_for_pid(a.worker_pid, pipe_name, 256);
    }

    char prefix[64] = {};
    if (a.keep_names != nullptr)
    {
        std::snprintf(prefix, sizeof prefix, "%s", a.keep_names);
    }
    else
    {
        std::snprintf(prefix, sizeof prefix, "MGPU_NR_SLOT_%lu",
                      (unsigned long)GetCurrentProcessId());
    }

    std::printf("nr_slot_client -> %ls   %ux%u  names=%s*\n", pipe_name, a.width, a.height,
                prefix);
    std::fflush(stdout);

    std::string err;
    Slot slots[(unsigned)nr::InputSlot::COUNT];
    slots[(unsigned)nr::InputSlot::COLOR].which = nr::InputSlot::COLOR;
    slots[(unsigned)nr::InputSlot::COLOR].dxgi_format = 28u;
    slots[(unsigned)nr::InputSlot::DEPTH].which = nr::InputSlot::DEPTH;
    slots[(unsigned)nr::InputSlot::DEPTH].dxgi_format = 40u;
    slots[(unsigned)nr::InputSlot::MOTION_VECTORS].which = nr::InputSlot::MOTION_VECTORS;
    slots[(unsigned)nr::InputSlot::MOTION_VECTORS].dxgi_format = 34u;
    slots[(unsigned)nr::InputSlot::OUTPUT].which = nr::InputSlot::OUTPUT;
    slots[(unsigned)nr::InputSlot::OUTPUT].dxgi_format = 28u;

    for (unsigned i = 0; i < (unsigned)nr::InputSlot::COUNT; ++i)
    {
        if (!make_slot(slots[i], prefix, a.width, a.height, err))
        {
            std::printf("SLOTCLIENT-RESULT ok=NO stage=slots detail=\"%s\"\n", err.c_str());
            return 1;
        }
        std::printf("[slots]  %-16s %s  row_pitch=%u slice_pitch=%u bytes=%llu\n",
                    nr::input_slot_metadata_name(slots[i].which), slots[i].name,
                    slots[i].row_pitch, slots[i].slice_pitch, slots[i].bytes);
    }

    nr::Pipe link;
    if (!link.connect(pipe_name, 20000, err))
    {
        std::printf("SLOTCLIENT-RESULT ok=NO stage=connect detail=\"%s\"\n", err.c_str());
        return 1;
    }

    nr::Hello hello;
    nr::init(hello, nr::Kind::HELLO, 0u);
    hello.client_pid = (std::uint32_t)GetCurrentProcessId();
    hello.client_protocol_version = nr::PROTO_VERSION;
    hello.client_build = 1u;
    hello.wants_transport = 0u;   // host-staged: the pipe describes mappings, not D3D12 surfaces
    nr::set_text(hello.client_name, nr::PROTO_NAME_MAX, "nr_slot_client");
    if (link.write_message(hello, 4000, err) != nr::IoResult::OK)
    {
        std::printf("SLOTCLIENT-RESULT ok=NO stage=hello-write detail=\"%s\"\n", err.c_str());
        return 1;
    }

    nr::Error inbound;
    nr::IoResult r = link.read_message(inbound, 20000, err);
    if (r != nr::IoResult::OK || inbound.h.kind != (std::uint16_t)nr::Kind::WORKER_READY)
    {
        std::printf("SLOTCLIENT-RESULT ok=NO stage=worker-ready detail=\"%s kind=%u\"\n",
                    err.c_str(), (unsigned)inbound.h.kind);
        return 1;
    }
    nr::WorkerReady ready;
    std::memcpy(&ready, &inbound, sizeof(ready));
    std::printf("[worker] reserved18=0x%08X descriptor=%d cumodule=%d blob=%llu device=0x%04X "
                "name=\"%s\"\n",
                (unsigned)ready.reserved18, ready.descriptor_status, ready.cumodule_status,
                ready.first_blob_size, ready.adapter_device, ready.worker_name);
    if (ready.reserved18 != 1)
    {
        // A protocol-only worker reports -1: it never created a session, and it
        // says so. Under --expect-protocol-only that is the honest answer, so the
        // client stops here - after proving what the control path can prove with
        // no GPU at all: the four mappings were published under the documented
        // names, the HELLO was answered, and the answer was READ rather than
        // assumed. Without the flag, a worker that has no session is a failure,
        // because the frames that follow would be frames nobody evaluated.
        if (a.expect_protocol_only && ready.reserved18 == -1)
        {
            nr::Shutdown sd;
            nr::init(sd, nr::Kind::SHUTDOWN, 0u);
            sd.reason = 0u;
            (void)link.write_message(sd, 4000, err);
            link.close();
            std::printf("SLOTCLIENT-RESULT ok=YES protocol_only=YES frames_requested=0 "
                        "frames_completed=0 reserved18=-1\n");
            return 0;
        }
        std::printf("SLOTCLIENT-RESULT ok=NO stage=reserved18 reserved18=0x%08X\n",
                    (unsigned)ready.reserved18);
        return 1;
    }

    for (unsigned i = 0; i < (unsigned)nr::InputSlot::COUNT; ++i)
    {
        const Slot &s = slots[i];
        nr::Config cfg;
        nr::init(cfg, nr::Kind::CONFIG, 0u);
        cfg.input_slot = (std::uint32_t)s.which;
        cfg.width = a.width;
        cfg.height = a.height;
        cfg.dxgi_format = s.dxgi_format;
        cfg.row_pitch = s.row_pitch;
        cfg.slice_pitch = s.slice_pitch;
        cfg.resource_offset = 0u;
        cfg.flags = 0u;
        cfg.frame_slots = nr::FRAME_SLOT_COUNT;
        nr::set_text(cfg.shared_name, nr::PROTO_NAME_MAX, s.name);
        if (link.write_message(cfg, 4000, err) != nr::IoResult::OK)
        {
            std::printf("SLOTCLIENT-RESULT ok=NO stage=config-%u detail=\"%s\"\n", i, err.c_str());
            return 1;
        }
    }
    std::printf("[ipc]    sent %u CONFIG message(s)\n", (unsigned)nr::InputSlot::COUNT);

    unsigned completed = 0u;
    unsigned long long first_in_hash = 0ull, first_out_hash = 0ull, first_sentinel_hash = 0ull;
    // What a slot the worker never wrote hashes to. Computed once, from the same
    // row-by-row rule the real hash uses, so the comparison means something.
    const unsigned long long expected_sentinel =
        sentinel_hash((unsigned long long)a.width * 4ull * (unsigned long long)a.height);
    std::int32_t last_hr = 0;

    for (unsigned f = 0; f < a.frames; ++f)
    {
        const unsigned slot = f % nr::FRAME_SLOT_COUNT;

        fill_slot(slots[(unsigned)nr::InputSlot::COLOR], f, 1u);
        fill_slot(slots[(unsigned)nr::InputSlot::DEPTH], f, 2u);
        fill_slot(slots[(unsigned)nr::InputSlot::MOTION_VECTORS], f, 3u);
        fill_sentinel(slots[(unsigned)nr::InputSlot::OUTPUT], slot);

        const unsigned long long in_hash =
            fnv1a_rows(slots[(unsigned)nr::InputSlot::COLOR].view +
                           (std::size_t)slot * slots[(unsigned)nr::InputSlot::COLOR].slice_pitch,
                       slots[(unsigned)nr::InputSlot::COLOR].row_pitch,
                       a.width * 4u, a.height);

        nr::FrameSubmit sub;
        nr::init(sub, nr::Kind::FRAME_SUBMIT, f + 1u);
        sub.slot = slot;
        sub.input_mask = 0x7u;   // COLOR | DEPTH | MOTION_VECTORS (OUTPUT is the result)
        sub.width = a.width;
        sub.height = a.height;
        sub.dxgi_format = slots[(unsigned)nr::InputSlot::COLOR].dxgi_format;
        sub.row_pitch = slots[(unsigned)nr::InputSlot::COLOR].row_pitch;
        sub.slice_pitch = slots[(unsigned)nr::InputSlot::COLOR].slice_pitch;
        sub.resource_offset = slot * slots[(unsigned)nr::InputSlot::COLOR].slice_pitch;
        sub.ingress_fence_value = 0ull;   // host-staged: nothing to wait for on the GPU

        if (link.write_message(sub, 20000, err) != nr::IoResult::OK)
        {
            std::printf("SLOTCLIENT-RESULT ok=NO stage=submit-%u detail=\"%s\"\n", f, err.c_str());
            return 1;
        }

        nr::FrameComplete done;
        r = link.read_message(done, 60000, err);
        if (r != nr::IoResult::OK || done.h.kind != (std::uint16_t)nr::Kind::FRAME_COMPLETE)
        {
            std::printf("SLOTCLIENT-RESULT ok=NO stage=complete-%u detail=\"%s kind=%u\"\n", f,
                        err.c_str(), (unsigned)done.h.kind);
            return 1;
        }
        last_hr = done.hr;

        const unsigned long long out_hash =
            fnv1a_rows(slots[(unsigned)nr::InputSlot::OUTPUT].view +
                           (std::size_t)slot * slots[(unsigned)nr::InputSlot::OUTPUT].slice_pitch,
                       slots[(unsigned)nr::InputSlot::OUTPUT].row_pitch, a.width * 4u, a.height);
        if (f == 0u)
        {
            first_in_hash = in_hash;
            first_out_hash = out_hash;
            first_sentinel_hash = expected_sentinel;
        }
        if (done.hr == 0) ++completed;

        std::printf("[frame]  %u slot=%u hr=0x%08X in_hash=0x%016llX out_hash=0x%016llX "
                    "output_was_written=%s egress_fence=%llu\n",
                    f, slot, (unsigned)done.hr, in_hash, out_hash,
                    (out_hash != expected_sentinel) ? "yes" : "NO (still the sentinel)",
                    done.egress_fence_value);
        std::fflush(stdout);
    }

    nr::Shutdown sd;
    nr::init(sd, nr::Kind::SHUTDOWN, 0u);
    sd.reason = 0u;
    (void)link.write_message(sd, 4000, err);
    link.close();

    std::printf("SLOTCLIENT-RESULT ok=%s frames_requested=%u frames_completed=%u last_hr=0x%08X "
                "reserved18=0x%08X first_in_hash=0x%016llX first_out_hash=0x%016llX "
                "sentinel=0x%016llX output_written=%s\n",
                (completed == a.frames) ? "YES" : "NO", a.frames, completed, (unsigned)last_hr,
                (unsigned)ready.reserved18, first_in_hash, first_out_hash, first_sentinel_hash,
                (first_out_hash != first_sentinel_hash) ? "YES" : "NO");
    return (completed == a.frames) ? 0 : 1;
}
