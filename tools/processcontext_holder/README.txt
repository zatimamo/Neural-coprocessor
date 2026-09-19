PROCESSCONTEXT-HOLDER  -  holder_ti.exe
=======================================

WHAT THIS IS
    PROCESS A of the SPLITPROCESS_ISOLATION experiment. It holds active
    RTX 4070 Ti SUPER D3D12 state in its own process and stays alive, so that a
    SEPARATE process can run the unchanged PROCESSCONTEXT-AB SINGLE arm while
    that state exists elsewhere on the machine.

    It is a separate executable, in a separate directory, with a separate CMake
    project, on purpose: the executable running the RTX 4070 reference lane must
    be the ALREADY-PROVEN one, byte for byte. A holder mode inside
    processcontext_ab.exe would have changed the very binary the comparison is
    about and invalidated the isolation test.

WHAT IT DOES - THE WHOLE LIST
    1. CreateDXGIFactory1
    2. enumerate adapters; find vendor 0x10DE device 0x2705 (RTX 4070 Ti SUPER),
       skipping software adapters - by PCI id, never by ordinal
    3. D3D12CreateDevice(adapter, feature level 11_0)
    4. CreateCommandQueue, type DIRECT
    5. CreateDescriptorHeap, CBV/SRV/UAV, 8 descriptors, shader visible
    6. print exactly:  HOLDER_READY
    7. hold those objects alive
    8. wait for the named stop event the PARENT owns, or an explicit byte on
       stdin, or stdin closing - which is what happens if the parent dies
    9. release everything, print exactly:  HOLDER_STOPPED, exit 0

    On setup failure it prints  HOLDER_FAILED reason=<token>  and exits 1.

WHAT IT DOES NOT DO
    No NGX. No NVAPI, and no call into it. No architecture hook and no detour;
    MinHook is not linked. No CUDA. No NR runtime and no DLSSNR snippet. No
    device on the RTX 4070. No swapchain. No command list, no resource, no
    fence, no submission - the queue is created and never used. It does not
    deploy anything and it never launches the game.

    The CI gate proves the four forbidden identifiers appear neither in this
    directory's sources nor in the built holder_ti.exe, and that they DO appear
    in the diagnostic - so the two binaries cannot have been confused.

USAGE
    holder_ti.exe --event <name> --seconds 0 --log context-holder.log

      --event <name>    a named stop event created by the PARENT before this
                        process starts. It is only OPENED here, so there is
                        exactly one owner and a stale event from an earlier run
                        cannot be inherited.
      --seconds <n>     a wait cap. 0 means no cap, which is what AutoLab passes:
                        the parent is the control, and a holder that gave up on
                        its own would end the experiment while PROCESS B was
                        still running.
      --log <path>      also write the log here; default context-holder.log in
                        the current directory. Every line is flushed, so the log
                        survives the parent stopping this process.

    Without --event the holder is still safe to run by hand: its stdin is then
    the control, and closing it is the stop signal.

BUILD
    cmake -S tools/processcontext_holder -B build-holder -A x64
    cmake --build build-holder --config Release
    -> build-holder/bin/holder_ti.exe

    There are no options to point at the NGX headers or at MinHook, because this
    program has no dependency on either.
