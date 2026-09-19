// ============================================================================
// MGPU NR worker - GPU-ONLY CROSS-ADAPTER TRANSPORT (v1: shared BUFFERS).
//
// THE PATH THIS IMPLEMENTS, AND THE PATH IT REFUSES TO IMPLEMENT
//
//     Ti SUPER (game)                 RTX 4070 (worker)
//     local texture                   local texture
//         |  CopyTextureRegion            ^  CopyTextureRegion
//         v                               |
//     shared cross-adapter buffer --> shared cross-adapter buffer
//
//     and back for the OUTPUT slot.
//
//     No CPU pixel copies. No staging through system memory by the CPU, no
//     Desktop Duplication, no WGC, no screen capture. The only CPU touches are
//     the SEED (writing the deterministic pattern once, so the test has known
//     input) and the VERIFY readback (hashing what came back). Both are labelled
//     as such in the report and neither is inside a measured leg.
//
// WHY BUFFERS, AND WHY THEY ARE placed RESOURCES ON A CROSS-ADAPTER HEAP
//     Microsoft's "Shared heaps" documentation is explicit, and it decides the
//     implementation:
//
//       * cross-adapter sharing works with heaps created by
//         ID3D12Device::CreateHeap plus CreatePlacedResource;
//       * it is ALSO allowed with CreateCommittedResource, but ONLY for row-major
//         D3D12_RESOURCE_DIMENSION_TEXTURE2D resources;
//       * the heap flags must include D3D12_HEAP_FLAG_SHARED and
//         D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, must not be a CPU-accessible heap
//         type, and must not deny buffers or textures;
//       * only resources carrying D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER may be
//         placed on such a heap;
//       * cross-adapter memory lives in D3D12_MEMORY_POOL_L0 - system memory -
//         which is measured and reported rather than assumed to be VRAM;
//       * the two devices coordinate with CROSS-ADAPTER FENCES, and the usual
//         cross-queue barrier rules still apply.
//
//     So a shared cross-adapter BUFFER is a placed resource on a cross-adapter
//     heap. A committed cross-adapter buffer is not a thing: that form is
//     reserved for row-major TEXTURE2D. TEXTURE2D placed on the same kind of
//     heap is the fallback, and which one was used is recorded in the report.
//
// FENCES: GPU-SIDE WAITS, NOT A CPU WAIT PER FRAME
//     Both devices open the SAME cross-adapter fence object. The game queue waits
//     on the value the worker signalled for a slot, and the worker queue waits on
//     the value the game signalled - on the GPU, in the queue, so a frame does
//     not cost a CPU round trip. If a queue wait on a shared cross-adapter fence
//     is refused by the driver, the exact HRESULT is recorded and the benchmark
//     falls back to a CPU-side wait on the same fence value, with `fence_mode`
//     in the report saying which one was actually used. It never silently
//     claims the cheaper one.
// ============================================================================

#pragma once

#include "nr_json.h"

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;
struct ID3D12Fence;
struct ID3D12Heap;
struct ID3D12Resource;
struct ID3D12QueryHeap;
struct IDXGIAdapter1;

namespace nr
{
    enum class SharedKind
    {
        // NOT `NONE`: this enum travels through files that include <windows.h>,
        // which is a minefield of short upper-case macros. `ERROR` cost a build
        // to learn that lesson; the shorter name is not worth repeating it.
        NOT_CREATED = 0,
        CROSS_ADAPTER_BUFFER = 1,
        CROSS_ADAPTER_TEXTURE2D = 2,
    };

    const char *shared_kind_name(SharedKind k);

    //: The four things that travel. Frame in flight x these four = the surfaces.
    const unsigned INPUT_SLOT_COUNT = 4;

    struct AdapterReport
    {
        bool     found = false;
        unsigned vendor = 0;
        unsigned device = 0;
        unsigned luid_high = 0;
        unsigned luid_low = 0;
        unsigned long long dedicated_video_memory = 0;
        wchar_t  name[128] = L"";
        //: D3D12_FEATURE_CROSS_NODE -> CrossNodeSharingTier, and
        //: D3D12_FEATURE_D3D12_OPTIONS -> CrossAdapterRowMajorTextureSupported.
        //: Reported because they are what a reader needs to judge whether a
        //: failure below is the driver's answer or an implementation mistake.
        unsigned cross_node_tier = 0;
        bool     row_major_cross_adapter_texture = false;
    };

    struct Failure
    {
        std::string stage;
        long long   hr = 0;              // HRESULT as a signed 64-bit value
        std::string note;

        std::string hr_hex() const;
    };

    struct TransportOptions
    {
        unsigned game_vendor = 0x10DEu;
        unsigned game_device = 0x2705u;      // RTX 4070 Ti SUPER
        unsigned worker_vendor = 0x10DEu;
        unsigned worker_device = 0x2786u;    // RTX 4070
        unsigned width = 1920u;
        unsigned height = 1080u;
        unsigned frames = 30u;
        unsigned slot_count = 3u;
        unsigned dxgi_format = 28u;          // DXGI_FORMAT_R8G8B8A8_UNORM
        bool     verify = true;
    };

    struct TransportResult
    {
        bool         supported = false;
        SharedKind   kind = SharedKind::NOT_CREATED;
        std::string  note;

        AdapterReport game;
        AdapterReport worker;

        //: The measured stages. Index order is the data's journey.
        Stats        leg[4];
        Stats        total;

        std::uint32_t frames_requested = 0;
        std::uint32_t frames_completed = 0;

        std::uint64_t payload_bytes = 0;
        std::uint64_t shared_bytes_per_slot = 0;

        std::string  seed_sha256;
        std::string  returned_sha256;
        bool         exact_match = false;
        std::uint32_t verified_frames = 0;

        std::string  fence_mode;             // "gpu-queue" or "cpu"
        std::uint32_t slot_waits_gpu = 0;
        std::uint32_t slot_waits_cpu = 0;

        std::vector<Failure> unsupported;    // every refusal, with its HRESULT
        std::vector<std::string> untimed_slot_checks;
    };

    const char *leg_name(unsigned i);

    class Transport
    {
    public:
        Transport();
        ~Transport();
        Transport(const Transport &) = delete;
        Transport &operator=(const Transport &) = delete;

        //: Adapter enumeration only: no device is created. Used by
        //: --list-adapters, which CI runs on a machine with no NVIDIA adapter to
        //: prove the selection is by PCI identity and that it refuses rather
        //: than picking something else.
        static bool enumerate_adapters(std::vector<AdapterReport> &out, std::string &err);

        //: Devices, queues, fences and the shared surfaces for one resolution.
        bool open(const TransportOptions &opt, std::string &err);

        //: The timed round trips plus the verification readback.
        bool run(const TransportOptions &opt, TransportResult &res);

        void close();

    private:
        struct Impl;
        Impl *impl_;
    };
}
