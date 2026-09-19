================================================================================
PROCESSCONTEXT-AB
Standalone diagnostic - does process-global / multi-device D3D12 state explain
why the private NVAPI CUDA-interop calls fail inside Cyberpunk but succeed in
NeuralScreen?
================================================================================

WHAT IT IS

One executable, four arms, each in its own process. Every arm runs the SAME
NeuralScreen-equivalent reference lane on the RTX 4070, and differs ONLY in
second-adapter state established before that lane starts:

    single          nothing else at all. The reference. MUST succeed.
    dual-held       RTX 4070 Ti SUPER device created and held.
                    No NGX, no swapchain, no queue, no heap on it.
    dual-active     ... plus a DIRECT command queue and a small, live
                    CBV/SRV/UAV descriptor heap held on that device.
    dual-released   Ti SUPER device created, recorded, then RELEASED. Nothing
                    from it is held when the lane runs.

While the lane runs, the two private calls the DLSSNR runtime makes during
CreateFeature are observed, pass-through only:

    0x0DDAC234  NvAPI_D3D12_GetCudaIndependentDescriptorObject
    0xAD1A677D  NvAPI_D3D12_CreateCuModule

and so is the feature creation itself:

    NVSDK_NGX_D3D12_CreateFeature(NVSDK_NGX_Feature_Reserved18) at 640x360

INTERPRETATION, FIXED IN ADVANCE

    E) SINGLE does not succeed        -> INVALID. Stop. Interpret nothing.
    A) SINGLE ok + DUAL-HELD fails    -> simultaneous multi-adapter state is
                                         sufficient.
    B) A and DUAL-ACTIVE fails        -> an ACTIVE second command/descriptor
                                         state is the trigger, not the device.
    C) DUAL-RELEASED fails            -> process-global NVIDIA state persists
                                         after release.
    D) all four succeed               -> the multi-device hypothesis is
                                         EXONERATED.

The table is printed by "processcontext_ab.exe --mode table", which reads the
four per-arm logs. That program applies exactly the rules above, in that order,
and nothing else.


HOW TO RUN

    RUN-CONTEXT-AB.cmd
    RUN-CONTEXT-AB.cmd "C:\full\path\to\nvngx_dlssnr.dll"

The NR DLL defaults to nvngx_dlssnr.dll beside the script. Its SHA256 is
computed and printed BEFORE it is loaded, and a mismatch aborts without loading
anything:

    required = dcc0dc2414aedec4a8e084647070383be068554042587180c20c784d4772d36f
               (NVIDIA DLSS-NR runtime 310.8.0, the build NeuralScreen v1.15.0 runs)

Requirements: an RTX 4070 Ti SUPER (10DE:2705) AND an RTX 4070 (10DE:2786) both
present, a driver that provides nvapi64.dll, the NGX core _nvngx.dll installed
by the driver (HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath), and
the NR DLL named above. Adapters are selected by PCI device id; an ordinal is
never selected.

Nothing is deployed. Cyberpunk is never launched. NeuralScreen is not touched.


WHY THE EXECUTABLE IS COPIED TO A NAME CONTAINING "nvngx.dll"

The DLSSNR snippet inspects the module file name of its CALLER and refuses a
caller whose name does not contain the substring "nvngx.dll", with
0xBAD00002 FAIL_PlatformError. This is why MGPU deploys as
"nvngx.dll_mgpu_bridge.addon64". RUN-CONTEXT-AB.cmd does the same thing: it
copies processcontext_ab.exe to nvngx.dll_processcontext_ab.exe, runs the four
arms under that name, and deletes the copy afterwards.

Running processcontext_ab.exe directly works, but the SINGLE arm will then fail
its caller check for a reason that has NOTHING to do with the hypothesis under
test - which makes the whole diagnostic INVALID under rule E. The program detects
this at startup, logs the module name it is running as, and prints a loud warning
if the name lacks the prefix.


FILES

    processcontext_ab.exe       the diagnostic
    RUN-CONTEXT-AB.cmd          runs all four arms, then prints the table
    README.txt                  this file
    SHA256SUMS.txt              hashes of the above

    context-single.log          } written next to the executable, one per arm,
    context-dual-held.log       } by that arm's own process
    context-dual-active.log     }
    context-dual-released.log   }


WHAT IT DOES NOT DO

    * It does not modify MGPU: no add-on source, artifact or behaviour changes.
    * It does not modify NeuralScreen.
    * It does not modify or patch any NVIDIA binary, NVAPI result, handle, status,
      argument or invocation count. The two private calls are called exactly once
      each, with the original arguments, and their NvApi_Status is returned
      verbatim.
    * It never dereferences pBlob. Only the pointer value and the size are logged.
    * It reads a caller-owned __out value ONLY when the status is NVAPI_OK. On a
      failure the log says the value was NOT READ and why.
    * It loads no ReShade and no Streamline.
    * It does not deploy, install, or copy anything outside its own directory.
    * It never launches Cyberpunk.


READING A LOG

Each arm's log records, in order: the module name it is running as, the adapter
enumeration with vendor id / device id / LUID for every adapter, the NR DLL hash
and the gate result, the second-adapter state that arm establishes, the control
device and its LUID, every NVAPI id resolved through nvapi_QueryInterface with
its caller and stack, the two private calls with their arguments and statuses,
the NGX entry-point results, and finally one machine-readable line:

    PCAB-RESULT mode=... valid=... control=... core_init=... caps=...
                snip_init=... populate=... feature=...
                desc_status=... desc_calls=... desc_probes=...
                cu_status=... cu_calls=... cu_probes=...

Two things in that line are worth knowing before reading a log:

  * NULL-argument capability probes are separated from real calls. A probe
    returning -14 (NVAPI_INVALID_POINTER) is the expected answer to a validation
    probe and is NOT a failure. desc_probes / cu_probes count them; desc_status
    and cu_status are the status of a REAL call only, or NA if no real call
    happened.

  * An armed line that is ABSENT means the diagnostic did not intercept that
    interface: the call is UNOBSERVED in that run, which is not a pass and not a
    failure. It is reported as "NOT OBSERVED", never as a zero.

The per-arm log also names the two calls' callers (module+offset) and prints the
stack above nvapi_QueryInterface, so the observation is equivalent to MGPU's
CUDADIAG rather than a summary of it.
