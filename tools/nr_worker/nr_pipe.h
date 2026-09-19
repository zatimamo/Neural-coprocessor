// ============================================================================
// MGPU NR worker - the named-pipe control channel (protocol v1 framing).
//
//     \\.\pipe\MGPU_NR_<pid>
//
// WHY A NAMED PIPE, AND WHY OVERLAPPED
//     Control traffic is a few hundred bytes per frame, so a pipe is the right
//     shape and shared memory would be overkill. Overlapped I/O is not
//     decoration either: this code runs inside, or beside, a game. Every read
//     and every write has a deadline, and a peer that stops responding turns
//     into a logged disconnect that disables the neural path - never into a
//     hang. The game must not be able to freeze because the worker stopped
//     answering.
//
// ONE CLIENT AT A TIME
//     The worker serves exactly one game process. A second connection is
//     refused by construction (nMaxInstances = 1) rather than queued.
// ============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace nr
{
    enum class IoResult
    {
        OK,
        TIMEOUT,
        DISCONNECTED,
        ERROR,
    };

    const char *io_result_name(IoResult r);

    //: One duplex pipe endpoint. Used as the server by the worker and as the
    //: client by the game side / the probe, because the framing is identical in
    //: both directions.
    class Pipe
    {
    public:
        Pipe() = default;
        ~Pipe();
        Pipe(const Pipe &) = delete;
        Pipe &operator=(const Pipe &) = delete;

        //: The server side: create the pipe and wait for exactly one client.
        //: `name` is the full \\.\pipe\... name.
        bool listen_and_accept(const wchar_t *name, std::uint32_t timeout_ms,
                               std::string &err);

        //: The client side: connect to an existing pipe, retrying until the
        //: deadline, because the worker may still be starting.
        bool connect(const wchar_t *name, std::uint32_t timeout_ms, std::string &err);

        //: Exactly `n` bytes, or a reason. Reads are bounded by the deadline.
        IoResult read_exact(void *buf, std::uint32_t n, std::uint32_t timeout_ms,
                            std::string &err);

        IoResult write_all(const void *buf, std::uint32_t n, std::uint32_t timeout_ms,
                           std::string &err);

        bool connected() const { return handle_ != nullptr; }
        void close();

        //: Messages are fixed structs, so "read a message" is: read the header,
        //: then the remainder the header declares. This is the only place the
        //: framing is interpreted.
        template <typename T>
        IoResult read_message(T &msg, std::uint32_t timeout_ms, std::string &err)
        {
            // Zeroed first: a message shorter than this build's struct must not
            // leave the tail holding whatever was on the caller's stack.
            std::memset(&msg, 0, sizeof msg);
            IoResult r = read_exact(&msg, (std::uint32_t)sizeof(Header), timeout_ms, err);
            if (r != IoResult::OK) return r;
            const std::uint32_t size = msg.h.size;
            if (size < sizeof(Header) || size > sizeof(T))
            {
                err = "the peer declared a message size this build cannot hold";
                return IoResult::ERROR;
            }
            if (size == sizeof(Header)) return IoResult::OK;
            return read_exact(reinterpret_cast<unsigned char *>(&msg) + sizeof(Header),
                              (std::uint32_t)(size - sizeof(Header)), timeout_ms, err);
        }

        template <typename T>
        IoResult write_message(const T &msg, std::uint32_t timeout_ms, std::string &err)
        {
            return write_all(&msg, (std::uint32_t)sizeof(T), timeout_ms, err);
        }

    private:
        void *handle_ = nullptr;      // HANDLE, kept void* so the header stays light
        void *event_ = nullptr;       // manual-reset overlapped completion event
    };

    //: Milliseconds since process start, for deadlines.
    std::uint64_t now_ms();
    void sleep_ms(std::uint32_t ms);
}
