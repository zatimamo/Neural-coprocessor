// ============================================================================
// MGPU out-of-process NR worker - CONTROL PROTOCOL, VERSION 1.
//
// WHAT THIS CHANNEL IS FOR
//     Low-frequency control and metadata between the game-side MGPU add-on and
//     mgpu_nr_worker.exe. The worker owns the RTX 4070 D3D12 device, the NGX
//     core, the architecture patch, the DLSSNR runtime and the Reserved18
//     session, because AutoLab proved that a second adapter's *active D3D12
//     state inside the game's process* is what breaks the private CUDA-interop
//     calls (descriptor -1, CuModule -1, Reserved18 0xBAD00002).
//
// WHAT NEVER GOES THROUGH IT
//     Pixel data. Not one byte, in either direction. Frames travel as
//     cross-adapter shared GPU resources whose NT handles are exchanged as
//     metadata; this channel carries frame ids, geometry, formats, pitches and
//     status codes only.
//
// FRAMING
//     Every message is a fixed, packed struct that BEGINS with Header:
//
//         magic       'M','G','P','U' in memory order
//         version     protocol version, currently 1
//         kind        Kind
//         size        the STRUCT SIZE on the wire, header included
//         frame_id    the frame this message belongs to, 0 when not frame-bound
//
//     A receiver refuses a message whose magic, version or size is not exactly
//     what version 1 defines for that kind. There is no "best effort" decode and
//     no variable-length tail: a mismatch is a protocol error, reported as ERROR
//     and fatal for the connection, because a mis-framed message on a shared
//     memory boundary is worse than a dropped connection.
//
//     The struct size check is the important one. `sizeof` is part of the
//     contract here, so the two sides cannot drift apart silently - which is why
//     static_assert pins it at compile time as well.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nr
{
    constexpr std::uint32_t PROTO_MAGIC = 0x5550474Du;   // 'M','G','P','U'
    constexpr std::uint16_t PROTO_VERSION = 1u;

    constexpr std::uint32_t PROTO_NAME_MAX = 64;
    constexpr std::uint32_t PROTO_TEXT_MAX = 192;
    constexpr std::uint32_t PROTO_MAX_MESSAGE = 4096;
    constexpr std::uint32_t PROTO_MAX_SLOTS = 8;
    constexpr std::uint32_t PROTO_FRAME_SLOTS = 3;

    //: Frame slots in flight. Three is the agreed starting point: enough for the
    //: game to copy frame N+1 while the worker still owns N, and few enough that
    //: the cross-adapter memory cost stays bounded.
    constexpr std::uint32_t FRAME_SLOT_COUNT = 3;

    enum class Kind : std::uint16_t
    {
        HELLO = 1,
        WORKER_READY = 2,
        CONFIG = 3,
        FRAME_SUBMIT = 4,
        FRAME_COMPLETE = 5,
        SHUTDOWN = 6,
        ERROR = 7,
    };

    const char *kind_name(Kind k);

    // -------------------------------------------------------------- messages

    // Every struct below is pack(1) and is sent as-is. The size field is
    // sizeof(the struct), so a build whose struct layout differs is refused by
    // the peer rather than misread.
#pragma pack(push, 1)

    struct Header
    {
        std::uint32_t magic;
        std::uint16_t version;
        std::uint16_t kind;
        std::uint32_t size;       // STRUCT SIZE on the wire, header included
        std::uint32_t frame_id;
    };
    static_assert(sizeof(Header) == 16, "the v1 header is 16 bytes");

    //: Client -> worker, once, immediately after the connection.
    struct Hello
    {
        Header        h;
        std::uint32_t client_pid;
        std::uint32_t client_protocol_version;
        std::uint32_t client_build;          // the add-on's build stamp
        std::uint32_t wants_transport;       // 1 = the client expects shared surfaces
        char          client_name[PROTO_NAME_MAX];
    };

    //: Worker -> client. Sent only after Reserved18 was created successfully, so
    //: that the client can never believe a half-initialised worker is ready.
    struct WorkerReady
    {
        Header        h;
        std::int32_t  reserved18;            // the CreateFeature(Reserved18) result
        std::int32_t  descriptor_status;     // the private CUDA-interop status
        std::int32_t  cumodule_status;
        std::uint32_t adapter_vendor;        // 0x10DE
        std::uint32_t adapter_device;        // 0x2786 for the RTX 4070
        std::uint32_t luid_high;
        std::uint32_t luid_low;
        std::uint32_t reserved18_handle_nonzero;
        std::uint64_t first_blob_size;       // 3944768 for the proven runtime
        char          worker_name[PROTO_NAME_MAX];
    };

    //: Client -> worker. The per-input metadata, one message per input slot. No
    //: pixel data: this describes a shared surface the worker will open.
    struct Config
    {
        Header        h;
        std::uint32_t input_slot;            // InputSlot: COLOR/DEPTH/MOTION_VECTORS/OUTPUT
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t dxgi_format;
        std::uint32_t row_pitch;
        std::uint32_t slice_pitch;
        std::uint32_t resource_offset;
        std::uint32_t flags;
        std::uint32_t frame_slots;           // how many ring entries the client will use
        char          shared_name[PROTO_NAME_MAX];   // the cross-adapter shared handle name
    };

    //: Client -> worker, per frame. The worker is told which slot became ready
    //: and with which metadata; the GPU-side work is fence-driven, not message
    //: driven.
    struct FrameSubmit
    {
        Header        h;
        std::uint32_t slot;                  // frame slot 0..FRAME_SLOT_COUNT-1
        std::uint32_t input_mask;            // bit per InputSlot present this frame
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t dxgi_format;
        std::uint32_t row_pitch;
        std::uint32_t slice_pitch;
        std::uint32_t resource_offset;
        std::uint64_t ingress_fence_value;   // the value the worker waits for
    };

    //: Worker -> client.
    struct FrameComplete
    {
        Header        h;
        std::uint32_t slot;
        std::int32_t  hr;
        std::uint64_t egress_fence_value;    // the value the client waits for
        std::int64_t  gpu_ticks;             // worker-side GPU time for the frame
    };

    //: Either direction.
    struct Shutdown
    {
        Header        h;
        std::uint32_t reason;
    };

    //: Either direction. Fatal for the connection: after an ERROR both sides stop
    //: using the worker path and the game continues without neural mode.
    struct Error
    {
        Header        h;
        std::int32_t  hr;
        std::uint32_t stage;
        char          text[PROTO_TEXT_MAX];
    };

#pragma pack(pop)

    // The layout is pinned member by member rather than by one arithmetic guess:
    // offsetof of the trailing array plus sizeof catches BOTH a wrong field count
    // and an accidental padding byte, which a single sum would not.
    static_assert(offsetof(Hello, client_name) == 32, "Hello: name offset");
    static_assert(sizeof(Hello) == 32 + PROTO_NAME_MAX, "Hello: total size");
    static_assert(offsetof(WorkerReady, worker_name) == 56, "WorkerReady: name offset");
    static_assert(sizeof(WorkerReady) == 56 + PROTO_NAME_MAX, "WorkerReady: total size");
    static_assert(offsetof(Config, shared_name) == 52, "Config: name offset");
    static_assert(sizeof(Config) == 52 + PROTO_NAME_MAX, "Config: total size");
    static_assert(sizeof(FrameSubmit) == 56, "FrameSubmit: total size");
    static_assert(sizeof(FrameComplete) == 40, "FrameComplete: total size");
    static_assert(sizeof(Shutdown) == 20, "Shutdown: total size");
    static_assert(offsetof(Error, text) == 24, "Error: text offset");
    static_assert(sizeof(Error) == 24 + PROTO_TEXT_MAX, "Error: total size");

    // The wire size of a message of this kind, or 0 for an unknown kind.
    std::uint32_t size_for(Kind k);

    // ------------------------------------------------------------- utilities

    //: Zero the whole struct and stamp a valid v1 header on it. The struct size
    //: is written from sizeof, never from a constant, so the header cannot
    //: disagree with the layout it describes.
    template <typename T>
    void init(T &msg, Kind k, std::uint32_t frame_id = 0)
    {
        std::memset(&msg, 0, sizeof msg);
        msg.h.magic = PROTO_MAGIC;
        msg.h.version = PROTO_VERSION;
        msg.h.kind = static_cast<std::uint16_t>(k);
        msg.h.size = static_cast<std::uint32_t>(sizeof(T));
        msg.h.frame_id = frame_id;
    }

    //: True when the header is a valid v1 header for `want` of exactly the size
    //: this build expects. A truncated read is caught by the size check, and a
    //: foreign protocol by the magic.
    bool header_ok(const Header &h, Kind want);

    //: The reason a header was refused, for the ERROR message and the log.
    const char *header_problem(const Header &h, Kind want);

    //: Copy a string into a fixed char array, always NUL-terminated.
    void set_text(char *dst, std::uint32_t cap, const char *src);

    //: The named-pipe name for a pid: \\.\pipe\MGPU_NR_<pid>.
    void pipe_name_for_pid(std::uint32_t pid, wchar_t *out, std::uint32_t out_chars);

    //: 'M','G','P','U' in memory order - the check that the two sides agree on
    //: byte order as well as on the constant.
    bool magic_bytes_are_mgpu();
}
