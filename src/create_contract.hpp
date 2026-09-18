// ============================================================================
// MGPU Bridge - CREATECONTRACT: creation-time parameter contract A/B
// ============================================================================
// ONE QUESTION, ONE BEHAVIOUR.
//
//   ARCHTEST closed with a measured answer: spoofing the RTX 4070's
//   architecture to Blackwell makes the private DLSS-NR runtime's Init return
//   Success, and CreateFeature(Reserved18) STILL returns 0xBAD00002. The
//   architecture gate is therefore real but is not the CreateFeature gate.
//
//   This experiment asks the next question and changes nothing else:
//
//     Does supplying the creation-time parameter contract the feature expects
//     change CreateFeature(Reserved18) from 0xBAD00002 into Success or another
//     result?
//
// TWO VARIANTS, ONE SOURCE REVISION
//   contract_control  : MGPU's existing CreateFeature parameters, unchanged.
//   contract_expanded : the same, plus the additional creation-time keys.
//
// BOTH keep the working architecture COMPAT hook exactly as it is. The spoof is
// REQUIRED to reach Init=Success, so removing it would invalidate the
// comparison; it is held constant across the two variants.
//
// SCOPE OF "ONLY"
//   This module sets interface values on the parameter block MGPU already owns
//   and passes to its existing CreateFeature call. It creates no device, no
//   queue, no allocator, no command list, no parameter block and no runtime. It
//   does not touch the architecture hook, the caller module, the DLL, the
//   driver, the dimensions, the command list, the transport, depth/motion
//   settings or any game setting.
//
//   The types below are not guessed. NVSDK_NGX_Parameter::Set is overloaded for
//   unsigned int, int, float and pointers, and every type used here already
//   appears in this repository:
//     unsigned  - gpu1_context.cpp sets NVSDK_NGX_Parameter_CreationNodeMask with 1u
//                 and NVSDK_NGX_Parameter_VisibilityNodeMask with 1u
//     int       - gpu1_context.cpp sets NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags
//                 with (int)
//     float     - gpu1_context.cpp sets "DLSSNR.MVecScaleX" with 1.0f
//
//   The key names are quoted from the pinned NVIDIA/DLSS header rather than
//   invented:
//     NVSDK_NGX_Parameter_Width                     "Width"
//     NVSDK_NGX_Parameter_Height                    "Height"
//     NVSDK_NGX_Parameter_CreationNodeMask          "CreationNodeMask"
//     NVSDK_NGX_Parameter_VisibilityNodeMask        "VisibilityNodeMask"
//     NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags "DLSS.Feature.Create.Flags"
//   The DLSSNR.* names are feature-private and live in no public header; they
//   are used verbatim as string literals, exactly as MGPU already uses
//   "DLSSNR.Width", "DLSSNR.MVecScaleX" and the rest.
#pragma once

namespace mgpu::contract
{
    // The one line that identifies which contract a log came from.
    void log_variant();

    // Apply the additional creation-time parameters, then log every one of them
    // once. A no-op in the contract_control build.
    //
    // `where` labels the call site ("startup probe" / "real stream") so the two
    // paths stay comparable in the log.
    void apply_expanded_create_contract(void *params, unsigned width, unsigned height,
                                        const char *where);

    // True when this build applies the expanded contract.
    bool expanded();
}
