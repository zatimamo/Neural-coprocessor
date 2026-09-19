// ============================================================================
// MGPU NR worker - named-pipe implementation. See nr_pipe.h for why.
// ============================================================================

#include "nr_pipe.h"

#include "nr_proto.h"

#include <windows.h>

#include <cstdio>

namespace nr
{
    const char *io_result_name(IoResult r)
    {
        switch (r)
        {
        case IoResult::OK:           return "OK";
        case IoResult::TIMEOUT:      return "TIMEOUT";
        case IoResult::DISCONNECTED: return "DISCONNECTED";
        case IoResult::IO_ERROR:     return "ERROR";
        }
        return "?";
    }

    std::uint64_t now_ms()
    {
        return (std::uint64_t)GetTickCount64();
    }

    void sleep_ms(std::uint32_t ms)
    {
        Sleep(ms);
    }

    Pipe::~Pipe()
    {
        close();
    }

    void Pipe::close()
    {
        if (handle_ != nullptr)
        {
            HANDLE h = (HANDLE)handle_;
            // Cancel anything still pending before closing, so a timed-out
            // overlapped read cannot complete into a closed handle.
            CancelIoEx(h, nullptr);
            DisconnectNamedPipe(h);   // a no-op failure on the client side
            CloseHandle(h);
            handle_ = nullptr;
        }
        if (event_ != nullptr)
        {
            CloseHandle((HANDLE)event_);
            event_ = nullptr;
        }
    }

    bool Pipe::listen_and_accept(const wchar_t *name, std::uint32_t timeout_ms,
                                 std::string &err)
    {
        close();

        const DWORD in_out = 64u * 1024u;
        HANDLE h = CreateNamedPipeW(
            name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,                       // exactly one client: the game process
            in_out, in_out,
            0,                       // default timeout, only used by WaitNamedPipe clients
            nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            char buf[128];
            std::snprintf(buf, sizeof buf, "CreateNamedPipeW failed (%lu)",
                          (unsigned long)GetLastError());
            err = buf;
            return false;
        }

        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ev == nullptr)
        {
            CloseHandle(h);
            err = "CreateEventW failed for the pipe";
            return false;
        }

        handle_ = h;
        event_ = ev;

        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        const BOOL ok = ConnectNamedPipe(h, &ov);
        if (ok)
        {
            return true;             // connected without waiting
        }
        const DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED)
        {
            return true;             // a client was already there
        }
        if (e != ERROR_IO_PENDING)
        {
            char buf[128];
            std::snprintf(buf, sizeof buf, "ConnectNamedPipe failed (%lu)", (unsigned long)e);
            err = buf;
            close();
            return false;
        }
        if (WaitForSingleObject(ev, timeout_ms) != WAIT_OBJECT_0)
        {
            err = "no client connected before the deadline";
            close();
            return false;
        }
        DWORD moved = 0;
        if (!GetOverlappedResult(h, &ov, &moved, FALSE))
        {
            err = "ConnectNamedPipe did not complete";
            close();
            return false;
        }
        return true;
    }

    bool Pipe::connect(const wchar_t *name, std::uint32_t timeout_ms, std::string &err)
    {
        close();
        const std::uint64_t deadline = now_ms() + timeout_ms;
        for (;;)
        {
            HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
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
                handle_ = h;
                event_ = ev;
                return true;
            }
            const DWORD e = GetLastError();
            if (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND)
            {
                char buf[128];
                std::snprintf(buf, sizeof buf, "CreateFileW(%ls) failed (%lu)", name,
                              (unsigned long)e);
                err = buf;
                return false;
            }
            if (now_ms() >= deadline)
            {
                err = "the pipe was not there before the deadline";
                return false;
            }
            // ERROR_PIPE_BUSY: wait for the instance; ERROR_FILE_NOT_FOUND: the
            // worker has not created it yet. Both are "try again shortly".
            WaitNamedPipeW(name, 50);
            sleep_ms(20);
        }
    }

    IoResult Pipe::read_exact(void *buf, std::uint32_t n, std::uint32_t timeout_ms,
                              std::string &err)
    {
        if (handle_ == nullptr) { err = "the pipe is not open"; return IoResult::DISCONNECTED; }
        unsigned char *p = (unsigned char *)buf;
        std::uint32_t got = 0;
        const std::uint64_t deadline = now_ms() + timeout_ms;

        while (got < n)
        {
            const std::uint64_t now = now_ms();
            if (now >= deadline) { err = "read timed out"; return IoResult::TIMEOUT; }
            const DWORD slice = (DWORD)(deadline - now);

            HANDLE h = (HANDLE)handle_;
            OVERLAPPED ov{};
            ov.hEvent = (HANDLE)event_;
            ResetEvent((HANDLE)event_);
            DWORD moved = 0;
            const BOOL ok = ReadFile(h, p + got, n - got, nullptr, &ov);
            if (!ok)
            {
                const DWORD e = GetLastError();
                if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED ||
                    e == ERROR_NO_DATA || e == ERROR_OPERATION_ABORTED)
                {
                    err = "the peer closed the pipe";
                    return IoResult::DISCONNECTED;
                }
                if (e != ERROR_IO_PENDING)
                {
                    char b[128];
                    std::snprintf(b, sizeof b, "ReadFile failed (%lu)", (unsigned long)e);
                    err = b;
                    return IoResult::IO_ERROR;
                }
                if (WaitForSingleObject((HANDLE)event_, slice) != WAIT_OBJECT_0)
                {
                    CancelIoEx(h, &ov);
                    // Let the cancelled read settle before the buffer goes away.
                    WaitForSingleObject((HANDLE)event_, 100);
                    err = "read timed out";
                    return IoResult::TIMEOUT;
                }
                if (!GetOverlappedResult(h, &ov, &moved, FALSE))
                {
                    const DWORD g = GetLastError();
                    if (g == ERROR_BROKEN_PIPE || g == ERROR_OPERATION_ABORTED)
                    {
                        err = "the peer closed the pipe";
                        return IoResult::DISCONNECTED;
                    }
                    char b[128];
                    std::snprintf(b, sizeof b, "GetOverlappedResult failed (%lu)",
                                  (unsigned long)g);
                    err = b;
                    return IoResult::IO_ERROR;
                }
            }
            else
            {
                if (!GetOverlappedResult(h, &ov, &moved, TRUE))
                {
                    err = "GetOverlappedResult failed for a completed read";
                    return IoResult::IO_ERROR;
                }
            }
            if (moved == 0)
            {
                err = "the peer closed the pipe";
                return IoResult::DISCONNECTED;
            }
            got += moved;
        }
        return IoResult::OK;
    }

    IoResult Pipe::write_all(const void *buf, std::uint32_t n, std::uint32_t timeout_ms,
                             std::string &err)
    {
        if (handle_ == nullptr) { err = "the pipe is not open"; return IoResult::DISCONNECTED; }
        const unsigned char *p = (const unsigned char *)buf;
        std::uint32_t sent = 0;
        const std::uint64_t deadline = now_ms() + timeout_ms;

        while (sent < n)
        {
            const std::uint64_t now = now_ms();
            if (now >= deadline) { err = "write timed out"; return IoResult::TIMEOUT; }
            const DWORD slice = (DWORD)(deadline - now);

            HANDLE h = (HANDLE)handle_;
            OVERLAPPED ov{};
            ov.hEvent = (HANDLE)event_;
            ResetEvent((HANDLE)event_);
            DWORD moved = 0;
            const BOOL ok = WriteFile(h, p + sent, n - sent, nullptr, &ov);
            if (!ok)
            {
                const DWORD e = GetLastError();
                if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED ||
                    e == ERROR_NO_DATA || e == ERROR_OPERATION_ABORTED)
                {
                    err = "the peer closed the pipe";
                    return IoResult::DISCONNECTED;
                }
                if (e != ERROR_IO_PENDING)
                {
                    char b[128];
                    std::snprintf(b, sizeof b, "WriteFile failed (%lu)", (unsigned long)e);
                    err = b;
                    return IoResult::IO_ERROR;
                }
                if (WaitForSingleObject((HANDLE)event_, slice) != WAIT_OBJECT_0)
                {
                    CancelIoEx(h, &ov);
                    WaitForSingleObject((HANDLE)event_, 100);
                    err = "write timed out";
                    return IoResult::TIMEOUT;
                }
                if (!GetOverlappedResult(h, &ov, &moved, FALSE))
                {
                    err = "the peer closed the pipe";
                    return IoResult::DISCONNECTED;
                }
            }
            else
            {
                if (!GetOverlappedResult(h, &ov, &moved, TRUE))
                {
                    err = "GetOverlappedResult failed for a completed write";
                    return IoResult::IO_ERROR;
                }
            }
            sent += moved;
        }
        return IoResult::OK;
    }
}
