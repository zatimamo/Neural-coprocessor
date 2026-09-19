// ============================================================================
// MGPU Bridge - APPID_NS: the NGX ApplicationId A/B
// ============================================================================
// ONE QUESTION, ONE VALUE.
//
//   NeuralScreen's WORKING Reserved18 path initialises NGX with
//
//       ApplicationId = 0x1000000ULL
//
//   for BOTH entry points:
//
//       core    NVSDK_NGX_D3D12_Init
//       snippet nvngx_dlssnr.dll!NVSDK_NGX_D3D12_Init_Ext
//
//   MGPU passes 0ULL to both, in the P1.0c startup probe and in the P4.1 stream
//   session.
//
//   Every other measurable difference between the two paths has been closed one
//   at a time - the D3D12 device, the caller identity (the nvngx.dll_ module
//   prefix), the runtime DLL, the architecture spoof, the expanded parameter
//   contract, the resolution, the transport - and the two private NVAPI
//   CUDA-interop calls still return NVAPI_ERROR under MGPU and NVAPI_OK under
//   NeuralScreen. The ApplicationId is a value the two paths genuinely differ in,
//   it is handed to the same two entry points, and it has never been varied.
//
// TWO VARIANTS, ONE SOURCE REVISION
//   MGPU_APPID_NS undefined  ->  CONTROL.  every Reserved18 init gets 0ULL.
//                                This is today's behaviour, unchanged.
//   MGPU_APPID_NS defined    ->  APPID_NS. every Reserved18 init gets
//                                0x1000000ULL.
//
//   The only artifact this phase publishes is APPID_NS; the CONTROL value lives
//   here so that the single difference between the two builds is this one number.
//
// WHAT "EVERY" MEANS, EXACTLY - four sessions, six call sites
//   P1.0c core     NVSDK_NGX_D3D12_Init        (and its Init_Ext fallback)
//   P1.0c snippet  NVSDK_NGX_D3D12_Init_Ext    (and its Init fallback)
//   P4.1  core     NVSDK_NGX_D3D12_Init
//   P4.1  snippet  NVSDK_NGX_D3D12_Init_Ext
//
//   All six call reserved18_app_id(). One function serves all of them, so the
//   four sessions cannot disagree with each other - which is the point. An
//   experiment in which the P1 probe and the P4 stream used different ids would
//   not be testing what it claims to test.
//
// WHAT IT MUST NOT TOUCH
//   The C2-SR session (s.sr_params, nvngx_dlss.dll) is NOT a Reserved18 session
//   and keeps 0ULL. The title's own DLSS sessions are not MGPU's to change and
//   MGPU does not call them at all. Nothing else in the add-on is conditional on
//   this definition: not the device, not the ReShade proxy / native choice, not
//   the queue / list / fence, not the parameter contract, not the architecture
//   hook, not the private NVAPI wrappers, not the runtime DLL, not the data path,
//   not the SDK version, not the feature id, not the resolution, not the
//   transport, not the depth and not the motion vectors.
//
// NO FALLBACK, NO RETRY, NO PROJECT ID
//   reserved18_app_id() returns one value. Nothing retries an Init with a
//   different id, and NVSDK_NGX_D3D12_Init_with_ProjectID is never called - not
//   even in the APPID_NS variant, because a project-id substitution would be a
//   second variable. A run whose Init fails is a result, not a reason to try
//   another number.
//
// INTERPRETATION, FIXED IN ADVANCE
//   A valid run requires P1.0c Init = Success AND P4.1 Init = Success. Then, on
//   the three observed quantities:
//
//     descriptor -1 -> 0  and  CuModule -1 -> 0
//         -> the ApplicationId was part of the blocker.
//     both still -1 with valid Init
//         -> the ApplicationId is EXONERATED, and the work returns to
//            PROCESSCONTEXT-AB.
//   Either Init FAIL_OutOfDate voids the NGX portion of the run; nothing in it
//   may be read as a result.
// ============================================================================

#pragma once

namespace mgpu::appidns
{
    // The ONE value this experiment varies, as the variant this build is.
    // CONTROL: 0ULL. APPID_NS: 0x1000000ULL.
    unsigned long long reserved18_app_id();

    // One line, once per process, naming the variant and the value, so a log can
    // never be attributed to the wrong arm.
    void log_variant();

    // The four fixed lines. Each states the id that the call it guards is about
    // to pass, so the log and the call cannot disagree:
    //
    //     [APPID] P1 core app_id=0x0000000001000000
    //     [APPID] P1 snippet app_id=0x0000000001000000
    //     [APPID] P4 core app_id=0x0000000001000000
    //     [APPID] P4 snippet app_id=0x0000000001000000
    //
    // In CONTROL all four print 0x0000000000000000.
    void log_p1_core();
    void log_p1_snippet();
    void log_p4_core();
    void log_p4_snippet();
}
