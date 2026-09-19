// ============================================================================
// MGPU NR worker - cross-adapter transport implementation. See nr_transport.h
// for why the shapes below are what they are.
//
// STRUCTURE
//     Side        one adapter: device, queue, allocator, list, the SHARED fence
//                 object opened on that device, a timestamp query heap and its
//                 readback buffer.
//     SharedSurface  one cross-adapter resource, created on the game device and
//                 opened on the worker device through a NAMED shared handle.
//                 Named, not anonymous, because the process the worker will run
//                 in is not this one - the benchmark uses the same by-name open
//                 so that the code path it measures is the code path the
//                 architecture needs.
//     Slot        one (frame slot, input slot) pair: the game's local texture,
//                 the worker's local texture, and their shared surface.
//
// CLOCKS
//     Timestamps are only ever subtracted WITHIN one device. The two middle legs
//     are measured on the RTX 4070's clock, the outer legs and the end-to-end
//     figure on the Ti SUPER's. Mixing them would be arithmetic across two
//     unsynchronised counters, which is how a latency report ends up wrong in a
//     way nobody can see.
// ============================================================================

#include "nr_transport.h"

#include "nr_slots.h"

#include <windows.h>
#include <bcrypt.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace nr
{
    const char *shared_kind_name(SharedKind k)
    {
        switch (k)
        {
        case SharedKind::NOT_CREATED:             return "NONE";
        case SharedKind::CROSS_ADAPTER_BUFFER:    return "CROSS_ADAPTER_BUFFER";
        case SharedKind::CROSS_ADAPTER_TEXTURE2D: return "CROSS_ADAPTER_TEXTURE2D";
        }
        return "?";
    }

    const char *leg_name(unsigned i)
    {
        switch (i)
        {
        case 0: return "tisu_to_shared";
        case 1: return "shared_to_rtx4070_local";
        case 2: return "rtx4070_local_to_shared";
        case 3: return "shared_to_tisu";
        }
        return "?";
    }

    std::string Failure::hr_hex() const
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "0x%08X", (unsigned)(hr & 0xFFFFFFFFll));
        return buf;
    }

    namespace
    {
        const std::uint64_t PLACEMENT_ALIGNMENT = 65536ull;

        std::uint64_t align_up(std::uint64_t v, std::uint64_t a)
        {
            return ((v + a - 1ull) / a) * a;
        }

        unsigned row_pitch_for(unsigned width, unsigned bytes_per_pixel)
        {
            const unsigned raw = width * bytes_per_pixel;
            return (raw + 255u) & ~255u;      // buffer copies need 256-byte rows
        }

        void record(std::vector<Failure> &into, const char *stage, HRESULT hr,
                    const std::string &note)
        {
            Failure f;
            f.stage = stage;
            f.hr = (long long)(int)hr;
            f.note = note;
            into.push_back(f);
        }

        struct Side
        {
            IDXGIAdapter1             *adapter = nullptr;
            ID3D12Device              *dev = nullptr;
            ID3D12CommandQueue        *queue = nullptr;
            ID3D12CommandAllocator    *alloc = nullptr;
            ID3D12GraphicsCommandList *list = nullptr;
            ID3D12Fence               *fence = nullptr;    // the SHARED object
            HANDLE                     fence_event = nullptr;
            ID3D12QueryHeap           *queries = nullptr;
            ID3D12Resource            *query_readback = nullptr;
            // UINT64, because that is what GetTimestampFrequency writes: an
            // `unsigned` here is a type error the compiler catches and a
            // reviewer does not.
            std::uint64_t              timestamp_frequency = 0;
            AdapterReport              report;
            const char                *who = "?";
        };

        struct SharedSurface
        {
            SharedKind      kind = SharedKind::NOT_CREATED;
            ID3D12Heap     *heap_game = nullptr;
            ID3D12Resource *res_game = nullptr;
            ID3D12Resource *res_worker = nullptr;
            HANDLE          handle = nullptr;
            std::uint64_t   heap_bytes = 0;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            std::uint32_t   width = 0, height = 0, row_pitch = 0, dxgi_format = 0;
        };

        struct Slot
        {
            std::string     name;
            ID3D12Resource *local_game = nullptr;
            ID3D12Resource *local_worker = nullptr;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT game_footprint{};
            SharedSurface   shared;
        };

        // ------------------------------------------------------------------ setup
        bool pick_adapter(IDXGIFactory1 *factory, unsigned vendor, unsigned device,
                          IDXGIAdapter1 **out, AdapterReport &report, std::string &err)
        {
            for (UINT i = 0;; ++i)
            {
                IDXGIAdapter1 *candidate = nullptr;
                if (factory->EnumAdapters1(i, &candidate) != S_OK) break;
                if (candidate == nullptr) continue;
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(candidate->GetDesc1(&d)) &&
                    (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                    d.VendorId == vendor && d.DeviceId == device)
                {
                    report.found = true;
                    report.vendor = d.VendorId;
                    report.device = d.DeviceId;
                    report.luid_high = (unsigned)d.AdapterLuid.HighPart;
                    report.luid_low = (unsigned)d.AdapterLuid.LowPart;
                    report.dedicated_video_memory = d.DedicatedVideoMemory;
                    std::wcsncpy(report.name, d.Description, 127);
                    *out = candidate;
                    return true;
                }
                candidate->Release();
            }
            char buf[220];
            std::snprintf(buf, sizeof buf,
                          "no adapter with vendor 0x%04X device 0x%04X. Both adapters are "
                          "required, and neither is ever selected by ordinal.", vendor, device);
            err = buf;
            return false;
        }

        bool open_side(IDXGIFactory1 *factory, Side &s, unsigned vendor, unsigned device,
                       const char *who, std::vector<Failure> &failures, std::string &err)
        {
            s.who = who;
            if (!pick_adapter(factory, vendor, device, &s.adapter, s.report, err)) return false;

            HRESULT hr = D3D12CreateDevice(s.adapter, D3D_FEATURE_LEVEL_11_0,
                                           IID_PPV_ARGS(&s.dev));
            if (FAILED(hr) || s.dev == nullptr)
            {
                record(failures, "D3D12CreateDevice", hr, who);
                err = std::string("D3D12CreateDevice failed for ") + who;
                return false;
            }

            D3D12_FEATURE_DATA_CROSS_NODE cross{};
            if (SUCCEEDED(s.dev->CheckFeatureSupport(D3D12_FEATURE_CROSS_NODE, &cross,
                                                     sizeof cross)))
            {
                s.report.cross_node_tier = (unsigned)cross.SharingTier;
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
            if (SUCCEEDED(s.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts,
                                                     sizeof opts)))
            {
                s.report.row_major_cross_adapter_texture =
                    (opts.CrossAdapterRowMajorTextureSupported != FALSE);
            }

            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = s.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.queue));
            if (FAILED(hr) || s.queue == nullptr)
            {
                record(failures, "CreateCommandQueue", hr, who);
                err = std::string("CreateCommandQueue failed for ") + who;
                return false;
            }
            hr = s.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&s.alloc));
            if (FAILED(hr))
            {
                record(failures, "CreateCommandAllocator", hr, who);
                err = std::string("CreateCommandAllocator failed for ") + who;
                return false;
            }
            hr = s.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr,
                                          IID_PPV_ARGS(&s.list));
            if (FAILED(hr))
            {
                record(failures, "CreateCommandList", hr, who);
                err = std::string("CreateCommandList failed for ") + who;
                return false;
            }
            if (FAILED(s.queue->GetTimestampFrequency(&s.timestamp_frequency)))
            {
                s.timestamp_frequency = 0;
            }
            s.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return s.fence_event != nullptr;
        }

        //: The cross-adapter heap and the placed resource on it.
        //:
        //:   heap properties Flags : SHARED_CROSS_ADAPTER   (the adapter-sharing bit)
        //:   CreateHeap flags      : SHARED | ALLOW_ALL_BUFFERS_AND_TEXTURES
        //:   resource flags        : ALLOW_CROSS_ADAPTER
        //:
        //: and the heap type must NOT be CPU-accessible. Get this wrong and the
        //: failure is E_INVALIDARG, which says nothing about which part was wrong
        //: - hence one record() per call, each naming the call.
        bool create_shared(Side &game, Side &worker, SharedKind kind, std::uint64_t bytes,
                           unsigned width, unsigned height, unsigned dxgi_format,
                           unsigned row_pitch, SharedSurface &out,
                           std::vector<Failure> &failures, std::string &err)
        {
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes = align_up(bytes, PLACEMENT_ALIGNMENT);
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            hd.Properties.CreationNodeMask = 1;
            hd.Properties.VisibleNodeMask = 1;
            hd.Alignment = PLACEMENT_ALIGNMENT;
            hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
                       D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

            HRESULT hr = game.dev->CreateHeap(&hd, IID_PPV_ARGS(&out.heap_game));
            if (FAILED(hr) || out.heap_game == nullptr)
            {
                record(failures, "CreateHeap(SHARED_CROSS_ADAPTER)", hr,
                       std::string(game.who) + ": the cross-adapter heap could not be created");
                err = "CreateHeap with D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER failed";
                return false;
            }

            D3D12_RESOURCE_DESC rd{};
            if (kind == SharedKind::CROSS_ADAPTER_BUFFER)
            {
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = bytes;
                rd.Height = 1;
                rd.Format = DXGI_FORMAT_UNKNOWN;
            }
            else
            {
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                rd.Width = width;
                rd.Height = height;
                rd.Format = (DXGI_FORMAT)dxgi_format;
            }
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

            // COMMON for both forms: the per-frame barriers below then have one
            // starting state to reason about, whatever the resource kind is.
            hr = game.dev->CreatePlacedResource(out.heap_game, 0, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, IID_PPV_ARGS(&out.res_game));
            if (FAILED(hr) || out.res_game == nullptr)
            {
                record(failures, "CreatePlacedResource(ALLOW_CROSS_ADAPTER)", hr,
                       std::string(game.who) + ": " +
                           ((kind == SharedKind::CROSS_ADAPTER_BUFFER)
                                ? "the shared cross-adapter BUFFER was refused"
                                : "the shared cross-adapter TEXTURE2D was refused"));
                err = "CreatePlacedResource with D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER failed";
                return false;
            }

            static unsigned counter = 0;
            wchar_t name[160];
            std::swprintf(name, 160, L"MGPU_NR_SHARED_%lu_%u",
                          (unsigned long)GetCurrentProcessId(), counter++);
            hr = game.dev->CreateSharedHandle(out.res_game, nullptr, GENERIC_ALL, name,
                                              &out.handle);
            if (FAILED(hr) || out.handle == nullptr)
            {
                record(failures, "CreateSharedHandle", hr, std::string(game.who) + ": the shared handle");
                err = "CreateSharedHandle failed";
                return false;
            }

            HANDLE opened = nullptr;
            hr = worker.dev->OpenSharedHandleByName(name, GENERIC_ALL, &opened);
            if (FAILED(hr) || opened == nullptr)
            {
                record(failures, "OpenSharedHandleByName", hr,
                       std::string(worker.who) + ": the shared handle could not be opened");
                err = "OpenSharedHandleByName failed on the worker device";
                return false;
            }
            hr = worker.dev->OpenSharedHandle(opened, IID_PPV_ARGS(&out.res_worker));
            CloseHandle(opened);
            if (FAILED(hr) || out.res_worker == nullptr)
            {
                record(failures, "OpenSharedHandle", hr,
                       std::string(worker.who) + ": the shared resource could not be opened");
                err = "OpenSharedHandle failed on the worker device";
                return false;
            }

            out.kind = kind;
            out.heap_bytes = hd.SizeInBytes;
            out.width = width;
            out.height = height;
            out.row_pitch = row_pitch;
            out.dxgi_format = dxgi_format;
            out.footprint.Offset = 0;
            out.footprint.Footprint.Format = (DXGI_FORMAT)dxgi_format;
            out.footprint.Footprint.Width = width;
            out.footprint.Footprint.Height = height;
            out.footprint.Footprint.Depth = 1;
            out.footprint.Footprint.RowPitch = row_pitch;
            return true;
        }

        bool create_local(Side &s, unsigned width, unsigned height, unsigned dxgi_format,
                          ID3D12Resource **out, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp,
                          std::string &err)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = width;
            rd.Height = height;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = (DXGI_FORMAT)dxgi_format;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = D3D12_RESOURCE_FLAG_NONE;

            const HRESULT hr = s.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(out));
            if (FAILED(hr) || *out == nullptr)
            {
                err = std::string(s.who) + ": the local texture could not be created";
                return false;
            }
            if (fp != nullptr)
            {
                const D3D12_RESOURCE_DESC desc = (*out)->GetDesc();
                UINT rows = 0;
                UINT64 row_size = 0, total = 0;
                s.dev->GetCopyableFootprints(&desc, 0, 1, 0, fp, &rows, &row_size, &total);
            }
            return true;
        }

        void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                     D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            if (before == after) return;
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
            list->ResourceBarrier(1, &b);
        }

        void place_on_shared(D3D12_TEXTURE_COPY_LOCATION &loc, const SharedSurface &sh, bool game)
        {
            loc.pResource = game ? sh.res_game : sh.res_worker;
            if (sh.kind == SharedKind::CROSS_ADAPTER_BUFFER)
            {
                loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                loc.PlacedFootprint = sh.footprint;
            }
            else
            {
                loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                loc.SubresourceIndex = 0;
            }
        }

        void copy_local_to_shared(ID3D12GraphicsCommandList *list, const SharedSurface &sh,
                                  bool game, ID3D12Resource *local)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            D3D12_TEXTURE_COPY_LOCATION src{};
            place_on_shared(dst, sh, game);
            src.pResource = local;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        void copy_shared_to_local(ID3D12GraphicsCommandList *list, const SharedSurface &sh,
                                  bool game, ID3D12Resource *local)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            D3D12_TEXTURE_COPY_LOCATION src{};
            dst.pResource = local;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            place_on_shared(src, sh, game);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        void fill_pattern(std::vector<unsigned char> &buf, unsigned seed)
        {
            unsigned x = seed ? seed : 0x1234567u;
            for (std::size_t i = 0; i < buf.size(); ++i)
            {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                buf[i] = (unsigned char)(x & 0xFFu);
            }
        }

        std::string sha256_memory(const void *data, std::size_t n)
        {
            std::string out;
            BCRYPT_ALG_HANDLE alg = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            DWORD obj = 0, got = 0;
            std::vector<unsigned char> objbuf;

            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                            nullptr, 0)))
            {
                return out;
            }
            if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj,
                                                  sizeof obj, &got, 0)))
            {
                BCryptCloseAlgorithmProvider(alg, 0);
                return out;
            }
            objbuf.resize(obj);
            if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, objbuf.data(), obj, nullptr, 0, 0)))
            {
                BCryptCloseAlgorithmProvider(alg, 0);
                return out;
            }
            BCryptHashData(hash, (PUCHAR)data, (ULONG)n, 0);
            unsigned char digest[32] = {};
            BCryptFinishHash(hash, digest, sizeof digest, 0);
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);

            char hex[65];
            for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
            hex[64] = '\0';
            out = hex;
            return out;
        }

        //: Submit whatever is recorded, signal the shared fence, and either wait
        //: on the GPU queue of the other side or on the CPU, depending on what
        //: the driver accepted for this fence.
        bool submit_and_signal(Side &s, ID3D12Fence *shared, std::uint64_t value,
                               std::vector<Failure> &failures)
        {
            const HRESULT chr = s.list->Close();
            if (FAILED(chr))
            {
                record(failures, "CommandList::Close", chr, s.who);
                return false;
            }
            ID3D12CommandList *const lists[1] = { s.list };
            s.queue->ExecuteCommandLists(1, lists);
            const HRESULT shr = s.queue->Signal(shared, value);
            if (FAILED(shr))
            {
                record(failures, "CommandQueue::Signal(shared)", shr, s.who);
                return false;
            }
            const HRESULT rhr = s.alloc->Reset();
            if (FAILED(rhr))
            {
                record(failures, "CommandAllocator::Reset", rhr, s.who);
                return false;
            }
            const HRESULT lhr = s.list->Reset(s.alloc, nullptr);
            if (FAILED(lhr))
            {
                record(failures, "CommandList::Reset", lhr, s.who);
                return false;
            }
            return true;
        }
    }

    struct Transport::Impl
    {
        Side          game;
        Side          worker;
        IDXGIFactory1 *factory = nullptr;
        SharedKind    kind = SharedKind::NOT_CREATED;
        unsigned      slot_count = 3;
        unsigned      width = 0, height = 0, dxgi_format = 0;
        unsigned      bytes_per_pixel = 4;
        std::uint64_t row_pitch = 0, slice_pitch = 0, payload = 0, shared_bytes = 0;
        std::uint64_t opened_frames = 0;

        std::vector<std::vector<Slot> > slots;      // [frame slot][input slot]
        std::vector<std::uint64_t> game_ready;      // signalled by the game, per frame
        std::vector<std::uint64_t> worker_done;     // signalled by the worker, per frame

        bool          gpu_waits = true;
        std::uint32_t gpu_waits_used = 0;
        std::uint32_t cpu_waits_used = 0;
        std::uint32_t queries_per_frame = 4;

        std::vector<Failure> failures;
    };

    Transport::Transport() : impl_(new Impl()) {}

    Transport::~Transport()
    {
        close();
        delete impl_;
        impl_ = nullptr;
    }

    const std::vector<Failure> &Transport::failures() const
    {
        static const std::vector<Failure> none;
        return (impl_ != nullptr) ? impl_->failures : none;
    }

    void Transport::adapter_reports(AdapterReport &game, AdapterReport &worker) const
    {
        if (impl_ == nullptr) return;
        game = impl_->game.report;
        worker = impl_->worker.report;
    }

    bool Transport::enumerate_adapters(std::vector<AdapterReport> &out, std::string &err)
    {
        IDXGIFactory1 *factory = nullptr;
        const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr) || factory == nullptr)
        {
            err = "CreateDXGIFactory1 failed";
            return false;
        }
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1 *candidate = nullptr;
            if (factory->EnumAdapters1(i, &candidate) != S_OK) break;
            if (candidate == nullptr) continue;
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(candidate->GetDesc1(&d)))
            {
                AdapterReport a;
                a.found = true;
                a.vendor = d.VendorId;
                a.device = d.DeviceId;
                a.luid_high = (unsigned)d.AdapterLuid.HighPart;
                a.luid_low = (unsigned)d.AdapterLuid.LowPart;
                a.dedicated_video_memory = d.DedicatedVideoMemory;
                std::wcsncpy(a.name, d.Description, 127);
                out.push_back(a);
            }
            candidate->Release();
        }
        factory->Release();
        return true;
    }

    bool Transport::open(const TransportOptions &opt, std::string &err)
    {
        Impl *im = impl_;
        im->width = opt.width;
        im->height = opt.height;
        im->dxgi_format = opt.dxgi_format;
        im->slot_count = opt.slot_count ? opt.slot_count : 3u;
        im->row_pitch = row_pitch_for(opt.width, im->bytes_per_pixel);
        im->slice_pitch = im->row_pitch * opt.height;
        im->payload = (std::uint64_t)opt.width * opt.height * im->bytes_per_pixel;
        im->opened_frames = opt.frames;

        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&im->factory));
        if (FAILED(hr) || im->factory == nullptr)
        {
            err = "CreateDXGIFactory1 failed";
            return false;
        }
        if (!open_side(im->factory, im->game, opt.game_vendor, opt.game_device,
                       "game/Ti SUPER", im->failures, err))
        {
            return false;
        }
        if (!open_side(im->factory, im->worker, opt.worker_vendor, opt.worker_device,
                       "worker/RTX 4070", im->failures, err))
        {
            return false;
        }

        // ---- the ONE cross-adapter fence, opened by both devices -------------
        hr = im->game.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED |
                                          D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                       IID_PPV_ARGS(&im->game.fence));
        if (FAILED(hr) || im->game.fence == nullptr)
        {
            record(im->failures, "CreateFence(SHARED|SHARED_CROSS_ADAPTER)", hr,
                   "the cross-adapter fence could not be created");
            err = "CreateFence with D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER failed";
            return false;
        }
        wchar_t fname[160];
        std::swprintf(fname, 160, L"MGPU_NR_FENCE_%lu", (unsigned long)GetCurrentProcessId());
        HANDLE fh = nullptr;
        hr = im->game.dev->CreateSharedHandle(im->game.fence, nullptr, GENERIC_ALL, fname, &fh);
        if (FAILED(hr) || fh == nullptr)
        {
            record(im->failures, "CreateSharedHandle(fence)", hr, "the fence handle");
            err = "CreateSharedHandle for the fence failed";
            return false;
        }
        HANDLE fopened = nullptr;
        hr = im->worker.dev->OpenSharedHandleByName(fname, GENERIC_ALL, &fopened);
        if (FAILED(hr) || fopened == nullptr)
        {
            CloseHandle(fh);
            record(im->failures, "OpenSharedHandleByName(fence)", hr,
                   "the worker device could not open the cross-adapter fence");
            err = "OpenSharedHandleByName for the fence failed";
            return false;
        }
        hr = im->worker.dev->OpenSharedHandle(fopened, IID_PPV_ARGS(&im->worker.fence));
        CloseHandle(fopened);
        CloseHandle(fh);
        if (FAILED(hr) || im->worker.fence == nullptr)
        {
            record(im->failures, "OpenSharedHandle(fence)", hr,
                   "the worker device could not open the fence object");
            err = "OpenSharedHandle for the fence failed";
            return false;
        }

        // ---- which shared form works: a probe, one HRESULT per refusal -------
        {
            SharedSurface probe;
            std::string why;
            const bool buffer_ok = create_shared(im->game, im->worker,
                                                 SharedKind::CROSS_ADAPTER_BUFFER,
                                                 im->slice_pitch, opt.width, opt.height,
                                                 opt.dxgi_format, (unsigned)im->row_pitch,
                                                 probe, im->failures, why);
            if (buffer_ok)
            {
                im->kind = SharedKind::CROSS_ADAPTER_BUFFER;
            }
            else
            {
                // NOTE: the failed buffer probe is NOT released here. It is
                // released once, below, on both paths - releasing it in this
                // branch as well would call Release() twice on the same object.
                SharedSurface probe2;
                if (!create_shared(im->game, im->worker,
                                   SharedKind::CROSS_ADAPTER_TEXTURE2D, im->slice_pitch,
                                   opt.width, opt.height, opt.dxgi_format,
                                   (unsigned)im->row_pitch, probe2, im->failures, why))
                {
                    if (probe2.res_worker) probe2.res_worker->Release();
                    if (probe2.res_game) probe2.res_game->Release();
                    if (probe2.heap_game) probe2.heap_game->Release();
                    if (probe2.handle) CloseHandle(probe2.handle);
                    if (probe.res_worker) probe.res_worker->Release();
                    if (probe.res_game) probe.res_game->Release();
                    if (probe.heap_game) probe.heap_game->Release();
                    if (probe.handle) CloseHandle(probe.handle);
                    err = "neither a cross-adapter buffer nor a cross-adapter texture could be "
                          "created; every HRESULT is recorded in the report";
                    return false;
                }
                im->kind = SharedKind::CROSS_ADAPTER_TEXTURE2D;
            }
            // The probe surface is released; the real ones are created below so
            // that there is exactly one creation path for the report to describe.
            if (probe.res_worker) probe.res_worker->Release();
            if (probe.res_game) probe.res_game->Release();
            if (probe.heap_game) probe.heap_game->Release();
            if (probe.handle) CloseHandle(probe.handle);
        }

        // ---- every (frame slot, input slot) surface + both local textures ----
        im->slots.resize(im->slot_count);
        for (unsigned s = 0; s < im->slot_count; ++s)
        {
            im->slots[s].resize(INPUT_SLOT_COUNT);
            for (unsigned in = 0; in < INPUT_SLOT_COUNT; ++in)
            {
                Slot &slot = im->slots[s][in];
                slot.name = input_slot_metadata_name((InputSlot)in);
                std::string why;
                if (!create_shared(im->game, im->worker, im->kind, im->slice_pitch,
                                   opt.width, opt.height, opt.dxgi_format,
                                   (unsigned)im->row_pitch, slot.shared, im->failures, why))
                {
                    err = why;
                    return false;
                }
                im->shared_bytes += slot.shared.heap_bytes;
                if (!create_local(im->game, opt.width, opt.height, opt.dxgi_format,
                                  &slot.local_game, &slot.game_footprint, why))
                {
                    err = why;
                    return false;
                }
                if (!create_local(im->worker, opt.width, opt.height, opt.dxgi_format,
                                  &slot.local_worker, nullptr, why))
                {
                    err = why;
                    return false;
                }
            }
        }

        // ---- timestamp queries: four leg boundaries per frame per device ----
        im->queries_per_frame = 4;
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = opt.frames * im->queries_per_frame + 4;
        qh.NodeMask = 0;

        Side *sides[2] = { &im->game, &im->worker };
        for (int i = 0; i < 2; ++i)
        {
            Side &s = *sides[i];
            if (FAILED(s.dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&s.queries))))
            {
                s.queries = nullptr;
                record(im->failures, "CreateQueryHeap(TIMESTAMP)", E_FAIL,
                       std::string(s.who) + ": GPU timestamps are unavailable; the report says so "
                       "rather than reporting a zero");
                continue;
            }
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (UINT64)qh.Count * sizeof(UINT64);
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(s.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&s.query_readback))))
            {
                s.query_readback = nullptr;
            }
        }
        return true;
    }

    void Transport::close()
    {
        Impl *im = impl_;
        if (im == nullptr) return;
        for (std::size_t s = 0; s < im->slots.size(); ++s)
        {
            for (std::size_t i = 0; i < im->slots[s].size(); ++i)
            {
                Slot &slot = im->slots[s][i];
                if (slot.local_worker) { slot.local_worker->Release(); slot.local_worker = nullptr; }
                if (slot.local_game) { slot.local_game->Release(); slot.local_game = nullptr; }
                if (slot.shared.res_worker) { slot.shared.res_worker->Release(); slot.shared.res_worker = nullptr; }
                if (slot.shared.res_game) { slot.shared.res_game->Release(); slot.shared.res_game = nullptr; }
                if (slot.shared.heap_game) { slot.shared.heap_game->Release(); slot.shared.heap_game = nullptr; }
                if (slot.shared.handle) { CloseHandle(slot.shared.handle); slot.shared.handle = nullptr; }
            }
            im->slots[s].clear();
        }
        im->slots.clear();

        Side *sides[2] = { &im->game, &im->worker };
        for (int i = 0; i < 2; ++i)
        {
            Side &s = *sides[i];
            if (s.query_readback) { s.query_readback->Release(); s.query_readback = nullptr; }
            if (s.queries) { s.queries->Release(); s.queries = nullptr; }
            if (s.fence_event) { CloseHandle(s.fence_event); s.fence_event = nullptr; }
            if (s.fence) { s.fence->Release(); s.fence = nullptr; }
            if (s.list) { s.list->Release(); s.list = nullptr; }
            if (s.alloc) { s.alloc->Release(); s.alloc = nullptr; }
            if (s.queue) { s.queue->Release(); s.queue = nullptr; }
            if (s.dev) { s.dev->Release(); s.dev = nullptr; }
            if (s.adapter) { s.adapter->Release(); s.adapter = nullptr; }
        }
        if (im->factory) { im->factory->Release(); im->factory = nullptr; }
    }

    // ------------------------------------------------------------------- run
    bool Transport::run(const TransportOptions &opt, TransportResult &res)
    {
        Impl *im = impl_;
        if (im->slots.empty())
        {
            res.note = "the transport was not opened";
            return false;
        }

        res.game = im->game.report;
        res.worker = im->worker.report;
        res.kind = im->kind;
        res.payload_bytes = im->payload;
        res.shared_bytes_per_slot = im->shared_bytes;
        res.unsupported = im->failures;
        res.frames_requested = (std::uint32_t)opt.frames;

        const bool have_ts = im->game.queries != nullptr && im->worker.queries != nullptr &&
                             im->game.query_readback != nullptr &&
                             im->worker.query_readback != nullptr &&
                             im->game.timestamp_frequency != 0 &&
                             im->worker.timestamp_frequency != 0;
        if (!have_ts)
        {
            res.note = "GPU timestamps are unavailable on at least one device, so the stage "
                       "statistics are empty. The CPU wall time is reported instead, and it is "
                       "NOT a substitute for them.";
        }

        unsigned frames = opt.frames;
        if (frames > im->opened_frames) frames = (unsigned)im->opened_frames;

        // Resource state per surface, per device. Tracking it is not bookkeeping
        // for its own sake: a transition whose "before" is wrong is an invalid
        // barrier, and the driver's answer to that is a device removal.
        const std::size_t n_surfaces = (std::size_t)im->slot_count * INPUT_SLOT_COUNT;
        std::vector<D3D12_RESOURCE_STATES> st_shared_game(n_surfaces, D3D12_RESOURCE_STATE_COMMON);
        std::vector<D3D12_RESOURCE_STATES> st_shared_worker(n_surfaces, D3D12_RESOURCE_STATE_COMMON);
        std::vector<D3D12_RESOURCE_STATES> st_local_game(n_surfaces, D3D12_RESOURCE_STATE_COMMON);
        std::vector<D3D12_RESOURCE_STATES> st_local_worker(n_surfaces, D3D12_RESOURCE_STATE_COMMON);

        auto index_of = [&](unsigned slot, unsigned in) {
            return (std::size_t)slot * INPUT_SLOT_COUNT + in;
        };
        auto go = [&](ID3D12GraphicsCommandList *list, ID3D12Resource *r,
                      D3D12_RESOURCE_STATES &tracked, D3D12_RESOURCE_STATES after) {
            barrier(list, r, tracked, after);
            tracked = after;
        };

        // ---- the seed: a deterministic pattern in the game's COLOR texture ---
        // This CPU write is the INPUT to the test and is deliberately outside
        // every measured leg. Nothing else touches pixels on the CPU.
        std::vector<unsigned char> pattern((std::size_t)im->payload);
        fill_pattern(pattern, 0xC0FFEEu);
        res.seed_sha256 = sha256_memory(pattern.data(), pattern.size());

        const unsigned color_in = (unsigned)InputSlot::COLOR;
        const unsigned out_slot = (unsigned)InputSlot::OUTPUT;
        {
            // An UPLOAD buffer holding the pattern with the aligned row pitch,
            // then one CopyTextureRegion into the local COLOR texture.
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = im->slice_pitch;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ID3D12Resource *upload = nullptr;
            HRESULT hr = im->game.dev->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload));
            if (FAILED(hr) || upload == nullptr)
            {
                record(im->failures, "seed/CreateCommittedResource(UPLOAD)", hr,
                       "the seed pattern could not be uploaded");
                res.note = "the seed upload failed, so the round trip has no known input to check";
                return false;
            }
            void *mapped = nullptr;
            const D3D12_RANGE read_range{ 0, 0 };
            hr = upload->Map(0, &read_range, &mapped);
            if (SUCCEEDED(hr) && mapped != nullptr)
            {
                const unsigned src_row = opt.width * im->bytes_per_pixel;
                unsigned char *dst = (unsigned char *)mapped;
                for (unsigned y = 0; y < opt.height; ++y)
                {
                    std::memcpy(dst + (std::size_t)y * im->row_pitch,
                                pattern.data() + (std::size_t)y * src_row, src_row);
                }
                upload->Unmap(0, nullptr);
            }

            Slot &col = im->slots[0][color_in];
            go(im->game.list, col.local_game, st_local_game[index_of(0, color_in)],
               D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            D3D12_TEXTURE_COPY_LOCATION src{};
            dst.pResource = col.local_game;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = col.game_footprint;
            im->game.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            go(im->game.list, col.local_game, st_local_game[index_of(0, color_in)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);

            if (!submit_and_signal(im->game, im->game.fence, 1ull, im->failures))
            {
                upload->Release();
                res.note = "the seed copy could not be submitted";
                return false;
            }
            im->game.fence->SetEventOnCompletion(1ull, im->game.fence_event);
            WaitForSingleObject(im->game.fence_event, 30000);
            upload->Release();
        }

        // ---- the frame loop -------------------------------------------------
        // NO CPU WAIT PER FRAME. Each queue waits, on the GPU, for the value the
        // other device signalled for the slot it is about to touch; with three
        // slots the first three frames need no wait at all and every later frame
        // waits on a value that three frames of work have already satisfied.
        const std::uint64_t game_base = 1000ull;
        const std::uint64_t worker_base = 100000ull;
        std::vector<std::uint64_t> game_ready(frames, 0ull);
        std::vector<std::uint64_t> worker_done(frames, 0ull);
        for (unsigned f = 0; f < frames; ++f)
        {
            game_ready[f] = game_base + f + 1ull;
            worker_done[f] = worker_base + f + 1ull;
        }

        auto queue_wait = [&](Side &s, std::uint64_t value, TransportResult &r) -> bool {
            if (im->gpu_waits && s.queue != nullptr)
            {
                const HRESULT hr = s.queue->Wait(s.fence, value);
                if (SUCCEEDED(hr))
                {
                    ++im->gpu_waits_used;
                    ++r.slot_waits_gpu;
                    return true;
                }
                if (im->gpu_waits)
                {
                    // Recorded ONCE, with the HRESULT, and then the benchmark
                    // says which mode it actually used.
                    record(im->failures, "CommandQueue::Wait(cross-adapter fence)", hr,
                           "the driver refused a GPU-side wait on the shared cross-adapter "
                           "fence; every wait from here is a CPU wait on the same value");
                    im->gpu_waits = false;
                }
            }
            ++im->cpu_waits_used;
            ++r.slot_waits_cpu;
            s.fence->SetEventOnCompletion(value, s.fence_event);
            return WaitForSingleObject(s.fence_event, 30000) == WAIT_OBJECT_0;
        };

        const std::uint64_t t_start = GetTickCount64();
        for (unsigned f = 0; f < frames; ++f)
        {
            const unsigned slot = f % im->slot_count;
            Slot &in = im->slots[slot][color_in];
            Slot &out = im->slots[slot][out_slot];
            const unsigned q = f * im->queries_per_frame;

            // The wait is on the SLOT, never on the frame: with three slots the
            // first three frames need no wait at all, and every later frame waits
            // for a value three frames of work have already satisfied.
            if (f >= im->slot_count)
            {
                if (!queue_wait(im->game, worker_done[f - im->slot_count], res))
                {
                    res.note = "a slot wait did not complete";
                    return false;
                }
            }

            // GAME: local COLOR -> shared; shared OUTPUT -> local OUTPUT.
            go(im->game.list, in.shared.res_game, st_shared_game[index_of(slot, color_in)],
               D3D12_RESOURCE_STATE_COPY_DEST);
            if (have_ts) im->game.list->EndQuery(im->game.queries, D3D12_QUERY_TYPE_TIMESTAMP, q + 0);
            copy_local_to_shared(im->game.list, in.shared, true, in.local_game);
            go(im->game.list, in.shared.res_game, st_shared_game[index_of(slot, color_in)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (have_ts) im->game.list->EndQuery(im->game.queries, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);

            go(im->game.list, out.shared.res_game, st_shared_game[index_of(slot, out_slot)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (have_ts) im->game.list->EndQuery(im->game.queries, D3D12_QUERY_TYPE_TIMESTAMP, q + 2);
            go(im->game.list, out.local_game, st_local_game[index_of(slot, out_slot)],
               D3D12_RESOURCE_STATE_COPY_DEST);
            copy_shared_to_local(im->game.list, out.shared, true, out.local_game);
            if (have_ts) im->game.list->EndQuery(im->game.queries, D3D12_QUERY_TYPE_TIMESTAMP, q + 3);

            if (!submit_and_signal(im->game, im->game.fence, game_ready[f], im->failures))
            {
                res.note = "the game command list could not be submitted";
                return false;
            }

            // WORKER: waits for this frame's ingress value, then shared -> local
            // COLOR, then local COLOR -> shared OUTPUT. It writes its result into
            // the shared surface in the SAME slot, which is why the game may not
            // overwrite that slot until the worker's value is reached.
            // The worker waits for THIS frame's ingress value before it touches
            // the slot, which is the other half of the same rule.
            if (!queue_wait(im->worker, game_ready[f], res))
            {
                res.note = "the worker's ingress wait did not complete";
                return false;
            }
            go(im->worker.list, in.shared.res_worker, st_shared_worker[index_of(slot, color_in)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (have_ts) im->worker.list->EndQuery(im->worker.queries, D3D12_QUERY_TYPE_TIMESTAMP, q + 0);
            go(im->worker.list, in.local_worker, st_local_worker[index_of(slot, color_in)],
               D3D12_RESOURCE_STATE_COPY_DEST);
            copy_shared_to_local(im->worker.list, in.shared, false, in.local_worker);
            go(im->worker.list, in.local_worker, st_local_worker[index_of(slot, color_in)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (have_ts) im->worker.list->EndQuery(im->worker.queries, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);

            go(im->worker.list, out.shared.res_worker, st_shared_worker[index_of(slot, out_slot)],
               D3D12_RESOURCE_STATE_COPY_DEST);
            if (have_ts) im->worker.list->EndQuery(im->worker.queries, D3D12_QUERY_TYPE_TIMESTAMP, q + 2);
            copy_local_to_shared(im->worker.list, out.shared, false, in.local_worker);
            go(im->worker.list, out.shared.res_worker, st_shared_worker[index_of(slot, out_slot)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (have_ts) im->worker.list->EndQuery(im->worker.queries, D3D12_QUERY_TYPE_TIMESTAMP, q + 3);

            if (!submit_and_signal(im->worker, im->worker.fence, worker_done[f], im->failures))
            {
                res.note = "the worker command list could not be submitted";
                return false;
            }

            // The game's copy back for THIS frame is only safe once the worker's
            // value for this frame is reached. That is a real dependency and it is
            // not hidden: it is the wait recorded above for the slot reuse.
            ++res.frames_completed;
        }

        // One CPU wait for the whole run, and one for the last frame's egress so
        // the readback below reads completed work.
        im->worker.fence->SetEventOnCompletion(worker_done[frames - 1], im->worker.fence_event);
        WaitForSingleObject(im->worker.fence_event, 120000);
        const std::uint64_t t_end = GetTickCount64();
        res.fence_mode = im->gpu_waits ? "gpu-queue" : "cpu";
        res.slot_waits_gpu = im->gpu_waits_used;
        res.slot_waits_cpu = im->cpu_waits_used;

        if (frames == 0)
        {
            res.note = "no frames were requested";
            res.unsupported = im->failures;
            return true;
        }

        // ---- timestamps: resolved once, at the end --------------------------
        std::vector<std::uint64_t> game_ts, worker_ts;
        if (have_ts)
        {
            Side *sides[2] = { &im->game, &im->worker };
            std::vector<std::uint64_t> *outs[2] = { &game_ts, &worker_ts };
            for (int i = 0; i < 2; ++i)
            {
                Side &s = *sides[i];
                s.list->ResolveQueryData(s.queries, D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                         frames * im->queries_per_frame, s.query_readback, 0);
                const std::uint64_t v = (i == 0 ? 900000ull : 990000ull);
                if (!submit_and_signal(s, s.fence, v, im->failures))
                {
                    continue;
                }
                s.fence->SetEventOnCompletion(v, s.fence_event);
                WaitForSingleObject(s.fence_event, 30000);
                void *mapped = nullptr;
                const D3D12_RANGE rr{ 0, (SIZE_T)(frames * im->queries_per_frame * sizeof(std::uint64_t)) };
                if (SUCCEEDED(s.query_readback->Map(0, &rr, &mapped)) && mapped != nullptr)
                {
                    const std::uint64_t *src = (const std::uint64_t *)mapped;
                    outs[i]->assign(src, src + frames * im->queries_per_frame);
                    s.query_readback->Unmap(0, nullptr);
                }
            }
        }

        if (have_ts && game_ts.size() >= (std::size_t)frames * 4 &&
            worker_ts.size() >= (std::size_t)frames * 4)
        {
            const double gf = (double)im->game.timestamp_frequency;
            const double wf = (double)im->worker.timestamp_frequency;
            std::vector<double> leg[4];
            std::vector<double> totals;
            for (unsigned f = 0; f < frames; ++f)
            {
                const std::size_t q = (std::size_t)f * 4;
                const double l0 = ((double)game_ts[q + 1] - (double)game_ts[q + 0]) / gf * 1000.0;
                const double l3 = ((double)game_ts[q + 3] - (double)game_ts[q + 2]) / gf * 1000.0;
                const double tot = ((double)game_ts[q + 3] - (double)game_ts[q + 0]) / gf * 1000.0;
                const double l1 = ((double)worker_ts[q + 1] - (double)worker_ts[q + 0]) / wf * 1000.0;
                const double l2 = ((double)worker_ts[q + 3] - (double)worker_ts[q + 2]) / wf * 1000.0;
                // A query that was never written reads as 0 and would produce a
                // negative delta. Those are skipped rather than reported as a
                // measurement, and the count of what was kept is in the report.
                if (l0 >= 0.0) leg[0].push_back(l0);
                if (l1 >= 0.0) leg[1].push_back(l1);
                if (l2 >= 0.0) leg[2].push_back(l2);
                if (l3 >= 0.0) leg[3].push_back(l3);
                if (tot >= 0.0) totals.push_back(tot);
            }
            for (int i = 0; i < 4; ++i) res.leg[i] = summarise(leg[i]);
            res.total = summarise(totals);
        }

        // ---- verification: read the game's OUTPUT texture back and hash it ---
        if (opt.verify)
        {
            const unsigned last_slot = (frames - 1) % im->slot_count;
            Slot &out = im->slots[last_slot][out_slot];

            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = out.game_footprint.Footprint.RowPitch * opt.height;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ID3D12Resource *rb = nullptr;
            HRESULT hr = im->game.dev->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&rb));
            if (SUCCEEDED(hr) && rb != nullptr)
            {
                go(im->game.list, out.local_game, st_local_game[index_of(last_slot, out_slot)],
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{};
                D3D12_TEXTURE_COPY_LOCATION src{};
                dst.pResource = rb;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = out.game_footprint;
                src.pResource = out.local_game;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                im->game.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

                const std::uint64_t v = 950000ull;
                if (submit_and_signal(im->game, im->game.fence, v, im->failures))
                {
                    im->game.fence->SetEventOnCompletion(v, im->game.fence_event);
                    WaitForSingleObject(im->game.fence_event, 30000);
                    void *mapped = nullptr;
                    const D3D12_RANGE rr{ 0, (SIZE_T)bd.Width };
                    if (SUCCEEDED(rb->Map(0, &rr, &mapped)) && mapped != nullptr)
                    {
                        const unsigned char *srcp = (const unsigned char *)mapped;
                        const unsigned src_row = opt.width * im->bytes_per_pixel;
                        std::vector<unsigned char> got((std::size_t)im->payload);
                        for (unsigned y = 0; y < opt.height; ++y)
                        {
                            std::memcpy(got.data() + (std::size_t)y * src_row,
                                        srcp + (std::size_t)y * out.game_footprint.Footprint.RowPitch,
                                        src_row);
                        }
                        rb->Unmap(0, nullptr);
                        res.returned_sha256 = sha256_memory(got.data(), got.size());
                        res.exact_match = (res.returned_sha256 == res.seed_sha256);
                        res.verified_frames = 1;
                    }
                }
                rb->Release();
            }
            else
            {
                record(im->failures, "verify/CreateCommittedResource(READBACK)", hr,
                       "the verification readback could not be created");
            }
        }

        // ---- the other two inputs, once each, untimed ------------------------
        // DEPTH and MOTION_VECTORS use the same surfaces and the same copies, so
        // one untimed pass through each is what says they are usable; the check
        // is the device-removed reason afterwards, which is the only way a bad
        // copy reports itself.
        for (unsigned in = 0; in < INPUT_SLOT_COUNT; ++in)
        {
            if (in == color_in || in == out_slot) continue;
            Slot &slot = im->slots[0][in];
            go(im->game.list, slot.shared.res_game, st_shared_game[index_of(0, in)],
               D3D12_RESOURCE_STATE_COPY_DEST);
            go(im->game.list, slot.local_game, st_local_game[index_of(0, in)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);
            copy_local_to_shared(im->game.list, slot.shared, true, slot.local_game);
            go(im->game.list, slot.shared.res_game, st_shared_game[index_of(0, in)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);
            const std::uint64_t gv = 960000ull + in;
            if (!submit_and_signal(im->game, im->game.fence, gv, im->failures))
            {
                res.untimed_slot_checks.push_back(std::string(slot.name) + ": submit failed");
                continue;
            }
            go(im->worker.list, slot.shared.res_worker, st_shared_worker[index_of(0, in)],
               D3D12_RESOURCE_STATE_COPY_SOURCE);
            go(im->worker.list, slot.local_worker, st_local_worker[index_of(0, in)],
               D3D12_RESOURCE_STATE_COPY_DEST);
            copy_shared_to_local(im->worker.list, slot.shared, false, slot.local_worker);
            const std::uint64_t wv = 996000ull + in;
            if (!submit_and_signal(im->worker, im->worker.fence, wv, im->failures))
            {
                res.untimed_slot_checks.push_back(std::string(slot.name) + ": submit failed");
                continue;
            }
            im->game.fence->SetEventOnCompletion(gv, im->game.fence_event);
            WaitForSingleObject(im->game.fence_event, 30000);
            im->worker.fence->SetEventOnCompletion(wv, im->worker.fence_event);
            WaitForSingleObject(im->worker.fence_event, 30000);

            const HRESULT grem = im->game.dev->GetDeviceRemovedReason();
            const HRESULT wrem = im->worker.dev->GetDeviceRemovedReason();
            char buf[240];
            std::snprintf(buf, sizeof buf,
                          "%s: untimed ingress on both devices, game device reason=0x%08X "
                          "worker device reason=0x%08X",
                          slot.name.c_str(), (unsigned)grem, (unsigned)wrem);
            res.untimed_slot_checks.push_back(buf);
            if (FAILED(grem) || FAILED(wrem))
            {
                record(im->failures, "device removed during an untimed slot check",
                       FAILED(grem) ? grem : wrem, slot.name);
            }
        }

        {
            char buf[220];
            std::snprintf(buf, sizeof buf,
                          "%u frame(s) through %u slot(s); shared %s; %.1f MiB of cross-adapter "
                          "heap; %.1f ms of CPU wall time for the whole resolution",
                          frames, im->slot_count, shared_kind_name(im->kind),
                          (double)im->shared_bytes / (1024.0 * 1024.0),
                          (double)(t_end - t_start));
            res.note = res.note.empty() ? buf : (res.note + " | " + buf);
        }

        res.supported = true;
        res.unsupported = im->failures;
        return true;
    }
}
