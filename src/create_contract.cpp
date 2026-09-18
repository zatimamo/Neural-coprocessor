// ============================================================================
// MGPU Bridge - CREATECONTRACT implementation. See create_contract.hpp.
//
// Independently written for this experiment. No third-party source is copied.
//
// This file only writes interface values into the NVSDK_NGX_Parameter block that
// MGPU already owns and already passes to its existing CreateFeature call. It
// builds nothing and calls no NGX entry point itself.
// ============================================================================

#include "create_contract.hpp"

#include <cstdio>

#include "diag.hpp"
#include "../ext/ngx/nvsdk_ngx.h"

namespace mgpu::contract
{
namespace
{
    // The experimental ScalingRatio / Scale values. Scale is dimensionless and
    // 1.0 means "no upscaling", which is what a native-resolution neural stage
    // declaring Output == Input means.
    const float TEST_SCALE = 1.0f;
    const float TEST_SCALING_RATIO = 1.0f;

    const unsigned TEST_UPSCALING = 0u;
    const unsigned TEST_RENDER_PRESET = 0u;
    const int TEST_CREATE_FLAGS = 0;
}

void log_variant()
{
#ifdef MGPU_CONTRACT_EXPANDED
    mgpu::diag::info("[CREATECONTRACT] variant=contract_expanded");
#else
    mgpu::diag::info("[CREATECONTRACT] variant=contract_control");
#endif
}

bool expanded()
{
#ifdef MGPU_CONTRACT_EXPANDED
    return true;
#else
    return false;
#endif
}

void apply_expanded_create_contract(void *params_void, unsigned width, unsigned height,
                                    const char *where)
{
    if (params_void == nullptr)
    {
        mgpu::diag::warn("[CREATECONTRACT] parameter block is null - expanded contract not applied");
        return;
    }

    NVSDK_NGX_Parameter *params = static_cast<NVSDK_NGX_Parameter *>(params_void);
    const char *site = (where != nullptr) ? where : "unspecified";

#ifndef MGPU_CONTRACT_EXPANDED
    // contract_control: MGPU's existing parameters are left exactly as they are.
    // Nothing is set, nothing is changed.
    char l[192];
    std::snprintf(l, sizeof l,
                  "[CREATECONTRACT] variant=contract_control at %s - no parameters added "
                  "(MGPU's existing contract only)", site);
    mgpu::diag::info(l);
#else
    char l[288];
    std::snprintf(l, sizeof l,
                  "[CREATECONTRACT] variant=contract_expanded at %s - applying the additional "
                  "creation-time parameters below", site);
    mgpu::diag::info(l);

    // ---- generic NGX ----
    params->Set(NVSDK_NGX_Parameter_Width,  (unsigned int)width);
    params->Set(NVSDK_NGX_Parameter_Height, (unsigned int)height);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   Width = %u", width);
    mgpu::diag::info(l);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   Height = %u", height);
    mgpu::diag::info(l);

    // ---- creation / visibility node masks ----
    params->Set(NVSDK_NGX_Parameter_CreationNodeMask,   1u);
    params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    mgpu::diag::info("[CREATECONTRACT]   CreationNodeMask = 1");
    mgpu::diag::info("[CREATECONTRACT]   VisibilityNodeMask = 1");

    // ---- the feature's own namespace ----
    params->Set("DLSSNR.Width",  (unsigned int)width);
    params->Set("DLSSNR.Height", (unsigned int)height);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.Width = %u", width);
    mgpu::diag::info(l);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.Height = %u", height);
    mgpu::diag::info(l);

    params->Set("DLSSNR.InputWidth",  (unsigned int)width);
    params->Set("DLSSNR.InputHeight", (unsigned int)height);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.InputWidth = %u", width);
    mgpu::diag::info(l);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.InputHeight = %u", height);
    mgpu::diag::info(l);

    params->Set("DLSSNR.OutputWidth",  (unsigned int)width);
    params->Set("DLSSNR.OutputHeight", (unsigned int)height);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.OutputWidth = %u", width);
    mgpu::diag::info(l);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.OutputHeight = %u", height);
    mgpu::diag::info(l);

    params->Set("DLSSNR.Output.Width",  (unsigned int)width);
    params->Set("DLSSNR.Output.Height", (unsigned int)height);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.Output.Width = %u", width);
    mgpu::diag::info(l);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.Output.Height = %u", height);
    mgpu::diag::info(l);

    params->Set("DLSSNR.Upscaling", TEST_UPSCALING);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.Upscaling = %u", TEST_UPSCALING);
    mgpu::diag::info(l);

    params->Set("DLSSNR.Scale", TEST_SCALE);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.Scale = %.1f", (double)TEST_SCALE);
    mgpu::diag::info(l);

    params->Set("DLSSNR.ScalingRatio", TEST_SCALING_RATIO);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.ScalingRatio = %.1f",
                  (double)TEST_SCALING_RATIO);
    mgpu::diag::info(l);

    params->Set("DLSSNR.Hint.Render.Preset", TEST_RENDER_PRESET);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   DLSSNR.Hint.Render.Preset = %u",
                  TEST_RENDER_PRESET);
    mgpu::diag::info(l);

    params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, TEST_CREATE_FLAGS);
    std::snprintf(l, sizeof l, "[CREATECONTRACT]   %s = %d",
                  NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, TEST_CREATE_FLAGS);
    mgpu::diag::info(l);

    mgpu::diag::info("[CREATECONTRACT]   (all of the above set immediately before CreateFeature)");
#endif
}
}
