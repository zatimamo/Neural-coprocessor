NR-WORKER  -  the out-of-process neural lane
============================================

WHY THIS EXISTS
    SPLITPROCESS_ISOLATION settled the architecture question experimentally:

        same process, Ti SUPER dual-active   Descriptor -1   CuModule -1   Reserved18 0xBAD00002
        separate process, Ti SUPER active    Descriptor  0   CuModule  0   Reserved18 Success

    The failure follows the second adapter's ACTIVE D3D12 STATE INSIDE THE PROCESS
    that runs the neural lane. So the RTX 4070 device, the NGX core, the
    architecture patch, the DLSSNR runtime and the Reserved18 session must never
    be created inside Cyberpunk.exe. They are created by mgpu_nr_worker.exe, and
    the game side owns capture and transport only.

WHAT IS IN THIS DIRECTORY
    mgpu_nr_worker.exe      PROCESS A. Runs the proven lane and owns the neural
                            session. --prove reports the eight conditions and
                            exits; --serve runs the lane and then serves the
                            control protocol; --protocol-only serves the protocol
                            WITHOUT touching NVAPI, NGX or D3D12.
    nr_selftest.exe         GPU-free tests: protocol v1, a REAL named-pipe
                            handshake between two endpoints, the frame-slot
                            pipeline and its refusals, the JSON writer and the
                            percentile maths. Runs in CI on every build.
    nr_ipc_probe.exe        the client half of the protocol. CI runs it against
                            the real worker; the game side will use its shape.
    nr_transport_bench.exe  the standalone two-GPU cross-adapter round trip.
                            Loads NO NGX and NO NVAPI: if the transport fails,
                            the neural worker is not implicated.

THE LANE IS THE PROVEN LANE, NOT A SECOND INTERPRETATION
    ../processcontext_ab/nv.cpp and ../processcontext_ab/observe.cpp are compiled
    VERBATIM into mgpu_nr_worker.exe. That is the NVAPI layer, the architecture
    cache/patch, the logger, the file hash and the two private CUDA-interop
    observers. The frozen directory is not modified: the processcontext-ab
    workflow fails if any file in it changes, and this build consumes it.

    The ORDER of the eleven steps and the creation CONTRACT are asserted equal to
    the frozen lane's by CI gates: every params->Set key and value must appear in
    the same order as the frozen file's, the app id must be 0x1000000 at both Init
    points, the version stamp must be the V2 form, and the generic Width/Height
    pair must be absent. Drift is a build failure, not a discovery at run time.

WHY THE WORKER RUNS UNDER A PREFIXED FILE NAME
    The DLSSNR snippet inspects the module name of its CALLER and refuses one that
    does not contain "nvngx.dll" with 0xBAD00002 FAIL_PlatformError - for a reason
    that has nothing to do with the neural work. Production deploys the worker from
    a file name carrying that prefix. RUN-NR-WORKER.cmd makes the copy the same way
    the PROCESSCONTEXT launcher does. Run it under its plain name and the log says
    so, loudly, before anything is created.

THE CONTROL CHANNEL
    \\.\pipe\MGPU_NR_<pid>, protocol version 1, one client, fixed packed structs
    that begin with magic / version / struct size / frame id. Messages: HELLO,
    WORKER_READY, CONFIG, FRAME_SUBMIT, FRAME_COMPLETE, SHUTDOWN, ERROR.

    NO PIXEL DATA CROSSES IT. Frames travel as cross-adapter shared GPU resources
    whose handles are exchanged as metadata; the pipe carries frame ids, geometry,
    formats, pitches and status codes.

    The worker prints WORKER_READY only after Reserved18 was created by the proven
    path. A worker started with --protocol-only sends reserved18 = -1 over the
    pipe, which is a different statement from "the session failed" and cannot be
    mistaken for one.

THE TRANSPORT, AND WHAT IT IS BUILT ON
    Microsoft's shared-heaps documentation decides the shapes, and it is worth
    stating because two of them are counter-intuitive:

      * cross-adapter sharing works with ID3D12Device::CreateHeap plus
        CreatePlacedResource. It is also allowed with CreateCommittedResource -
        but ONLY for row-major TEXTURE2D resources. A committed cross-adapter
        BUFFER is not a thing, which is why v1 places a buffer on a heap.
      * the heap must be D3D12_HEAP_TYPE_DEFAULT, must carry
        D3D12_HEAP_FLAG_SHARED and D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, must not
        deny buffers or textures, and only resources carrying
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER may be placed on it.
      * cross-adapter memory lives in D3D12_MEMORY_POOL_L0, which is SYSTEM
        memory, not VRAM. The benchmark reports the bytes it actually allocated.
      * the two devices coordinate with cross-adapter fences, and the usual
        cross-queue barrier rules still apply between them.

    The benchmark tries a shared BUFFER first (as v1 prefers), falls back to a
    shared TEXTURE2D if the driver refuses it, and records EVERY refusal with the
    exact HRESULT and the call that produced it.

NO CPU WAIT PER FRAME
    Both devices open the SAME cross-adapter fence. The game queue waits on the
    value the worker signalled for a slot, and the worker queue waits on the value
    the game signalled - on the GPU, in the queue. With three frame slots the first
    three frames need no wait at all and every later frame waits on a value that
    three frames of work have already satisfied. If the driver refuses a GPU-side
    wait, the HRESULT is recorded, the benchmark falls back to a CPU wait on the
    same fence value, and `fence_mode` in the report says which one it used.

SLOTS
    INPUT SLOT   what the data IS:  COLOR, DEPTH, MOTION_VECTORS, OUTPUT
    FRAME SLOT   WHEN the data is: slot 0, slot 1, slot 2
    STATE        FREE -> GAME_COPYING -> READY_FOR_WORKER -> WORKER_RUNNING
                      -> READY_FOR_GAME -> GAME_COPYING_BACK -> FREE

    A frame slot may not be reused until BOTH devices have completed it. That is
    not a comment: release() carries the fence value each device must reach and
    refuses while either is short. The self-test asserts the refusal, so the rule
    cannot degrade into a comment that no longer holds.

BUILD
    cmake -S tools/nr_worker -B build-nr -A x64 \
          -DPCAB_NGX_DIR=<ext/ngx> -DPCAB_MINHOOK_DIR=<ext/minhook>
    cmake --build build-nr --config Release
    -> build-nr/bin/{mgpu_nr_worker,nr_selftest,nr_ipc_probe,nr_transport_bench}.exe

RUN, ON THE REAL MACHINE
    RUN-NR-WORKER.cmd "C:\...\neuralscreen-v1.15.0-full\native\nvngx_dlssnr.dll"

    Produces:
        context-nr-worker.log         the Reserved18 proof, with the eight conditions
        nr-selftest.json              the GPU-free test results
        nr-ipc-probe.json             the handshake result and control-channel latency
        transport-benchmark.json      the four legs, four resolutions, hashes, HRESULTs

WHAT IT DOES NOT DO YET
    The worker does not open a shared surface or copy a pixel: FRAME_SUBMIT is
    answered with FRAME_COMPLETE and E_NOTIMPL, which is the honest answer until
    the transport is wired to that process. Nor is anything connected to
    Cyberpunk's frame resources, and the game-side add-on is untouched.

SAFETY CONTRACT (THE PART THAT MUST HOLD WHEN THIS IS WIRED UP)
    Never crash the game because the worker failed. If the worker dies, if the
    pipe disconnects, if any shared-resource operation fails, or if the worker
    reports E_NOTIMPL: log the exact HRESULT, disable neural mode, and let the
    game continue. The worker has a deadline on every pipe operation, so a silent
    peer turns into a logged disconnect rather than a hang. Private NVAPI return
    codes are never patched.
