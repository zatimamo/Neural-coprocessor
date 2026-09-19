// ============================================================================
// MGPU NR worker - control protocol v1, implementation.
// ============================================================================

#include "nr_proto.h"

#include <cstdio>
#include <cwchar>

namespace nr
{
    const char *kind_name(Kind k)
    {
        switch (k)
        {
        case Kind::HELLO:         return "HELLO";
        case Kind::WORKER_READY:  return "WORKER_READY";
        case Kind::CONFIG:        return "CONFIG";
        case Kind::FRAME_SUBMIT:  return "FRAME_SUBMIT";
        case Kind::FRAME_COMPLETE:return "FRAME_COMPLETE";
        case Kind::SHUTDOWN:      return "SHUTDOWN";
        case Kind::ERROR:         return "ERROR";
        }
        return "UNKNOWN";
    }

    std::uint32_t size_for(Kind k)
    {
        switch (k)
        {
        case Kind::HELLO:          return (std::uint32_t)sizeof(Hello);
        case Kind::WORKER_READY:   return (std::uint32_t)sizeof(WorkerReady);
        case Kind::CONFIG:         return (std::uint32_t)sizeof(Config);
        case Kind::FRAME_SUBMIT:   return (std::uint32_t)sizeof(FrameSubmit);
        case Kind::FRAME_COMPLETE: return (std::uint32_t)sizeof(FrameComplete);
        case Kind::SHUTDOWN:       return (std::uint32_t)sizeof(Shutdown);
        case Kind::ERROR:          return (std::uint32_t)sizeof(Error);
        }
        return 0u;
    }

    const char *header_problem(const Header &h, Kind want)
    {
        if (h.magic != PROTO_MAGIC)            return "magic is not MGPU";
        if (h.version != PROTO_VERSION)        return "protocol version is not 1";
        if (h.kind != (std::uint16_t)want)     return "message kind is not the one expected here";
        if (h.size != size_for(want))          return "struct size does not match version 1";
        return "";
    }

    bool header_ok(const Header &h, Kind want)
    {
        return header_problem(h, want)[0] == '\0';
    }

    void set_text(char *dst, std::uint32_t cap, const char *src)
    {
        if (dst == nullptr || cap == 0u) return;
        std::uint32_t i = 0;
        if (src != nullptr)
        {
            for (; i + 1u < cap && src[i] != '\0'; ++i) dst[i] = src[i];
        }
        dst[i] = '\0';
    }

    void pipe_name_for_pid(std::uint32_t pid, wchar_t *out, std::uint32_t out_chars)
    {
        if (out == nullptr || out_chars == 0u) return;
        std::swprintf(out, out_chars, L"\\\\.\\pipe\\MGPU_NR_%lu", (unsigned long)pid);
    }

    bool magic_bytes_are_mgpu()
    {
        // A byte-order assertion, not a tautology: the constant is written so
        // that the FOURCC reads 'M','G','P','U' in memory order on a
        // little-endian machine, which is the only order this protocol defines.
        const std::uint32_t m = PROTO_MAGIC;
        const unsigned char *p = reinterpret_cast<const unsigned char *>(&m);
        return p[0] == 'M' && p[1] == 'G' && p[2] == 'P' && p[3] == 'U';
    }
}
