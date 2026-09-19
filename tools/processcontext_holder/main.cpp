// ============================================================================
// holder_ti.exe - PROCESS A of the SPLITPROCESS_ISOLATION experiment.
//
// WHY THIS IS A SEPARATE PROGRAM AND NOT A FIFTH MODE
//   PROCESSCONTEXT established that the RTX 4070 lane fails INSIDE a process
//   that also holds active RTX 4070 Ti SUPER D3D12 state - a device, a DIRECT
//   command queue and a small CBV/SRV/UAV descriptor heap - while the SINGLE
//   arm alone reproduces the working result. The question that follows is
//   whether that state still breaks the lane when it is alive in a DIFFERENT
//   process.
//
//   That question is only readable if PROCESS B is the ALREADY-PROVEN
//   executable, byte for byte. Adding a holder mode to the diagnostic would have
//   changed the very binary the comparison is about, so the holder lives here
//   instead: its own directory, its own CMake project, its own binary, and no
//   shared source with tools/processcontext_ab. The diagnostic's sources are
//   frozen at the implementation that produced the PROCESSCONTEXT evidence, and
//   the CI gates prove it.
//
// WHAT IT DOES - THE WHOLE LIST
//   1. CreateDXGIFactory1
//   2. enumerate adapters and find vendor 0x10DE / device 0x2705
//      (the RTX 4070 Ti SUPER), skipping software adapters
//   3. D3D12CreateDevice on that adapter at feature level 11_0
//   4. CreateCommandQueue, type DIRECT
//   5. CreateDescriptorHeap, CBV/SRV/UAV, 8 descriptors, shader visible
//   6. print HOLDER_READY
//   7. hold those objects alive
//   8. wait for the named stop event - or an explicit byte on stdin, or stdin
//      closing, which is what happens if the parent dies
//   9. release, print HOLDER_STOPPED, exit 0
//
//   On any setup failure it prints HOLDER_FAILED reason=<token> and exits 1.
//
// WHAT IT DELIBERATELY DOES NOT DO
//   No NGX. No NVAPI, and no call into it. No architecture hook, no detour, no
//   MinHook. No CUDA. No NR runtime and no snippet. No second adapter, and in
//   particular no device on the RTX 4070. No swapchain. No command list, no
//   resource, no fence, no submission: the queue exists and is never used.
//
//   It cannot even name those things: the CI gate fails if the tokens are found
//   in this directory's sources OR in the built binary.
//
// WHY THE CONTROLS ARE SHAPED THIS WAY
//   The stop event is created and owned by the PARENT, before this starts, and
//   this process only OPENS it. There is therefore exactly one owner, and a
//   stale event left behind by an earlier run cannot be inherited. stdin is a
//   second, independent control: the parent holds the write end of a pipe, so a
//   parent that dies closes it - which is what keeps a holder from outliving
//   its orchestrator, and what keeps a failed run from leaving GPU state behind.
//
//   The wait cap is a third, weaker control. AutoLab passes 0, meaning no cap:
//   the parent is the control, and a holder that gave up on its own would end
//   the experiment while PROCESS B was still running.
//
// IT LINKS NOTHING BUT d3d12 AND dxgi, and it is built by its own CMake project
// with no NGX headers and no MinHook: there is no code path here that could load
// a neural runtime even by accident.
// ============================================================================

#include <windows.h>

#include <dxgi1_6.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

//: The adapter this process exists to hold state on, identified by PCI ids -
//: never by ordinal. An ordinal would silently pick a different card on a
//: machine with a different adapter order.
const unsigned TI_SUPER_VENDOR = 0x10DEu;
const unsigned TI_SUPER_DEVICE = 0x2705u;

FILE *g_log = nullptr;

// ------------------------------------------------------------------- logging
//
// One line, to stdout AND to the log file, FLUSHED. Flushing is not cosmetic: a
// parent reads HOLDER_READY from a pipe and archives the log after the process
// has been stopped, so an unflushed line is a line that does not exist.
void logf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);

    if (g_log != nullptr)
    {
        std::fputs(buf, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}

int fail(const char *reason)
{
    logf("HOLDER_FAILED reason=%s", reason);
    if (g_log != nullptr)
    {
        std::fclose(g_log);
        g_log = nullptr;
    }
    return 1;
}

// ------------------------------------------------------------ stdin liveness
//
// True while the parent still holds the write end. A byte the parent writes is
// an EXPLICIT STOP COMMAND; a closed pipe means the parent is gone.
bool stdin_still_open()
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return true;

    DWORD avail = 0;
    if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
    {
        if (avail == 0) return true;
        char buf[64];
        DWORD got = 0;
        if (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
        {
            logf("[holder] an explicit stop command arrived on stdin (%lu byte(s))",
                 (unsigned long)got);
            return false;
        }
        return true;
    }
    if (GetLastError() == ERROR_BROKEN_PIPE)
    {
        logf("[holder] stdin is closed - the parent is gone");
        return false;
    }
    // Not a pipe at all. The named event is then the only control, which is the
    // documented first choice anyway.
    return true;
}

// ------------------------------------------------------------------ arguments
struct Options
{
    const char *event_name = nullptr;      // owned by the parent; we only open it
    unsigned    cap_seconds = 0;           // 0 = no cap; the parent is the control
    const char *log_path = "context-holder.log";
    bool        help = false;
};

bool parse_args(int argc, char **argv, Options &opt)
{
    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
        if (std::strcmp(a, "--event") == 0 && i + 1 < argc)
        {
            opt.event_name = argv[++i];
        }
        else if (std::strcmp(a, "--seconds") == 0 && i + 1 < argc)
        {
            opt.cap_seconds = (unsigned)std::strtoul(argv[++i], nullptr, 10);
        }
        else if (std::strcmp(a, "--log") == 0 && i + 1 < argc)
        {
            opt.log_path = argv[++i];
        }
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0)
        {
            opt.help = true;
        }
        else
        {
            std::fprintf(stderr, "holder_ti: unrecognised argument \"%s\"\n", a);
            return false;
        }
    }
    return true;
}

void usage()
{
    std::fprintf(stderr,
                 "holder_ti - hold active RTX 4070 Ti SUPER D3D12 state (device +\n"
                 "            DIRECT queue + CBV/SRV/UAV heap) and nothing else.\n"
                 "\n"
                 "  --event <name>    a named stop event the PARENT owns (recommended)\n"
                 "  --seconds <n>     wait cap; 0 = no cap (the parent is the control)\n"
                 "  --log <path>      also write the log here (default context-holder.log)\n"
                 "\n"
                 "Prints HOLDER_READY once the state is held, HOLDER_STOPPED after it\n"
                 "is released, or HOLDER_FAILED reason=<token> if it cannot be set up.\n");
}

// The event name arrives on an ASCII command line and is opened by its WIDE
// name, because that is how the parent created it.
bool to_wide(const char *in, wchar_t *out, size_t out_chars)
{
    if (in == nullptr) return false;
    return MultiByteToWideChar(CP_UTF8, 0, in, -1, out, (int)out_chars) > 0;
}

}   // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parse_args(argc, argv, opt))
    {
        usage();
        return 1;
    }
    if (opt.help)
    {
        usage();
        return 0;
    }

    // The log file is opened first, so that a failure during setup is recorded
    // where the parent will look for it.
    g_log = std::fopen(opt.log_path, "w");
    if (g_log == nullptr)
    {
        std::fprintf(stderr, "holder_ti: could not open the log \"%s\"\n", opt.log_path);
    }

    logf("================================================================================");
    logf("holder_ti  -  PROCESS A: active Ti SUPER D3D12 state, and nothing else");
    logf("================================================================================");
    logf("[holder] pid      = %lu", (unsigned long)GetCurrentProcessId());
    logf("[holder] cmdline  = %ls", GetCommandLineW());
    logf("[holder] NO NGX, NO NVAPI, NO architecture hook, NO CUDA, NO NR runtime, NO");
    logf("[holder] RTX 4070 device, NO swapchain, NO command list, NO submission.");

    // ---- 1 + 2. the factory, and the adapter, found by PCI id -------------
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    logf("[holder] CreateDXGIFactory1 hr=0x%08X factory=%p", (unsigned)hr, (void *)factory);
    if (FAILED(hr) || factory == nullptr) return fail("CREATE_DXGI_FACTORY_FAILED");

    IDXGIAdapter1 *ad_ti = nullptr;
    DXGI_ADAPTER_DESC1 d_ti{};
    bool found = false;
    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1 *candidate = nullptr;
        if (factory->EnumAdapters1(i, &candidate) != S_OK) break;
        if (candidate == nullptr) continue;

        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(candidate->GetDesc1(&d))
            && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0
            && d.VendorId == TI_SUPER_VENDOR
            && d.DeviceId == TI_SUPER_DEVICE)
        {
            ad_ti = candidate;
            d_ti = d;
            found = true;
            break;
        }
        candidate->Release();
    }

    if (!found || ad_ti == nullptr)
    {
        logf("[holder] no adapter with vendor 0x%04X device 0x%04X was enumerated",
             TI_SUPER_VENDOR, TI_SUPER_DEVICE);
        factory->Release();
        return fail("NO_TI_SUPER_ADAPTER");
    }

    logf("[holder] adapter  = \"%ls\" vendor=0x%04X device=0x%04X luid=%08lX:%08lX "
         "vram=%llu MiB",
         d_ti.Description, d_ti.VendorId, d_ti.DeviceId,
         (unsigned long)d_ti.AdapterLuid.HighPart,
         (unsigned long)d_ti.AdapterLuid.LowPart,
         (unsigned long long)(d_ti.DedicatedVideoMemory / (1024ull * 1024ull)));

    // ---- 3. the device -----------------------------------------------------
    ID3D12Device *dev = nullptr;
    hr = D3D12CreateDevice(ad_ti, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    logf("[holder] D3D12CreateDevice(FL_11_0) hr=0x%08X device=%p", (unsigned)hr, (void *)dev);
    if (FAILED(hr) || dev == nullptr)
    {
        ad_ti->Release();
        factory->Release();
        return fail("D3D12_CREATE_DEVICE_FAILED");
    }

    // ---- 4. the DIRECT queue. Created, held, never used. -------------------
    ID3D12CommandQueue *queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    logf("[holder] CreateCommandQueue(DIRECT) hr=0x%08X queue=%p", (unsigned)hr, (void *)queue);
    if (FAILED(hr) || queue == nullptr)
    {
        dev->Release();
        ad_ti->Release();
        factory->Release();
        return fail("CREATE_COMMAND_QUEUE_FAILED");
    }

    // ---- 5. the descriptor heap -------------------------------------------
    ID3D12DescriptorHeap *heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    logf("[holder] CreateDescriptorHeap(CBV_SRV_UAV x8, SHADER_VISIBLE) hr=0x%08X heap=%p",
         (unsigned)hr, (void *)heap);
    if (FAILED(hr) || heap == nullptr)
    {
        queue->Release();
        dev->Release();
        ad_ti->Release();
        factory->Release();
        return fail("CREATE_DESCRIPTOR_HEAP_FAILED");
    }

    // ---- the stop control: a named event the PARENT owns -------------------
    HANDLE stop = nullptr;
    if (opt.event_name != nullptr && opt.event_name[0] != '\0')
    {
        wchar_t wide_event[256] = L"";
        if (to_wide(opt.event_name, wide_event, 256))
        {
            stop = OpenEventW(SYNCHRONIZE, FALSE, wide_event);
        }
        logf("[holder] stop event \"%ls\" -> %p%s", wide_event, (void *)stop,
             (stop != nullptr) ? "" : "  (could not be opened; stdin is the control)");
    }
    else
    {
        logf("[holder] no stop event was named; stdin is the control");
    }

    // ---- 6 + 7. the state is held -----------------------------------------
    logf("[holder] the state is established and will be HELD. Waiting for the stop "
         "signal: the named event, an explicit byte on stdin, or stdin closing - "
         "which is what happens if the parent dies. Cap %u seconds (0 = none).",
         opt.cap_seconds);
    logf("HOLDER_READY");

    const DWORD started = GetTickCount();
    const char *why = "the wait cap expired";
    for (;;)
    {
        if (stop != nullptr && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
        {
            why = "the named stop event was signalled";
            break;
        }
        if (!stdin_still_open())
        {
            why = "stdin closed, or an explicit stop command";
            break;
        }
        if (opt.cap_seconds != 0
            && (GetTickCount() - started) / 1000u >= opt.cap_seconds)
        {
            why = "the wait cap expired";
            break;
        }
        Sleep(100);
    }

    // ---- 9. release, then announce the stop -------------------------------
    logf("[holder] stopping: %s", why);
    if (stop != nullptr) CloseHandle(stop);
    heap->Release();
    queue->Release();
    dev->Release();
    ad_ti->Release();
    factory->Release();
    logf("[holder] held state released; nothing was ever submitted");
    logf("HOLDER_STOPPED");

    if (g_log != nullptr)
    {
        std::fclose(g_log);
        g_log = nullptr;
    }
    return 0;
}
