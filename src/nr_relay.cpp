// ============================================================================
// MGPU Bridge - the out-of-process relay, game-side implementation.
// See nr_relay.hpp for the design, the measurements it rests on, and the
// honesty rule it is required to obey.
//
// TWO THINGS THIS FILE IS DELIBERATELY CAREFUL ABOUT
//
// 1. IT IS INDEPENDENT OF THE WORKER'S BUILD. The add-on must not gain a
//    dependency on tools/nr_worker: the worker owns the RTX 4070 device, the
//    NGX core and the Reserved18 session, and the add-on owning a link-time or
//    include-time reach into that tree is exactly the coupling the
//    architecture exists to prevent (nr-worker.yml's architecture gate checks
//    it). So the v1 wire layout is REPRODUCED here, byte for byte, with the
//    size, field-offset and enumerator static_asserts nr_proto.h itself uses,
//    and the CI job that builds this file diffs the two sets of numbers. A
//    drift is then a compile error here AND a failed gate, not a silent
//    misread of a frame.
//
// 2. IT REPORTS, IT DOES NOT DECIDE. Every value a reader might mistake for a
//    result - the Reserved18 answer, a frame's hr, the egress fence - is the
//    worker's, quoted. The only value this file computes is whether the OUTPUT
//    mapping's bytes changed, which is a fact about memory, not a verdict
//    about an image.
// ============================================================================

#include "nr_relay.hpp"

#ifdef MGPU_NR_RELAY

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mgpu::relay
{
    // ========================================================================
    // THE v1 WIRE LAYOUT, REPRODUCED.
    //
    // tools/nr_worker/nr_proto.h is the authority. This block mirrors it field
    // for field because the add-on does not include that directory (see the
    // header of this file). The static_asserts below are the SAME assertions
    // nr_proto.h makes - total sizes and field offsets - so a change to the
    // worker's protocol cannot compile here without this block changing too,
    // and the protocol-only selftest then fails against the real worker if the
    // two ever disagree in a way the asserts cannot see (a renamed field of the
    // same size). The CI job ALSO diffs this block against nr_proto.h.
    // ========================================================================
    namespace wire
    {
        constexpr std::uint32_t PROTO_MAGIC = 0x5550474Du;   // 'M','G','P','U'
        constexpr std::uint16_t PROTO_VERSION = 1u;
        constexpr std::uint32_t PROTO_NAME_MAX = 64;
        constexpr std::uint32_t PROTO_TEXT_MAX = 192;
        //: Frame slots in flight. The mapping each slot publishes must hold all
        //: of them: the worker opens the mapping and indexes into it with
        //: slot * slice_pitch, so a mapping sized for ONE frame would be read
        //: out of bounds from the second frame on.
        constexpr std::uint32_t FRAME_SLOT_COUNT = 3;

        enum class Kind : std::uint16_t
        {
            HELLO = 1,
            WORKER_READY = 2,
            CONFIG = 3,
            FRAME_SUBMIT = 4,
            FRAME_COMPLETE = 5,
            SHUTDOWN = 6,
            // NOT `ERROR`: <wingdi.h> defines ERROR as 0 and this file includes
            // <windows.h>. The wire value is 7 and the protocol still calls it
            // ERROR; only the identifier differs. Same reason as nr_proto.h.
            ERROR_MESSAGE = 7,
        };

        enum class InputSlot : std::uint32_t
        {
            COLOR = 0, DEPTH = 1, MOTION_VECTORS = 2, OUTPUT = 3, COUNT = 4,
        };

#pragma pack(push, 1)
        struct Header
        {
            std::uint32_t magic;
            std::uint16_t version;
            std::uint16_t kind;
            std::uint32_t size;
            std::uint32_t frame_id;
        };
        struct Hello
        {
            Header        h;
            std::uint32_t client_pid;
            std::uint32_t client_protocol_version;
            std::uint32_t client_build;
            std::uint32_t wants_transport;
            char          client_name[PROTO_NAME_MAX];
        };
        struct WorkerReady
        {
            Header        h;
            std::int32_t  reserved18;
            std::int32_t  descriptor_status;
            std::int32_t  cumodule_status;
            std::uint32_t adapter_vendor;
            std::uint32_t adapter_device;
            std::uint32_t luid_high;
            std::uint32_t luid_low;
            std::uint32_t reserved18_handle_nonzero;
            std::uint64_t first_blob_size;
            char          worker_name[PROTO_NAME_MAX];
        };
        struct Config
        {
            Header        h;
            std::uint32_t input_slot;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t dxgi_format;
            std::uint32_t row_pitch;
            std::uint32_t slice_pitch;
            std::uint32_t resource_offset;
            std::uint32_t flags;
            std::uint32_t frame_slots;
            char          shared_name[PROTO_NAME_MAX];
        };
        struct FrameSubmit
        {
            Header        h;
            std::uint32_t slot;
            std::uint32_t input_mask;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t dxgi_format;
            std::uint32_t row_pitch;
            std::uint32_t slice_pitch;
            std::uint32_t resource_offset;
            std::uint64_t ingress_fence_value;
        };
        struct FrameComplete
        {
            Header        h;
            std::uint32_t slot;
            std::int32_t  hr;
            std::uint64_t egress_fence_value;
            std::int64_t  gpu_ticks;
        };
        struct Shutdown
        {
            Header        h;
            std::uint32_t reason;
        };
        struct Error
        {
            Header        h;
            std::int32_t  hr;
            std::uint32_t stage;
            char          text[PROTO_TEXT_MAX];
        };
#pragma pack(pop)

        static_assert(sizeof(Header) == 16, "the v1 header is 16 bytes");
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
        static_assert((unsigned)Kind::HELLO == 1u && (unsigned)Kind::WORKER_READY == 2u &&
                      (unsigned)Kind::CONFIG == 3u && (unsigned)Kind::FRAME_SUBMIT == 4u &&
                      (unsigned)Kind::FRAME_COMPLETE == 5u && (unsigned)Kind::SHUTDOWN == 6u &&
                      (unsigned)Kind::ERROR_MESSAGE == 7u, "the v1 kind numbers");
        static_assert((unsigned)InputSlot::COLOR == 0u && (unsigned)InputSlot::DEPTH == 1u &&
                      (unsigned)InputSlot::MOTION_VECTORS == 2u &&
                      (unsigned)InputSlot::OUTPUT == 3u &&
                      (unsigned)InputSlot::COUNT == 4u, "the four input slots");

        template <typename T>
        void init(T &msg, Kind k, std::uint32_t frame_id)
        {
            std::memset(&msg, 0, sizeof msg);
            msg.h.magic = PROTO_MAGIC;
            msg.h.version = PROTO_VERSION;
            msg.h.kind = static_cast<std::uint16_t>(k);
            msg.h.size = static_cast<std::uint32_t>(sizeof(T));
            msg.h.frame_id = frame_id;
        }

        //: The wire size this build expects for a kind, from the same sizeof the
        //: sender used. A kind this build does not know has no size. The read
        //: path below checks the peer's declared size against this before it
        //: re-reads the message as the struct the kind actually is.
        std::uint32_t sizeof_expected(Kind k);

        void set_text(char *dst, std::uint32_t cap, const char *src)
        {
            if (dst == nullptr || cap == 0u) return;
            std::memset(dst, 0, cap);
            if (src == nullptr) return;
            std::size_t n = std::strlen(src);
            if (n >= cap) n = cap - 1u;
            std::memcpy(dst, src, n);
        }
    }

    const char *status_name(Status s)
    {
        switch (s)
        {
        case Status::NOT_ATTEMPTED:    return "NOT_ATTEMPTED";
        case Status::NOT_COMPILED_IN:  return "NOT_COMPILED_IN";
        case Status::WORKER_MISSING:   return "WORKER_MISSING";
        case Status::RUNTIME_MISSING:  return "RUNTIME_MISSING";
        case Status::BAD_CONFIG:       return "BAD_CONFIG";
        case Status::PROCESS_FAILED:   return "PROCESS_FAILED";
        case Status::PIPE_FAILED:      return "PIPE_FAILED";
        case Status::HANDSHAKE_FAILED: return "HANDSHAKE_FAILED";
        case Status::SLOT_FAILED:      return "SLOT_FAILED";
        case Status::OK:               return "OK";
        }
        return "?";
    }

    // ==================================================================== log

    namespace
    {
        //: The runtime the GAME ships, by hash. <game>\bin\x64\mgpu\
        //: nvngx_dlssnr.dll, FileVersion 310.8.0.0. NOT the hash nr_worker.cpp
        //: pins as its own default (dcc0dc24..., the NeuralScreen v2.0.1 native
        //: build), which is a different binary with a different behaviour. A
        //: caller that knows better sets Config::runtime.hash; this is the
        //: default so that "the relay launched the worker" and "the worker ran
        //: the game's runtime" are the same statement.
        const char *kGameRuntimeSha256 =
            "4B8D19BC3EFF58A084F5ECA7489C921501C203450169FB82FF4F649A4482BA05";

        LogSink g_sink = nullptr;

        void default_sink(int level, const char *line)
        {
            // The selftest's sink. The add-on replaces this with mgpu::diag so
            // the lines land in ReShade.log; this default exists so the module
            // is usable, and testable, with no ReShade in the process at all.
            std::fprintf(level == 2 ? stderr : stdout, "%s\n", line);
            std::fflush(level == 2 ? stderr : stdout);
            OutputDebugStringA(line);
            OutputDebugStringA("\n");
        }

        void log_line(int level, const char *fmt, ...)
        {
            char buf[1024];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
            LogSink s = (g_sink != nullptr) ? g_sink : &default_sink;
            s(level, buf);
        }

        // ============================================================ the state

        struct Slot
        {
            bool           published = false;
            wire::InputSlot which = wire::InputSlot::COLOR;
            std::string    name;              // the mapping name, narrow
            std::wstring   wname;             // the same, wide
            //: THIS SLOT's published geometry, format and derived pitches. Kept
            //: so submit() can check a frame against the slot it is handing it
            //: to; the four slots do not share any of these numbers.
            unsigned       width = 0u;
            unsigned       height = 0u;
            unsigned       row_bytes = 0u;    // bpp * width, the LOGICAL row
            unsigned       dxgi_format = 0u;
            unsigned       bpp = 0u;
            unsigned       row_pitch = 0u;
            unsigned       slice_pitch = 0u;
            unsigned long long bytes = 0ull;
            HANDLE         mapping = nullptr;
            unsigned char *view = nullptr;
            std::vector<unsigned char> fill;   // one slice, built per submit
        };

        struct State
        {
            bool          started = false;
            Config        cfg;
            std::wstring  exe;                 // resolved worker path
            std::wstring  pipe;                // resolved pipe name
            std::string   pipe_narrow;
            bool          owns_worker = false;
            PROCESS_INFORMATION pi{};
            HANDLE        pipe_handle = nullptr;
            HANDLE        pipe_event = nullptr;
            Slot          slots[(unsigned)wire::InputSlot::COUNT];
            std::int32_t  reserved18 = -1;
            std::string   evidence;
            std::string   last_error;
            std::uint32_t next_frame_id = 1u;
        };

        State g;

        // ====================================================== small utilities

        //: The directory this translation unit is loaded from.
        //:
        //: GetModuleHandleExW FROM_ADDRESS and a function's own address, NOT
        //: GetModuleHandle(nullptr): the latter returns the GAME's module, which
        //: is a different directory and a different product. The add-on exposes
        //: its own handle as mgpu::module_handle(), but reaching for it here
        //: would give this file a link-time dependency on dllmain.cpp and would
        //: not compile in the selftest, which is the same code in a different
        //: binary. The address of a function in THIS file resolves to whichever
        //: module this file was compiled into, which is exactly the question
        //: being asked. FROM_ADDRESS takes a reference rather than creating one,
        //: so it is safe to call while the loader lock is held - and the
        //: selftest calls this from no lock at all.
        HMODULE own_module()
        {
            HMODULE m = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)(const void *)&own_module, &m))
                return m;
            return nullptr;
        }

        std::wstring directory_of(const std::wstring &file)
        {
            const std::size_t cut = file.find_last_of(L"\\/");
            return (cut == std::wstring::npos) ? std::wstring() : file.substr(0, cut);
        }

        //: The directory the worker and the runtime are resolved under. The
        //: add-on is deployed as <game>\bin\x64\nvngx.dll_mgpu_bridge.addon64,
        //: so its own directory IS the directory the deployment puts mgpu\ in.
        std::wstring effective_module_dir(const std::wstring &override_dir)
        {
            if (!override_dir.empty()) return override_dir;
            wchar_t buf[32768];
            const DWORD n = GetModuleFileNameW(own_module(), buf,
                                               (DWORD)(sizeof buf / sizeof buf[0]));
            if (n == 0u || n >= (DWORD)(sizeof buf / sizeof buf[0])) return std::wstring();
            return directory_of(std::wstring(buf, n));
        }

        std::wstring default_worker_path(const std::wstring &dir)
        {
            if (dir.empty()) return std::wstring();
            // The prefixed FILE NAME is not cosmetic. The DLSSNR snippet inspects
            // the module name of its caller and refuses a caller whose file name
            // does not contain "nvngx.dll", with 0xBAD00002 - the same code the
            // two-device failure returns. A worker that dropped the prefix would
            // therefore fail in a way that looks exactly like the blocker this
            // whole architecture exists to avoid, which is why the name is
            // spelled out here and asserted by the CI gate.
            return dir + L"\\mgpu\\nvngx.dll_mgpu_nr_worker.exe";
        }

        std::wstring default_runtime_path(const std::wstring &dir)
        {
            if (dir.empty()) return std::wstring();
            return dir + L"\\mgpu\\nvngx_dlssnr.dll";
        }

        void wide_to_narrow(const std::wstring &w, std::string &out)
        {
            out.clear();
            if (w.empty()) return;
            const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                                 nullptr);
            if (need <= 1) return;
            std::string tmp((std::size_t)need, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &tmp[0], need, nullptr, nullptr);
            out.assign(tmp.c_str());
        }

        std::wstring narrow_to_wide(const std::string &s)
        {
            std::wstring out;
            if (s.empty()) return out;
            const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            if (need <= 1) return out;
            std::wstring tmp((std::size_t)need, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &tmp[0], need);
            out.assign(tmp.c_str());
            return out;
        }

        double now_ms()
        {
            // A monotonic millisecond clock for the deadlines. GetTickCount64
            // is the same source nr_pipe.cpp uses.
            return (double)GetTickCount64();
        }

        //: Bytes per pixel for the formats the lane actually carries. The set is
        //: the worker's own table (tools/nr_worker/nr_worker.cpp,
        //: test_bytes_per_pixel) rather than a guess, because the worker refuses
        //: a CONFIG whose format has no known bytes per pixel and the two sides
        //: must therefore agree about which formats are usable. 10 is in here
        //: and 34 is too: Cyberpunk's motion vectors are fmt=10
        //: (R16G16B16A16_FLOAT, EIGHT bytes per pixel), while the proven control
        //: lane's are fmt=34 (R16G16_FLOAT, four). Publishing one pitch for both
        //: would halve the rows the worker reads.
        unsigned bpp_of(unsigned dxgi_format)
        {
            switch (dxgi_format)
            {
            case 28u: return 4u;   // DXGI_FORMAT_R8G8B8A8_UNORM     - COLOR and OUTPUT
            case 40u: return 4u;   // DXGI_FORMAT_D32_FLOAT           - DEPTH
            case 41u: return 4u;   // DXGI_FORMAT_R32_FLOAT           - DEPTH, the game's
            case 34u: return 4u;   // DXGI_FORMAT_R16G16_FLOAT        - MOTION_VECTORS
            case 10u: return 8u;   // DXGI_FORMAT_R16G16B16A16_FLOAT  - MOTION_VECTORS, the game's
            default:  return 0u;
            }
        }

        //: The published slot descriptor for an index, in InputSlot order.
        const SlotConfig *config_slot(const Config &cfg, unsigned i)
        {
            switch (i)
            {
            case 0u: return &cfg.slots.color;
            case 1u: return &cfg.slots.depth;
            case 2u: return &cfg.slots.motion_vectors;
            case 3u: return &cfg.slots.output;
            default: return nullptr;
            }
        }

        const char *slot_name(wire::InputSlot s)
        {
            switch (s)
            {
            case wire::InputSlot::COLOR:          return "COLOR";
            case wire::InputSlot::DEPTH:          return "DEPTH";
            case wire::InputSlot::MOTION_VECTORS: return "MOTION_VECTORS";
            case wire::InputSlot::OUTPUT:         return "OUTPUT";
            default:                              return "?";
            }
        }

        // ============================================================ the pipe

        //: One duplex named pipe, client end. A local re-implementation rather
        //: than a reuse of tools/nr_worker/nr_pipe.cpp for the reason in this
        //: file's header: the add-on must not include that tree. The framing is
        //: the same and the deadlines are the same shape - every read and write
        //: is bounded, because this runs inside a game.
        bool pipe_open(const std::wstring &name, unsigned timeout_ms, std::string &err)
        {
            const double deadline = now_ms() + (double)timeout_ms;
            for (;;)
            {
                HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    if (ev == nullptr)
                    {
                        CloseHandle(h);
                        err = "CreateEventW failed for the pipe";
                        return false;
                    }
                    g.pipe_handle = h;
                    g.pipe_event = ev;
                    return true;
                }
                const DWORD e = GetLastError();
                if (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND)
                {
                    char b[160];
                    std::snprintf(b, sizeof b, "CreateFileW on the pipe failed (%lu)",
                                  (unsigned long)e);
                    err = b;
                    return false;
                }
                if (now_ms() >= deadline)
                {
                    err = "no worker answered on the pipe before the deadline";
                    return false;
                }
                // ERROR_PIPE_BUSY -> the instance exists but is taken;
                // ERROR_FILE_NOT_FOUND -> the worker has not created it yet.
                // Both mean "try again shortly". The worker's own connect() has
                // the same loop, and this one exists separately because the
                // add-on cannot reach that translation unit.
                WaitNamedPipeW(name.c_str(), 50);
                Sleep(20);
            }
        }

        bool pipe_read_exact(void *buf, std::uint32_t n, unsigned timeout_ms, std::string &err)
        {
            if (g.pipe_handle == nullptr) { err = "the pipe is not open"; return false; }
            unsigned char *p = (unsigned char *)buf;
            std::uint32_t got = 0u;
            const double deadline = now_ms() + (double)timeout_ms;

            while (got < n)
            {
                const double now = now_ms();
                if (now >= deadline) { err = "the read timed out"; return false; }
                const DWORD slice = (DWORD)(deadline - now);

                OVERLAPPED ov{};
                ov.hEvent = g.pipe_event;
                ResetEvent(g.pipe_event);
                DWORD moved = 0u;
                const BOOL ok = ReadFile(g.pipe_handle, p + got, n - got, nullptr, &ov);
                if (!ok)
                {
                    const DWORD e = GetLastError();
                    if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED ||
                        e == ERROR_NO_DATA || e == ERROR_OPERATION_ABORTED)
                    {
                        err = "the worker closed the pipe";
                        return false;
                    }
                    if (e != ERROR_IO_PENDING)
                    {
                        char b[160];
                        std::snprintf(b, sizeof b, "ReadFile failed (%lu)", (unsigned long)e);
                        err = b;
                        return false;
                    }
                    if (WaitForSingleObject(g.pipe_event, slice) != WAIT_OBJECT_0)
                    {
                        CancelIoEx(g.pipe_handle, &ov);
                        WaitForSingleObject(g.pipe_event, 100);
                        err = "the read timed out";
                        return false;
                    }
                    if (!GetOverlappedResult(g.pipe_handle, &ov, &moved, FALSE))
                    {
                        err = "GetOverlappedResult failed for a read";
                        return false;
                    }
                }
                else if (!GetOverlappedResult(g.pipe_handle, &ov, &moved, TRUE))
                {
                    err = "GetOverlappedResult failed for a completed read";
                    return false;
                }
                if (moved == 0u) { err = "the worker closed the pipe"; return false; }
                got += moved;
            }
            return true;
        }

        bool pipe_write_all(const void *buf, std::uint32_t n, unsigned timeout_ms,
                            std::string &err)
        {
            if (g.pipe_handle == nullptr) { err = "the pipe is not open"; return false; }
            const unsigned char *p = (const unsigned char *)buf;
            std::uint32_t sent = 0u;
            const double deadline = now_ms() + (double)timeout_ms;

            while (sent < n)
            {
                const double now = now_ms();
                if (now >= deadline) { err = "the write timed out"; return false; }
                const DWORD slice = (DWORD)(deadline - now);

                OVERLAPPED ov{};
                ov.hEvent = g.pipe_event;
                ResetEvent(g.pipe_event);
                DWORD moved = 0u;
                const BOOL ok = WriteFile(g.pipe_handle, p + sent, n - sent, nullptr, &ov);
                if (!ok)
                {
                    const DWORD e = GetLastError();
                    if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED ||
                        e == ERROR_NO_DATA || e == ERROR_OPERATION_ABORTED)
                    {
                        err = "the worker closed the pipe";
                        return false;
                    }
                    if (e != ERROR_IO_PENDING)
                    {
                        char b[160];
                        std::snprintf(b, sizeof b, "WriteFile failed (%lu)", (unsigned long)e);
                        err = b;
                        return false;
                    }
                    if (WaitForSingleObject(g.pipe_event, slice) != WAIT_OBJECT_0)
                    {
                        CancelIoEx(g.pipe_handle, &ov);
                        WaitForSingleObject(g.pipe_event, 100);
                        err = "the write timed out";
                        return false;
                    }
                    if (!GetOverlappedResult(g.pipe_handle, &ov, &moved, FALSE))
                    {
                        err = "GetOverlappedResult failed for a write";
                        return false;
                    }
                }
                else if (!GetOverlappedResult(g.pipe_handle, &ov, &moved, TRUE))
                {
                    err = "GetOverlappedResult failed for a completed write";
                    return false;
                }
                sent += moved;
            }
            return true;
        }

        std::uint32_t wire_size_of(wire::Kind k)
        {
            switch (k)
            {
            case wire::Kind::HELLO:          return (std::uint32_t)sizeof(wire::Hello);
            case wire::Kind::WORKER_READY:   return (std::uint32_t)sizeof(wire::WorkerReady);
            case wire::Kind::CONFIG:         return (std::uint32_t)sizeof(wire::Config);
            case wire::Kind::FRAME_SUBMIT:   return (std::uint32_t)sizeof(wire::FrameSubmit);
            case wire::Kind::FRAME_COMPLETE: return (std::uint32_t)sizeof(wire::FrameComplete);
            case wire::Kind::SHUTDOWN:       return (std::uint32_t)sizeof(wire::Shutdown);
            case wire::Kind::ERROR_MESSAGE:  return (std::uint32_t)sizeof(wire::Error);
            }
            return 0u;
        }

        template <typename T>
        bool write_msg(const T &msg, unsigned timeout_ms, std::string &err)
        {
            return pipe_write_all(&msg, (std::uint32_t)sizeof(T), timeout_ms, err);
        }

        //: Read one message of the largest size v1 defines, refuse a header
        //: whose declared size is not `want`'s, and only then re-read it through
        //: the struct the kind actually is. Same discipline as the worker's
        //: serve(): a size mismatch is a protocol error, never a best-effort
        //: decode, because a mis-framed message on a channel that describes
        //: shared memory is worse than a dropped connection.
        bool read_msg(wire::Error &buf, wire::Kind want, unsigned timeout_ms, std::string &err)
        {
            std::memset(&buf, 0, sizeof buf);
            if (!pipe_read_exact(&buf, (std::uint32_t)sizeof(wire::Header), timeout_ms, err))
                return false;
            if (buf.h.magic != wire::PROTO_MAGIC || buf.h.version != wire::PROTO_VERSION)
            {
                err = "the peer does not speak protocol v1";
                return false;
            }
            const std::uint32_t expected = wire_size_of(want);
            if (buf.h.size != expected)
            {
                char b[192];
                std::snprintf(b, sizeof b,
                              "the peer declared size %u for kind %u; this build expects %u",
                              buf.h.size, (unsigned)buf.h.kind, expected);
                err = b;
                return false;
            }
            if (!pipe_read_exact((unsigned char *)&buf + sizeof(wire::Header),
                                 expected - (std::uint32_t)sizeof(wire::Header), timeout_ms, err))
                return false;
            return true;
        }

        void pipe_close()
        {
            if (g.pipe_handle != nullptr)
            {
                // Cancel anything pending before closing, so a timed-out
                // overlapped read cannot complete into a closed handle.
                CancelIoEx(g.pipe_handle, nullptr);
                CloseHandle(g.pipe_handle);
                g.pipe_handle = nullptr;
            }
            if (g.pipe_event != nullptr)
            {
                CloseHandle(g.pipe_event);
                g.pipe_event = nullptr;
            }
        }

        // ============================================================ the slots

        //: A stable, per-session mapping name derived from the pipe name. The
        //: pipe already carries the worker's pid, so two workers cannot collide
        //: on this, and sanitising the leaf keeps "\\" and "." out of the
        //: mapping namespace (a name like "Local\\MGPU_NR_1234" is the same
        //: section namespace OpenFileMappingW reads on the worker's side).
        std::string mapping_name_for(const std::wstring &pipe, const char *slot)
        {
            std::string leaf;
            wide_to_narrow(pipe, leaf);
            const std::size_t cut = leaf.find_last_of('\\');
            if (cut != std::string::npos) leaf = leaf.substr(cut + 1u);
            std::string safe;
            for (std::size_t i = 0; i < leaf.size() && safe.size() < 48u; ++i)
            {
                const char c = leaf[i];
                const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '_';
                safe.push_back(ok ? c : '_');
            }
            if (safe.empty()) safe = "MGPU_NR";
            return "Local\\" + safe + "_" + slot;
        }

        void slots_release()
        {
            for (unsigned i = 0u; i < (unsigned)wire::InputSlot::COUNT; ++i)
            {
                Slot &s = g.slots[i];
                if (s.view != nullptr) { UnmapViewOfFile(s.view); s.view = nullptr; }
                if (s.mapping != nullptr) { CloseHandle(s.mapping); s.mapping = nullptr; }
                s.published = false;
            }
        }

        //: Publish one slot: a pagefile-backed named section big enough for all
        //: FRAME_SLOT_COUNT frames, zeroed.
        //:
        //: THE ARITHMETIC, DERIVED RATHER THAN GUESSED:
        //:     row_pitch   = align_up(width * bytes_per_pixel, 256)
        //:                   256 == D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, so the
        //:                   rows in the mapping can be copied to a D3D12
        //:                   upload buffer one row at a time with no repacking.
        //:     slice_pitch = row_pitch * height
        //:     bytes       = slice_pitch * FRAME_SLOT_COUNT
        //: The worker validates the opened mapping against
        //: resource_offset + slice_pitch for the geometry CONFIG carried, so an
        //: error here is refused by the worker rather than read out of bounds -
        //: but the refusal would only appear on the rig, which is why the number
        //: is spelled out and printed.
        bool slot_publish(Slot &s, wire::InputSlot which, const std::wstring &pipe, unsigned w,
                          unsigned h, unsigned dxgi_format, std::string &err)
        {
            s.which = which;
            // THE PUBLISHED NUMBERS ARE KEPT, per slot, because submit() checks
            // every frame against them. One geometry for all four slots is what
            // would let a 1920x1080 motion-vector buffer be read as if it were
            // 3840x2160 and still come back hr == 0 with a changed output.
            s.width = w;
            s.height = h;
            s.dxgi_format = dxgi_format;
            s.bpp = bpp_of(dxgi_format);
            if (s.bpp == 0u)
            {
                char b[160];
                std::snprintf(b, sizeof b,
                              "format %u has no bytes-per-pixel in this build, so its pitch "
                              "cannot be derived", dxgi_format);
                err = b;
                return false;
            }
            if (w == 0u || h == 0u)
            {
                err = "a slot was published with width 0 or height 0";
                return false;
            }

            s.row_bytes = w * s.bpp;
            s.row_pitch = (unsigned)((((unsigned long long)s.row_bytes) + 255ull) & ~255ull);
            s.slice_pitch = s.row_pitch * h;
            s.bytes = (unsigned long long)s.slice_pitch * (unsigned long long)wire::FRAME_SLOT_COUNT;
            s.name = mapping_name_for(pipe, slot_name(which));
            s.wname = narrow_to_wide(s.name);
            if (s.wname.empty()) { err = "a slot's mapping name did not convert to wide text"; return false; }

            s.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                           (DWORD)(s.bytes >> 32),
                                           (DWORD)(s.bytes & 0xFFFFFFFFull), s.wname.c_str());
            if (s.mapping == nullptr)
            {
                err = "CreateFileMappingW failed for " + s.name;
                return false;
            }
            s.view = (unsigned char *)MapViewOfFile(s.mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                                    0, 0, 0);
            if (s.view == nullptr)
            {
                CloseHandle(s.mapping);
                s.mapping = nullptr;
                err = "MapViewOfFile failed for " + s.name;
                return false;
            }
            // Zeroed: the worker reads at least every published row, and a
            // slot the worker never wrote must read as "nothing here" rather
            // than as whatever the game's own process happened to have mapped.
            std::memset(s.view, 0, (std::size_t)s.bytes);
            s.fill.assign((std::size_t)s.slice_pitch, 0u);
            s.published = true;
            return true;
        }

        //: CONFIG carries THIS SLOT's geometry, format and both pitches. That is
        //: the whole reason the protocol has them per message rather than once
        //: per session, and the worker stores them per slot and reads each
        //: frame out of the slot it names.
        bool send_config(const Slot &s, std::string &err)
        {
            wire::Config c;
            wire::init(c, wire::Kind::CONFIG, 0u);
            c.input_slot = (std::uint32_t)s.which;
            c.width = s.width;
            c.height = s.height;
            c.dxgi_format = s.dxgi_format;
            c.row_pitch = s.row_pitch;
            c.slice_pitch = s.slice_pitch;
            c.resource_offset = 0u;
            c.flags = 0u;
            c.frame_slots = wire::FRAME_SLOT_COUNT;
            wire::set_text(c.shared_name, wire::PROTO_NAME_MAX, s.name.c_str());
            return write_msg(c, 4000u, err);
        }
    }   // namespace

    const SlotConfig *slot_config(const Config &cfg, unsigned slot_index)
    {
        return config_slot(cfg, slot_index);
    }

    // ================================================================ honesty

    bool reserved18_is_success(std::int32_t value)
    {
        // THE ONLY PLACE a success may be claimed, and it is a comparison
        // against the worker's own 1. Nothing else in this file, and nothing in
        // the selftest, may produce that word.
        return value == 1;
    }

    const char *reserved18_name(std::int32_t value)
    {
        if (value == 1) return "Success";
        if (value == -1) return "not-a-session";              // --protocol-only, on purpose
        switch ((std::uint32_t)value)
        {
        case 0xBAD00002u: return "FAIL_PlatformError";        // the two-device failure
        case 0xBAD0000Cu: return "FAIL_OutOfDate";
        case 0xBAD00001u: return "FAIL_Init";
        case 0xBAD00003u: return "FAIL_NotSupported";
        case 0xBAD00004u: return "FAIL_InvalidParameter";
        default:          return "unknown-result";
        }
    }

    // ============================================================== lifecycle

    void set_log_sink(LogSink sink)
    {
        g_sink = sink;
    }

    bool available(std::string &why)
    {
        Config probe;
        const std::wstring dir = effective_module_dir(probe.module_dir);
        const std::wstring exe = probe.worker_exe.empty() ? default_worker_path(dir)
                                                          : probe.worker_exe;
        if (exe.empty())
        {
            why = "worker: could not resolve this module's directory";
            return false;
        }
        const DWORD attrs = GetFileAttributesW(exe.c_str());
        std::string narrow;
        wide_to_narrow(exe, narrow);
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            why = "worker MISSING at " + narrow;
            return false;
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0u)
        {
            why = "worker path is a DIRECTORY, not the executable: " + narrow;
            return false;
        }
        why = "worker present at " + narrow;
        return true;
    }

    //: The worker's command line. The runtime flags are built here rather than
    //: left to the caller because the two values that must travel together -
    //: the runtime DLL and the hash the worker must pin for it - are the pair a
    //: caller can most easily get half right.
    namespace
    {
        bool build_worker_command(const Config &cfg, const std::wstring &dir,
                                  std::wstring &cmd, std::string &err)
        {
            const std::wstring exe = cfg.worker_exe.empty() ? default_worker_path(dir)
                                                            : cfg.worker_exe;
            cmd = L"\"" + exe + L"\" --serve";
            if (!cfg.worker_args.empty()) { cmd += L" "; cmd += cfg.worker_args; }

            if (!cfg.protocol_only)
            {
                const std::wstring dll = cfg.runtime.path.empty() ? default_runtime_path(dir)
                                                                  : cfg.runtime.path;
                if (dll.empty())
                {
                    err = "the neural runtime path could not be resolved";
                    return false;
                }
                std::string dll_narrow;
                wide_to_narrow(dll, dll_narrow);

                cmd += L" --nr-dll \"";
                cmd += dll;
                cmd += L"\"";

                if (cfg.runtime.pin_hash)
                {
                    // The GAME's runtime, by hash, explicitly. nr_worker.cpp's own
                    // default names a DIFFERENT build (the NeuralScreen v2.0.1
                    // native nvngx_dlssnr.dll, dcc0dc24...); the game ships
                    // 310.8.0.0. Leaving the worker on its default would run the
                    // lane against a runtime the game never loads and then report
                    // the difference as a neural result, so the hash is passed
                    // explicitly and logged with the path it came from.
                    const char *hash = (cfg.runtime.hash[0] != '\0')
                        ? cfg.runtime.hash
                        : kGameRuntimeSha256;
                    cmd += L" --nr-dll-sha256 ";
                    cmd += narrow_to_wide(std::string(hash));
                    log_line(0, "[MGPU][RELAY] runtime pinned: path=%s sha256=%s source=%s",
                             dll_narrow.c_str(), hash,
                             (cfg.runtime.hash[0] != '\0') ? "caller" : "relay-default(game)");
                }
                else
                {
                    // Not a silent bypass: it is said, in the log, that the gate
                    // is off.
                    cmd += L" --nr-dll-sha256 \"\"";
                    log_line(1, "[MGPU][RELAY] runtime path=%s and the worker's SHA256 gate is "
                                "DISABLED on purpose (pin_hash=false)", dll_narrow.c_str());
                }
            }

            if (!cfg.worker_log_name.empty())
            {
                cmd += L" --log-name ";
                cmd += cfg.worker_log_name;
            }
            if (!cfg.pipe_name.empty())
            {
                cmd += L" --pipe \"";
                cmd += cfg.pipe_name;
                cmd += L"\"";
            }
            return true;
        }
    }

    Status start(const Config &cfg, std::string &why)
    {
        if (g.started)
        {
            why = "the relay is already started";
            g.last_error = why;
            return Status::BAD_CONFIG;
        }
        g.cfg = cfg;
        g.last_error.clear();
        g.evidence.clear();

        // EVERY slot's geometry is the caller's, so every slot's is checked.
        // There is no "the" geometry any more: a frame's colour and its motion
        // vectors are different sizes, and each is published from its own
        // numbers in the loop below.
        for (unsigned i = 0u; i < 4u; ++i)
        {
            const SlotConfig *sc = config_slot(cfg, i);
            if (sc == nullptr || sc->width == 0u || sc->height == 0u || sc->dxgi_format == 0u)
            {
                why = "every one of the four slots needs a non-zero width, height and format; "
                      "the relay publishes what it is given and refuses what it cannot carry";
                g.last_error = why;
                return Status::BAD_CONFIG;
            }
        }
        const std::wstring dir = effective_module_dir(cfg.module_dir);
        if (dir.empty())
        {
            why = "this module's directory could not be resolved, so neither the worker nor the "
                  "runtime can be found";
            g.last_error = why;
            return Status::BAD_CONFIG;
        }
        const std::wstring worker_exe = cfg.worker_exe.empty() ? default_worker_path(dir)
                                                               : cfg.worker_exe;
        if (worker_exe.empty())
        {
            why = "the worker path could not be resolved";
            g.last_error = why;
            return Status::BAD_CONFIG;
        }
        g.exe = worker_exe;

        if (cfg.auto_launch)
        {
            if (!cfg.worker_exe.empty() &&
                GetFileAttributesW(worker_exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                why = "the configured worker executable is not there";
                g.last_error = why;
                log_line(1, "[MGPU][RELAY] %s", why.c_str());
                return Status::WORKER_MISSING;
            }
            if (cfg.worker_exe.empty())
            {
                std::string why_avail;
                if (!available(why_avail))
                {
                    // THE HONEST ANSWER WHEN THE WORKER IS NOT DEPLOYED: the
                    // add-on is not broken, the neural path simply cannot exist.
                    // This is the normal state of an install that has only the
                    // add-on, and it is a warning, never an error.
                    why = why_avail + " - the relay is inert; the add-on continues without "
                                      "neural mode";
                    g.last_error = why;
                    log_line(1, "[MGPU][RELAY] %s", why.c_str());
                    return Status::WORKER_MISSING;
                }
            }
            if (!cfg.protocol_only)
            {
                // The runtime the worker will be told to load, checked here as
                // well as in the worker so the failure is reported by the
                // process that is looking at the deployment, with the path.
                const std::wstring dll = cfg.runtime.path.empty() ? default_runtime_path(dir)
                                                                  : cfg.runtime.path;
                if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    std::string dll_narrow;
                    wide_to_narrow(dll, dll_narrow);
                    why = "the neural runtime is MISSING at " + dll_narrow +
                          "; the worker would refuse to run, so it is not launched";
                    g.last_error = why;
                    log_line(2, "[MGPU][RELAY] %s", why.c_str());
                    return Status::RUNTIME_MISSING;
                }
            }
        }

        // ---- launch, or do not -------------------------------------------
        if (cfg.auto_launch)
        {
            std::wstring cmd;
            if (!build_worker_command(cfg, dir, cmd, why))
            {
                g.last_error = why;
                log_line(2, "[MGPU][RELAY] %s", why.c_str());
                return Status::BAD_CONFIG;
            }

            STARTUPINFOW si{};
            si.cb = sizeof si;
            std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
            mutable_cmd.push_back(L'\0');
            // CREATE_NO_WINDOW: the worker is a service to the game and must
            // never flash a console. It logs to its own file and to stdout,
            // which the game does not own.
            const BOOL ok = CreateProcessW(g.exe.c_str(), mutable_cmd.data(), nullptr, nullptr,
                                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &g.pi);
            if (!ok)
            {
                char b[224];
                std::snprintf(b, sizeof b, "CreateProcessW failed (%lu) for the worker",
                              (unsigned long)GetLastError());
                why = b;
                g.last_error = why;
                log_line(2, "[MGPU][RELAY] %s", why.c_str());
                return Status::PROCESS_FAILED;
            }
            g.owns_worker = true;
            CloseHandle(g.pi.hThread);
            g.pi.hThread = nullptr;
            {
                std::string cmd_narrow;
                wide_to_narrow(cmd, cmd_narrow);
                // The whole command line, so which binary, which runtime and
                // which hash were used is a fact in the log rather than a
                // reconstruction from the code.
                log_line(0, "[MGPU][RELAY] launched worker pid=%lu no-console=1 cmd=%s",
                         (unsigned long)g.pi.dwProcessId, cmd_narrow.c_str());
            }
        }

        // ---- the pipe ----------------------------------------------------
        if (cfg.pipe_name.empty())
        {
            char b[128];
            std::snprintf(b, sizeof b, "\\\\.\\pipe\\MGPU_NR_%lu",
                          (unsigned long)(g.owns_worker ? g.pi.dwProcessId : GetCurrentProcessId()));
            g.pipe = narrow_to_wide(b);
        }
        else
        {
            g.pipe = cfg.pipe_name;
        }
        if (g.pipe.empty())
        {
            why = "the pipe name could not be resolved";
            g.last_error = why;
            shutdown();
            return Status::BAD_CONFIG;
        }
        wide_to_narrow(g.pipe, g.pipe_narrow);
        log_line(0, "[MGPU][RELAY] pipe=%s", g.pipe_narrow.c_str());

        if (!pipe_open(g.pipe, cfg.connect_ms, why))
        {
            g.last_error = why;
            log_line(2, "[MGPU][RELAY] %s", why.c_str());
            shutdown();
            return Status::PIPE_FAILED;
        }

        // ---- HELLO / WORKER_READY ----------------------------------------
        {
            wire::Hello hello;
            wire::init(hello, wire::Kind::HELLO, 0u);
            hello.client_pid = (std::uint32_t)GetCurrentProcessId();
            hello.client_protocol_version = wire::PROTO_VERSION;
            hello.client_build = 1u;
            // Host-staged: this client expects mappings, never D3D12 surfaces.
            hello.wants_transport = 0u;
            wire::set_text(hello.client_name, wire::PROTO_NAME_MAX, "mgpu_nr_relay");
            if (!write_msg(hello, 4000u, why))
            {
                g.last_error = why;
                log_line(2, "[MGPU][RELAY] HELLO: %s", why.c_str());
                shutdown();
                return Status::HANDSHAKE_FAILED;
            }
        }

        wire::WorkerReady ready;
        if (!read_msg(*(wire::Error *)&ready, wire::Kind::WORKER_READY, cfg.handshake_ms, why))
        {
            g.last_error = why;
            log_line(2, "[MGPU][RELAY] WORKER_READY: %s", why.c_str());
            shutdown();
            return Status::HANDSHAKE_FAILED;
        }
        g.reserved18 = ready.reserved18;

        // ---- the one line the eventual rig verification greps for ---------
        // EXACT FORMAT, and the name is honest: "Success" only for the worker's
        // own 1. -1 is "not-a-session" because a --protocol-only worker never
        // created one, and that is a different statement from "the session
        // failed", which is why it is spelled differently.
        {
            char line[256];
            std::snprintf(line, sizeof line,
                          "[MGPU][RELAY] CreateFeature(Reserved18) result=0x%08X (%s) src=worker",
                          (unsigned)ready.reserved18, reserved18_name(ready.reserved18));
            g.evidence = line;
            // Level 1 when it is not a session at all: a warning, not an error -
            // the add-on is expected to keep running without neural mode.
            log_line(reserved18_is_success(ready.reserved18) ? 0 : 1, "%s", line);
        }

        // ---- the worker's own facts, one line each ------------------------
        log_line(0, "[MGPU][RELAY] worker descriptor=%d cumodule=%d first_blob=%llu "
                    "reserved18_handle_nonzero=%u",
                 ready.descriptor_status, ready.cumodule_status,
                 (unsigned long long)ready.first_blob_size,
                 ready.reserved18_handle_nonzero);
        log_line(0, "[MGPU][RELAY] worker adapter vendor=0x%04X device=0x%04X luid=%08X:%08X",
                 ready.adapter_vendor, ready.adapter_device,
                 ready.luid_high, ready.luid_low);
        log_line(0, "[MGPU][RELAY] worker name=\"%s\" worker_pid=%lu client_pid=%lu",
                 ready.worker_name, (unsigned long)g.pi.dwProcessId,
                 (unsigned long)GetCurrentProcessId());
        log_line(0, "[MGPU][RELAY] frame_slots=%u pitch_align=256 "
                    "geometry per slot is logged once, below",
                 (unsigned)wire::FRAME_SLOT_COUNT);

        // ---- the four slots, and the four CONFIG messages ------------------
        // PUBLISHED EVEN WHEN reserved18 IS NOT A SESSION. The worker has
        // already answered WORKER_READY, so it is listening; sending the
        // metadata lets a protocol-only run prove the whole control path - the
        // four names, the geometry, the pitches - with no GPU at all, which is
        // what the CI selftest is. Nothing is submitted to a worker that has no
        // session: submit() refuses.
        //
        // EACH SLOT IS PUBLISHED FROM ITS OWN NUMBERS. There is no shared
        // geometry here on purpose: the lane's colour is 3840x2160 fmt=28 while
        // its motion vectors are 1920x1080 fmt=10 in the same frame, and a
        // single published geometry would make the worker read the wrong number
        // of rows and still answer hr == 0.
        {
            const wire::InputSlot order[4] = { wire::InputSlot::COLOR, wire::InputSlot::DEPTH,
                                               wire::InputSlot::MOTION_VECTORS,
                                               wire::InputSlot::OUTPUT };
            for (unsigned i = 0u; i < 4u; ++i)
            {
                const SlotConfig *sc = config_slot(cfg, i);
                if (sc == nullptr)
                {
                    why = "an input slot has no configuration";
                    g.last_error = why;
                    shutdown();
                    return Status::BAD_CONFIG;
                }
                if (sc->width == 0u || sc->height == 0u || sc->dxgi_format == 0u)
                {
                    char b[192];
                    std::snprintf(b, sizeof b,
                                  "the %s slot has no usable geometry or format "
                                  "(width=%u height=%u fmt=%u)",
                                  slot_name(order[i]), sc->width, sc->height, sc->dxgi_format);
                    why = b;
                    g.last_error = why;
                    log_line(2, "[MGPU][RELAY] %s", why.c_str());
                    shutdown();
                    return Status::BAD_CONFIG;
                }

                Slot &s = g.slots[(unsigned)order[i]];
                if (!slot_publish(s, order[i], g.pipe, sc->width, sc->height, sc->dxgi_format,
                                  why))
                {
                    g.last_error = why;
                    log_line(2, "[MGPU][RELAY] %s", why.c_str());
                    shutdown();
                    return Status::SLOT_FAILED;
                }
                // ONE LINE PER SLOT, AT START. A rig log then shows what each
                // slot actually carries - which is the fact a truncated-frame
                // report would otherwise have to be reconstructed from.
                log_line(0, "[MGPU][RELAY] slot %-14s %s %ux%u fmt=%u bpp=%u row_bytes=%u "
                            "row_pitch=%u slice_pitch=%u bytes=%llu",
                         slot_name(order[i]), s.name.c_str(), s.width, s.height, s.dxgi_format,
                         s.bpp, s.row_bytes, s.row_pitch, s.slice_pitch,
                         (unsigned long long)s.bytes);
                if (!send_config(s, why))
                {
                    g.last_error = why;
                    log_line(2, "[MGPU][RELAY] CONFIG %s: %s", slot_name(order[i]), why.c_str());
                    shutdown();
                    return Status::SLOT_FAILED;
                }
            }
            log_line(0, "[MGPU][RELAY] sent %u CONFIG message(s); no pixel data crosses the pipe",
                     (unsigned)wire::InputSlot::COUNT);
        }

        g.started = true;
        return Status::OK;
    }

    void shutdown()
    {
        if (g.started && g.pipe_handle != nullptr)
        {
            // Best effort, short deadline: the worker may already be gone, and
            // that is not a failure.
            wire::Shutdown sd;
            wire::init(sd, wire::Kind::SHUTDOWN, 0u);
            sd.reason = 0u;
            std::string ignored;
            (void)write_msg(sd, 300u, ignored);
        }

        if (g.owns_worker && g.pi.hProcess != nullptr)
        {
            const DWORD wait_ms = (g.cfg.shutdown_ms == 0u) ? 1u : g.cfg.shutdown_ms;
            const DWORD w = WaitForSingleObject(g.pi.hProcess, wait_ms);
            if (w != WAIT_OBJECT_0)
            {
                // It did not take the SHUTDOWN. It is OUR process - we started
                // it - so it does not outlive this call. A worker left behind
                // holds an RTX 4070 device and a Reserved18 session for nobody.
                log_line(1, "[MGPU][RELAY] the worker did not exit in %lu ms; terminating it",
                         (unsigned long)wait_ms);
                TerminateProcess(g.pi.hProcess, 2u);
                WaitForSingleObject(g.pi.hProcess, 1000u);
            }
            CloseHandle(g.pi.hProcess);
            g.pi.hProcess = nullptr;
            if (g.pi.hThread != nullptr) { CloseHandle(g.pi.hThread); g.pi.hThread = nullptr; }
        }

        pipe_close();
        slots_release();
        g.started = false;
        g.owns_worker = false;
        g.reserved18 = -1;
        g.pipe.clear();
        g.pipe_narrow.clear();
        g.exe.clear();
        g.cfg = Config();
    }

    bool ready()
    {
        return g.started && g.pipe_handle != nullptr;
    }

    std::int32_t reserved18()
    {
        return g.reserved18;
    }

    const std::string &reserved18_evidence()
    {
        return g.evidence;
    }

    const std::string &last_error()
    {
        return g.last_error;
    }

    std::uint32_t worker_pid()
    {
        return (std::uint32_t)g.pi.dwProcessId;
    }

    bool slot_info(unsigned slot_index, SlotInfo &out)
    {
        if (slot_index >= (unsigned)wire::InputSlot::COUNT) return false;
        const Slot &s = g.slots[slot_index];
        if (!s.published) return false;
        // THIS slot's numbers, not a session-wide geometry: the four differ.
        out.width = s.width;
        out.height = s.height;
        out.dxgi_format = s.dxgi_format;
        out.row_pitch = s.row_pitch;
        out.slice_pitch = s.slice_pitch;
        out.bytes = s.bytes;
        out.mapping_name = s.name;
        return true;
    }

    // ================================================================= frames

    bool submit(const FrameView &frame, std::uint32_t frame_id, SubmitResult &out,
                std::string &why)
    {
        out = SubmitResult();
        if (!ready())
        {
            why = "the relay is not started";
            g.last_error = why;
            return false;
        }
        if (frame.color == nullptr || frame.depth == nullptr ||
            frame.motion_vectors == nullptr)
        {
            why = "a frame input pointer is null";
            g.last_error = why;
            return false;
        }
        if (frame.height == 0u)
        {
            why = "the frame declares 0 rows; the worker would read one row of the slot's data "
                  "and answer about a frame that was never there";
            g.last_error = why;
            return false;
        }
        // A worker with no session can not evaluate anything. Submitting to it
        // would move pixels nobody reads and let a caller believe a frame was
        // relayed, so this refuses instead - and says which worker it was.
        if (!reserved18_is_success(g.reserved18))
        {
            char b[192];
            std::snprintf(b, sizeof b,
                          "the worker reported Reserved18=0x%08X (%s); it has no neural session "
                          "and no frame may be submitted to it",
                          (unsigned)g.reserved18, reserved18_name(g.reserved18));
            why = b;
            g.last_error = why;
            return false;
        }

        struct Input { const void *src; unsigned stride; unsigned row_bytes; };
        // A zero stride means the buffer is tightly packed: rows follow each
        // other, so the stride IS the row length. Derived here rather than
        // refused, because a zero stride is not something a caller can see in
        // an int.
        const Input inputs[3] = {
            { frame.color,          frame.color_stride  ? frame.color_stride  : frame.color_row_bytes,
              frame.color_row_bytes  },
            { frame.depth,          frame.depth_stride  ? frame.depth_stride  : frame.depth_row_bytes,
              frame.depth_row_bytes  },
            { frame.motion_vectors,
              frame.motion_stride ? frame.motion_stride : frame.motion_row_bytes,
              frame.motion_row_bytes },
        };
        const wire::InputSlot classes[3] = { wire::InputSlot::COLOR, wire::InputSlot::DEPTH,
                                             wire::InputSlot::MOTION_VECTORS };

        // ---- THE GEOMETRY CONTRACT, CHECKED PER SLOT ----------------------
        //
        // THIS IS THE CHECK THAT MAKES "hr == 0" MEAN SOMETHING. A frame whose
        // motion vectors are 1920x1080 handed to a slot published at 3840x2160
        // would otherwise be read by the worker as the top half of the wrong
        // buffer, evaluated, and returned with hr == 0 and a changed OUTPUT -
        // every rule this module has would pass on a frame that was never fully
        // used. So: the row count must be the slot's, the row LENGTH must be
        // exactly the slot's (bpp * width, not merely something that fits under
        // the pitch), and the stride must not be shorter than the row. Three
        // separate refusals, each naming the slot and both geometries, and each
        // a returned failure rather than a clamp.
        for (unsigned i = 0u; i < 3u; ++i)
        {
            const Slot &s = g.slots[(unsigned)classes[i]];
            const Input &in = inputs[i];
            if (!s.published || s.view == nullptr)
            {
                char b[160];
                std::snprintf(b, sizeof b, "the %s slot was never published",
                              slot_name(classes[i]));
                why = b;
                g.last_error = why;
                return false;
            }
            if (frame.height != s.height)
            {
                char b[256];
                std::snprintf(b, sizeof b,
                              "the frame has %u rows and the %s slot was published with %u; the "
                              "worker takes its row count from the slot's own height, so this "
                              "frame would be truncated or over-read. The relay does not "
                              "resample or crop - reduce the frame to the published size first.",
                              frame.height, slot_name(classes[i]), s.height);
                why = b;
                g.last_error = why;
                return false;
            }
            if (in.row_bytes != s.row_bytes)
            {
                char b[288];
                std::snprintf(b, sizeof b,
                              "the %s rows are %u bytes and the slot was published at %u bytes "
                              "(%ux%u fmt=%u, %u bytes per pixel); this is not the geometry the "
                              "slot was configured for",
                              slot_name(classes[i]), in.row_bytes, s.row_bytes, s.width, s.height,
                              s.dxgi_format, s.bpp);
                why = b;
                g.last_error = why;
                return false;
            }
            if (in.stride < in.row_bytes)
            {
                char b[256];
                std::snprintf(b, sizeof b,
                              "the %s stride is %u bytes and a row is %u; the relay would read "
                              "past the end of each row",
                              slot_name(classes[i]), in.stride, in.row_bytes);
                why = b;
                g.last_error = why;
                return false;
            }
        }

        // The frame slot is chosen by the relay: the worker releases a slot as
        // soon as it answers FRAME_COMPLETE, and this client is strictly
        // request/response, so a three-deep ring in submit order can never
        // present the worker a slot it still owns.
        const unsigned slot = (unsigned)(frame_id % wire::FRAME_SLOT_COUNT);

        for (unsigned i = 0u; i < 3u; ++i)
        {
            const Slot &s = g.slots[(unsigned)classes[i]];
            const Input &in = inputs[i];
            const unsigned char *src = (const unsigned char *)in.src;
            unsigned char *dst = s.view + (std::size_t)slot * s.slice_pitch;
            for (unsigned y = 0u; y < s.height; ++y)
            {
                std::memcpy(dst + (std::size_t)y * s.row_pitch,
                            src + (std::size_t)y * in.stride, in.row_bytes);
                // The tail of the row, from the logical width to the aligned
                // pitch, is cleared. The worker uploads row_bytes per row, so
                // it never reads those bytes, but the OUTPUT mapping is read
                // back by the game side and a stale tail there is a bug waiting
                // for the next person.
                if (s.row_pitch > in.row_bytes)
                    std::memset(dst + (std::size_t)y * s.row_pitch + in.row_bytes, 0,
                                (std::size_t)(s.row_pitch - in.row_bytes));
            }
        }

        // Snapshot the OUTPUT slice as the game left it. "The worker wrote the
        // output" is then a fact about memory, and it is reported as a fact
        // rather than as a verdict about the image.
        //
        // NON-CONST on purpose: `fill` is this slot's own snapshot buffer and
        // assign() writes into it. Taking the slot through a const reference
        // (as the first draft did) makes the call ill-formed - C2662 - because
        // the snapshot is state the relay keeps, not something it reads.
        {
            Slot &o = g.slots[(unsigned)wire::InputSlot::OUTPUT];
            if (!o.published || o.view == nullptr)
            {
                why = "the OUTPUT slot was never published";
                g.last_error = why;
                return false;
            }
            o.fill.assign(o.view + (std::size_t)slot * o.slice_pitch,
                          o.view + (std::size_t)slot * o.slice_pitch + o.slice_pitch);
        }

        // ---- FRAME_SUBMIT -------------------------------------------------
        wire::FrameSubmit sub;
        wire::init(sub, wire::Kind::FRAME_SUBMIT, frame_id);
        {
            const Slot &c = g.slots[(unsigned)wire::InputSlot::COLOR];
            sub.slot = slot;
            sub.input_mask = 0x7u;   // COLOR | DEPTH | MOTION_VECTORS; OUTPUT is the result
            // The COLOUR slot's numbers, which is what the worker records on the
            // ring for this frame. The per-class numbers it actually reads with
            // come from each slot's own CONFIG, which is why they can differ.
            sub.width = c.width;
            sub.height = c.height;
            sub.dxgi_format = c.dxgi_format;
            sub.row_pitch = c.row_pitch;
            sub.slice_pitch = c.slice_pitch;
            // The client's own layout decides the offset - the worker adds the
            // per-class resource_offset from CONFIG on top of it.
            sub.resource_offset = slot * c.slice_pitch;
            sub.ingress_fence_value = 0ull;   // host-staged: no GPU work to wait for
        }
        if (!write_msg(sub, 20000u, why))
        {
            g.last_error = why;
            log_line(2, "[MGPU][RELAY] FRAME_SUBMIT frame_id=%u: %s", frame_id, why.c_str());
            return false;
        }

        wire::FrameComplete done;
        if (!read_msg(*(wire::Error *)&done, wire::Kind::FRAME_COMPLETE, 60000u, why))
        {
            g.last_error = why;
            log_line(2, "[MGPU][RELAY] FRAME_COMPLETE frame_id=%u: %s", frame_id, why.c_str());
            return false;
        }

        out.completed = true;
        out.worker_hr = done.hr;                      // VERBATIM, never derived
        out.egress_fence = done.egress_fence_value;   // VERBATIM
        out.gpu_ticks = (std::uint64_t)done.gpu_ticks;
        out.slot = done.slot;
        out.frame_id = done.h.frame_id;
        {
            const Slot &o = g.slots[(unsigned)wire::InputSlot::OUTPUT];
            const unsigned char *now = o.view + (std::size_t)slot * o.slice_pitch;
            out.output_changed = (o.fill.size() == (std::size_t)o.slice_pitch) &&
                                 (std::memcmp(now, o.fill.data(), o.slice_pitch) != 0);
        }

        // THE HONESTY RULE FOR FRAMES: evaluated only when the worker's own hr
        // says so. There is no "probably", and a zero-filled output that the
        // worker did not write is reported as NOT written.
        log_line(0, "[MGPU][RELAY] frame_id=%u slot=%u hr=0x%08X egress_fence=%llu "
                    "output_written=%s evaluated=%s",
                 frame_id, done.slot, (unsigned)done.hr,
                 (unsigned long long)done.egress_fence_value,
                 out.output_changed ? "yes" : "no",
                 (out.worker_hr == 0 && out.output_changed) ? "yes" : "NO");
        return true;
    }

    // =======================================================================
    // The wire-size table, defined out of line so the header of the file stays
    // readable and so sizeof_expected() can be declared inside the wire block.
    // =======================================================================
}   // namespace mgpu::relay

namespace mgpu::relay::wire
{
    std::uint32_t sizeof_expected(Kind k)
    {
        switch (k)
        {
        case Kind::HELLO:          return (std::uint32_t)sizeof(Hello);
        case Kind::WORKER_READY:   return (std::uint32_t)sizeof(WorkerReady);
        case Kind::CONFIG:         return (std::uint32_t)sizeof(Config);
        case Kind::FRAME_SUBMIT:   return (std::uint32_t)sizeof(FrameSubmit);
        case Kind::FRAME_COMPLETE: return (std::uint32_t)sizeof(FrameComplete);
        case Kind::SHUTDOWN:       return (std::uint32_t)sizeof(Shutdown);
        case Kind::ERROR_MESSAGE:  return (std::uint32_t)sizeof(Error);
        }
        return 0u;
    }
}

#endif   // MGPU_NR_RELAY
