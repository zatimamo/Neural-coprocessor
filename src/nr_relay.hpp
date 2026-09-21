// ============================================================================
// MGPU Bridge - the OUT-OF-PROCESS RELAY, game-side half.
//
// WHAT THIS IS
//     The game-side end of the proven out-of-process neural lane. It owns the
//     IPC half of the bridge and NOTHING of D3D12: frames arrive here as plain
//     host pointers, the frames leave here as plain host pointers, and the
//     only thing in between is a named pipe and four named file mappings.
//
// WHY IT IS OUT OF PROCESS AT ALL
//     Measured, not assumed: NVIDIA's DLSS-NR runtime snippet fails inside
//     CreateFeature(NVSDK_NGX_Feature_Reserved18) with 0xBAD00002
//     (FAIL_PlatformError) whenever the CALLING PROCESS holds two or more D3D12
//     devices, of any vendor. The failing path is the private CUDA interop -
//     NvAPI_D3D12_GetCudaIndependentDescriptorObject and
//     NvAPI_D3D12_CreateCuModule both return -1. A process with exactly one
//     device succeeds.
//
//     So the RTX 4070 device, the NGX core, the architecture patch, the DLSSNR
//     runtime and the Reserved18 session live in mgpu_nr_worker.exe, and this
//     module is how the game talks to it. It never creates a device, never
//     loads nvngx.dll, never touches NVAPI and has no opinion about images.
//
// WHY THE CARRIER IS HOST MEMORY
//     Both adapters on this machine report CrossNodeSharingTier = 0 and
//     CrossAdapterRowMajorTextureSupported = false, and a copy INTO a
//     cross-adapter shared surface carries nothing (transport-benchmark.json,
//     ok=false). So the four frame surfaces are NAMED FILE MAPPINGS that both
//     processes map; a memcpy cannot fail to carry bytes. A CONFIG message per
//     slot carries the mapping name, the geometry, the format and both pitches;
//     FRAME_SUBMIT says "slot N now holds a frame"; FRAME_COMPLETE comes back
//     with the worker's own hr and its egress fence value.
//
// THE HONESTY RULE, WHICH IS THE POINT OF THIS FILE
//     The Reserved18 result this module reports is the value the worker sent in
//     WORKER_READY, quoted verbatim. `1` is Success and nothing else is.
//     `-1` means the worker never created a session - which is exactly what a
//     --protocol-only worker reports on purpose, so the control path is
//     testable on a machine with no NVIDIA adapter at all. Any other value is
//     whatever NGX returned (0xBAD00002 -> FAIL_PlatformError).
//     A frame is reported as evaluated only when the worker answered
//     FRAME_COMPLETE with hr == 0 for that frame AND the OUTPUT mapping's bytes
//     actually changed. There is no fallback path and no optimistic default.
//
// INDEPENDENCE
//     No ReShade type, no gpu1_context type, no ImGui, no NGX header, no NVAPI
//     header and no new link library: <windows.h> and the C++ runtime only,
//     exactly like tools/nr_worker/. That is what lets the SELFTEST build this
//     same translation unit into a GPU-free executable
//     (tools/nr_worker/nr_relay_selftest.cpp) and run the whole handshake
//     against a real worker process in CI. One implementation, tested where it
//     can be tested; the D3D12 halves stay on the far side of this interface.
//
// NOT WIRED IN YET, ON PURPOSE
//     Nothing calls this module. It compiles into the add-on only when
//     MGPU_NR_RELAY is defined, and the frame-pipeline call site (capture into
//     the slots, readback of the result) is a separate, later piece of work
//     written against this API. Until that exists, a run of the add-on behaves
//     exactly as it does without this file.
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

namespace mgpu::relay
{
    // ------------------------------------------------------------------ shape

    //: What a caller learns from start(). Every failure means nothing was
    //: started and nothing is leaked; the reason string says which step refused.
    enum class Status
    {
        NOT_ATTEMPTED = 0,
        NOT_COMPILED_IN,
        WORKER_MISSING,
        RUNTIME_MISSING,
        BAD_CONFIG,
        PROCESS_FAILED,
        PIPE_FAILED,
        HANDSHAKE_FAILED,
        SLOT_FAILED,
        OK,
    };

    const char *status_name(Status s);

    //: The relay's log, one line per call, whole lines only. `level` is 0 for
    //: information, 1 for a warning and 2 for an error.
    using LogSink = void (*)(int level, const char *line);

    //: Install the add-on's log (ReShade's log, through mgpu::diag) before
    //: start(). Null restores the default, which is stdout/stderr plus
    //: OutputDebugString - what the GPU-free selftest uses, because it has no
    //: ReShade. NOTHING calls this yet: the call-site integration installs it.
    void set_log_sink(LogSink sink);

    //: The neural runtime the GAME ships, with the hash the worker must pin for
    //: it. These two travel together on purpose: a path without its hash is how
    //: a wrong runtime gets loaded and then misread as a neural failure.
    struct NeuralRuntime
    {
        //: Empty -> <module dir>\mgpu\nvngx_dlssnr.dll, which is where the
        //: deployment puts it beside the worker.
        std::wstring path;
        //: The SHA256 the worker must pin for that DLL, as hex, lowercase or
        //: uppercase. Empty -> the relay's own default, which is the GAME's
        //: nvngx_dlssnr.dll (FileVersion 310.8.0.0): the runtime the game's own
        //: DLSS-NR path uses.
        //:
        //: It is deliberately NOT nr_worker.cpp's default. That default names a
        //: DIFFERENT build - the NeuralScreen v2.0.1 native nvngx_dlssnr.dll,
        //: dcc0dc24... - so leaving the worker on its default would run the lane
        //: against a runtime the game never loads, and would then report the
        //: difference as a neural result.
        char         hash[65] = "";
        //: true  -> pass the hash so the worker's gate is armed.
        //: false -> pass "" and DISABLE the worker's gate on purpose. The relay
        //:          logs, loudly, that it did. This is never a silent bypass.
        bool         pin_hash = true;
    };

    //: THE PER-SLOT GEOMETRY AND FORMAT CONTRACT.
    //:
    //: WHY IT IS PER SLOT AND NOT ONE GEOMETRY FOR ALL FOUR. The four slots do
    //: not share a size, a format or a pitch in a real frame, and that is not an
    //: edge case - it is the measured shape of the lane. Cyberpunk's own log
    //: shows, in the SAME frame:
    //:     [P4.0] stream arm source=3840x2160 fmt=28 rowPitch=15360
    //:     [R78]  MVEC ARM: mode=3 REAL, region 1920x1080 fmt=10 pitch=15360
    //: colour at 3840x2160 fmt=28 (R8G8B8A8_UNORM), motion vectors at 1920x1080
    //: fmt=10 (R16G16B16A16_FLOAT, 8 bytes per pixel), depth at fmt=41.
    //:
    //: A module that published ONE geometry four times would tell the worker to
    //: read the top `height` rows of a taller buffer, and the evaluated result
    //: would still come back hr == 0 with a changed OUTPUT - so EVERY honesty
    //: rule this module has would pass on a truncated frame. That is the failure
    //: this structure exists to make impossible: CONFIG already carries geometry
    //: and format per slot, the worker already stores and uses them per slot, and
    //: submit() refuses a frame whose numbers are not its slot's numbers.
    struct SlotConfig
    {
        unsigned width = 0u;
        unsigned height = 0u;
        //: DXGI_FORMAT. The relay needs it only for its bytes per pixel, which
        //: is what the pitch and the row-length check are derived from, so a
        //: format whose bytes per pixel this build does not know is refused at
        //: publish time rather than guessed at.
        unsigned dxgi_format = 0u;
    };

    //: The four slots, named rather than an array: a caller cannot silently
    //: leave one unset in a positional list, and slot_config() is then a lookup
    //: instead of an index the caller had to keep in step.
    struct SlotSet
    {
        SlotConfig color;
        SlotConfig depth;
        SlotConfig motion_vectors;
        SlotConfig output;
    };

    struct Config
    {
        //: The worker executable. Empty means <module dir>\mgpu\
        //: nvngx.dll_mgpu_nr_worker.exe. The NAME is not cosmetic: the DLSSNR
        //: snippet inspects the module name of its caller and refuses a caller
        //: whose file name does not contain "nvngx.dll" - with 0xBAD00002, the
        //: same code the two-device failure returns, which is exactly how that
        //: refusal would be misread as the neural blocker. So the deployed file
        //: names keep the prefix: the worker is
        //: <game>\bin\x64\mgpu\nvngx.dll_mgpu_nr_worker.exe and the add-on is
        //: <game>\bin\x64\nvngx.dll_mgpu_bridge.addon64.
        std::wstring worker_exe;

        //: The directory the worker and the runtime are resolved under. Empty
        //: means this module's own directory - which on the rig IS the add-on's
        //: directory, <game>\bin\x64, since the add-on is loaded from there.
        //: The selftest sets it, because it is not deployed beside the worker.
        std::wstring module_dir;

        //: Extra worker arguments, appended verbatim after the built-ins. The
        //: relay always passes --serve (it needs a worker that serves). A rig
        //: run additionally needs the runtime flags, which this struct builds
        //: from `runtime`; CI passes --protocol-only through here instead, and
        //: the two modes therefore share this one field.
        std::wstring worker_args;

        //: \\.\pipe\MGPU_NR_<pid>. Empty means \\.\pipe\MGPU_NR_<worker pid>.
        std::wstring pipe_name;

        //: THE NEURAL SIZE R, PER SLOT, AND WHAT R IS.
        //:
        //: The relay publishes each slot at these numbers and the caller is
        //: responsible for having REDUCED a frame to them. The add-on's stream
        //: lane already produces its three neural inputs at a reduced size:
        //: stream_state::sr_color is "R, tone-mapped, NR input", sr_depth is
        //: "R, point-reduced", and the motion vectors live at mvec_w x mvec_h.
        //: R is the GAME'S OWN DECLARED RENDER EXTENT, not the display extent
        //: (measured 1280x720 in one Cyberpunk run).
        //:
        //: THE RELAY DOES NOT RESAMPLE, CROP OR PAD ANYTHING, and must not be
        //: read as if it did. It copies rows into a mapping and copies them back
        //: out. A frame whose numbers are not the published numbers is REFUSED
        //: with the slot named and both geometries quoted.
        //:
        //: WHY THE DEFAULTS ARE 640x360: that is the proven CONTROL geometry,
        //: the one the worker's Reserved18 session is created at, and the one
        //: the GPU-free selftest publishes so that test stays a two-line setup.
        //: It is NOT a claim about what a real frame is - the rig geometry is
        //: the caller's R, and these four lines are what it overwrites.
        //:
        //: AND RIGHT NOW IT IS ALSO A CEILING, WHICH IS THE WORKER'S RULE, NOT
        //: THIS MODULE'S. The worker's frame path refuses a frame whose extent is
        //: not the extent its Reserved18 feature was created at, and that extent
        //: is its own pinned 640x360 (nr_worker.cpp frame_pipeline_run, and the
        //: NR_CTRL_W/NR_CTRL_H constants a CI gate pins). So today the relay
        //: publishes these numbers, the worker refuses anything else, and the
        //: refusal is the honest answer rather than a truncated frame. Raising R
        //: is the worker's change to make - the comment at that check says which
        //: line must move when it gains a geometry override - and this default is
        //: the one place on this side that follows it.
        SlotSet slots = {
            { 640u, 360u, 28u },   // COLOR          R8G8B8A8_UNORM
            { 640u, 360u, 41u },   // DEPTH          R32_FLOAT
            { 640u, 360u, 10u },   // MOTION_VECTORS R16G16B16A16_FLOAT
            { 640u, 360u, 28u },   // OUTPUT         R8G8B8A8_UNORM
        };

        //: true  -> the relay builds the worker's runtime flags from `runtime`
        //:          and launches a worker that runs the neural lane.
        //: false -> a --protocol-only worker: the control path with no GPU, no
        //:          NGX and no NVAPI, which is what CI can run. No runtime flags
        //:          are built and no runtime DLL is required. The selftest uses
        //:          this and also passes --protocol-only through worker_args.
        bool protocol_only = false;

        //: true  -> CreateProcessW the worker and connect to it.
        //: false -> connect to a worker somebody else started.
        bool auto_launch = true;

        NeuralRuntime runtime;

        //: How long the worker may take to appear on its pipe, and how long
        //: HELLO/WORKER_READY may take once it has.
        unsigned connect_ms = 20000u;
        unsigned handshake_ms = 20000u;

        //: How long shutdown() waits for the worker to exit on its own before
        //: it terminates it. DllMain teardown wants this SMALL (the code inside
        //: the loader lock must not block for seconds).
        unsigned shutdown_ms = 3000u;

        //: The worker's own log file name: it writes context-<name>.log in its
        //: working directory. Reported by the relay so a rig run can find it.
        std::wstring worker_log_name = L"nr-worker-relay";
    };

    //: The slot descriptor for an InputSlot index (0..3), or null when the
    //: index does not name one of the four.
    const SlotConfig *slot_config(const Config &cfg, unsigned slot_index);

    //: One frame, as plain host memory.
    //:
    //: EVERY INPUT CARRIES ITS OWN NUMBERS, because the four slots do not share
    //: them (see SlotConfig). Each pointer is the START of that input's frame
    //: data, never the base of a larger surface, and the relay copies
    //: `height` rows of `*_row_bytes` bytes using that input's own stride - so a
    //: sub-rectangled readback buffer is described correctly. Nothing is ever
    //: scaled, cropped or padded: a mismatch against the published slot is a
    //: returned failure.
    //:
    //: There is deliberately no output buffer here. The OUTPUT mapping is the
    //: result and it lives in the relay; out_row_bytes and out_stride were in an
    //: earlier draft and were never read, which would have implied an input that
    //: does not exist.
    struct FrameView
    {
        const void *color = nullptr;
        const void *depth = nullptr;
        const void *motion_vectors = nullptr;

        //: Rows in this frame. Checked against the published height of EVERY
        //: input slot, because the worker takes each slot's row count from the
        //: height CONFIG carried for that slot.
        unsigned height = 0u;

        //: The LOGICAL length of one row of each input, in bytes. Checked to be
        //: exactly bytes_per_pixel(format) * width of that slot, not merely to
        //: fit inside the pitch.
        unsigned color_row_bytes = 0u;
        unsigned depth_row_bytes = 0u;
        unsigned motion_row_bytes = 0u;

        //: The HOST stride of each input: bytes between the starts of two rows.
        //: A D3D12 readback buffer's row pitch is 256-aligned and that is exactly
        //: what a caller passes here. Zero means tightly packed, and the relay
        //: derives it from that slot's row_bytes rather than refusing.
        unsigned color_stride = 0u;
        unsigned depth_stride = 0u;
        unsigned motion_stride = 0u;
    };

    //: ONE INPUT SLOT, AS A FRAME IS CHECKED AGAINST IT. This is the subset of a
    //: published slot that the geometry contract needs - no mapping, no view, no
    //: state, nothing that requires a device or a session. That is the whole
    //: point of it: the rules that make the worker's `hr == 0` mean something can
    //: then be driven in a GPU-free test instead of only on the rig.
    //:
    //: ORDER IS FIXED: [0] COLOR, [1] DEPTH, [2] MOTION_VECTORS.
    struct SlotGeometry
    {
        bool     published = false;
        unsigned width = 0u;
        unsigned height = 0u;
        unsigned dxgi_format = 0u;
        unsigned bytes_per_pixel = 0u;
        //: bytes_per_pixel * width - the LOGICAL row, never the aligned pitch.
        unsigned row_bytes = 0u;
    };

    //: What the worker said about a submitted frame, quoted, never inferred.
    struct SubmitResult
    {
        bool          completed = false;        // a FRAME_COMPLETE arrived
        std::int32_t  worker_hr = 0;            // the worker's own hr, verbatim
        std::uint64_t egress_fence = 0ull;      // the worker's egress fence value
        std::uint64_t gpu_ticks = 0ull;         // the worker's own GPU time
        unsigned      slot = 0u;
        std::uint32_t frame_id = 0u;
        //: True only when the OUTPUT mapping's bytes differ from what they held
        //: before the submit. Zero-initialised mappings read the same before and
        //: after a frame nothing evaluated, which is how "no pixels moved" is
        //: told apart from "the model wrote an identical-looking image".
        bool          output_changed = false;
    };

    // -------------------------------------------------------------- lifecycle

    //: Is the worker executable where it must be? `why` always says what was
    //: looked for and what was found, in one line. GPU-free, side-effect free.
    bool available(std::string &why);

    //: Launch (or connect to) the worker, connect the pipe, HELLO, read
    //: WORKER_READY, publish the four slots and send the four CONFIG messages.
    //: Every step is bounded and every failure is non-fatal to the add-on: it
    //: returns a Status with a reason and leaves no handle, mapping or process
    //: behind (shutdown() is still safe to call).
    Status start(const Config &cfg, std::string &why);

    //: Best effort, bounded, idempotent, and safe from DllMain teardown: send
    //: SHUTDOWN, wait up to cfg.shutdown_ms for the worker to exit on its own,
    //: terminate it only if it does not, then close and unmap everything.
    void shutdown();

    bool ready();

    //: The worker's Reserved18 answer, exactly as WORKER_READY carried it.
    //: 1 = Success. -1 = that run created no session (protocol-only). Anything
    //: else is what NGX returned. Meaningful only while ready().
    std::int32_t reserved18();

    //: The exact string start() logged for Reserved18, for a caller that wants
    //: to record it itself rather than re-derive it.
    const std::string &reserved18_evidence();

    //: The last failure's reason, for the caller's own log line.
    const std::string &last_error();

    std::uint32_t worker_pid();

    // -------------------------------------------------------------- the frames

    //: Publish one frame: fill the four slots for frame `frame_id`,
    //: FRAME_SUBMIT it, and wait for FRAME_COMPLETE. `out.worker_hr` and
    //: `out.egress_fence` are the worker's values verbatim. Returns false only
    //: when no answer arrived at all, or when the worker has no session to
    //: submit to; a worker that answered with a failure hr is a SUCCESSFUL
    //: submit whose hr says so, which is what makes the difference visible
    //: instead of swallowed.
    bool submit(const FrameView &frame, std::uint32_t frame_id, SubmitResult &out,
                std::string &why);

    //: The geometry and pitches the relay published for a slot, so the pipeline
    //: call site can size its readback. Returns false when that slot is not
    //: published.
    struct SlotInfo
    {
        unsigned      width = 0u;
        unsigned      height = 0u;
        unsigned      dxgi_format = 0u;
        unsigned      row_pitch = 0u;     // 256-aligned, the D3D12 placement pitch
        unsigned      slice_pitch = 0u;   // row_pitch * height
        std::uint64_t bytes = 0ull;       // slice_pitch * FRAME_SLOT_COUNT
        std::string   mapping_name;
    };
    bool slot_info(unsigned slot_index, SlotInfo &out);

    // ------------------------------------------------------------ honesty API

    //: THE PER-SLOT GEOMETRY CONTRACT, AS A PURE FUNCTION: no pipe, no mapping,
    //: no session, no state. `submit()` calls it after its session guard, and
    //: the GPU-free selftest calls it directly, so the three rules that make the
    //: worker's `hr == 0` mean something are exercised where there is no device
    //: to exercise them with:
    //:
    //:   * the frame's row count must equal THAT slot's published height, because
    //:     the worker reads each slot's rows from the height its CONFIG carried;
    //:   * each input's logical row length must be exactly bytes_per_pixel(format)
    //:     times that slot's width - not merely something that fits under the
    //:     pitch, which is how a truncated frame would pass;
    //:   * each input's host stride must not be shorter than its row.
    //:
    //: Returns true when every input agrees with its slot; otherwise false with
    //: `why` naming the slot and both geometries. A refusal, never a clamp.
    bool check_frame(const FrameView &frame, const SlotGeometry inputs[3], std::string &why);

    //: The one place the word Success is allowed to be produced: a comparison
    //: against the worker's `1`. Everything else about Reserved18 is named
    //: honestly - "not-a-session" for -1, FAIL_PlatformError for 0xBAD00002,
    //: FAIL_OutOfDate for 0xBAD0000C, and the raw value for anything unknown.
    //: Exposed so the selftest asserts the same predicate the module uses.
    bool reserved18_is_success(std::int32_t value);

    //: The honest name for a Reserved18 result. Never returns "Success" unless
    //: the value is 1.
    const char *reserved18_name(std::int32_t value);
}
