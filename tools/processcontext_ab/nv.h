// ============================================================================
// PROCESSCONTEXT-AB - shared declarations.
//
// Minimal NVAPI + NGX surface for the standalone diagnostic. Everything here is
// a public vendor fact, quoted rather than guessed:
//
//   nvapi_interface.h : { "NvAPI_Initialize",             0x0150e828 }
//                       { "NvAPI_EnumPhysicalGPUs",        0xe5ac921f }
//                       { "NvAPI_GPU_GetPCIIdentifiers",   0x2ddfb66e }
//                       { "NvAPI_GPU_GetArchInfo",         0xd8265d24 }
//                       { "NvAPI_D3D12_GetCudaIndependentDescriptorObject", 0x0ddac234 }
//                       { "NvAPI_D3D12_CreateCuModule",                    0xad1a677d }
//   nvapi.h           : NV_GPU_ARCH_INFO_V2 = { NvU32 version; NvU32 architecture;
//                                               NvU32 implementation; NvU32 revision; }
//                       NV_GPU_ARCH_INFO_VER_2 = MAKE_NVAPI_VERSION(...,2)
//
// The NGX entry-point typedefs are the same ones the MGPU add-on uses, which are
// CI-proven to compile and to drive a session to Init=Success. The
// D3D12 forms are declared here because nvsdk_ngx_d3d12.h is not in the NVIDIA
// header tree this repository pins.
// ============================================================================

#pragma once

#include <windows.h>
// IID_PPV_ARGS lives in combaseapi.h, and WIN32_LEAN_AND_MEAN keeps windows.h
// from pulling in ole2.h, which is what would otherwise provide it. The MGPU
// add-on includes it explicitly for the same reason.
#include <combaseapi.h>
#include <d3d12.h>

// Order matters: d3d12.h first. The NGX header forward-declares ID3D12Device
// (as does d3d12.h), and a repeated typedef to the same struct is legal in C++.
#include <nvsdk_ngx.h>

// ------------------------------------------------------------------ NVAPI

namespace pcab
{
    const unsigned NVAPI_ID_INITIALIZE          = 0x0150E828u;
    const unsigned NVAPI_ID_ENUM_PHYSICAL_GPUS  = 0xE5AC921Fu;
    const unsigned NVAPI_ID_GET_PCI_IDENTIFIERS = 0x2DDFB66Eu;
    const unsigned NVAPI_ID_GET_ARCH_INFO       = 0xD8265D24u;
    const unsigned NVAPI_ID_GET_CUDA_DESCRIPTOR = 0x0DDAC234u;
    const unsigned NVAPI_ID_CREATE_CU_MODULE    = 0xAD1A677Du;

    // NV_GPU_ARCH_INFO_VER_2 = 2 * 1000 + 0  (MAKE_NVAPI_VERSION)
    const unsigned NV_GPU_ARCH_INFO_VER_2 = 2000u;

    struct NV_GPU_ARCH_INFO
    {
        unsigned version;
        unsigned architecture;
        unsigned implementation;
        unsigned revision;
    };
    static_assert(sizeof(NV_GPU_ARCH_INFO) == 16, "NV_GPU_ARCH_INFO is four NvU32");

    // The value NeuralScreen spoofs to: the architecture the DLSSNR 310.8.0
    // runtime accepts ("spoofed to the DLL's accepted value"; VERSION.txt of the
    // NeuralScreen package says 0x1B0). The real Ada value it replaces is 0x190.
    const unsigned SPOOF_ARCHITECTURE   = 0x1B0u;
    const unsigned SPOOF_IMPLEMENTATION = 0x3u;
    const unsigned SPOOF_REVISION       = 0xA1u;

    using pfn_query = void *(*)(unsigned int);
    using pfn_init  = int (*)(void);

    // nvapi64.dll is loaded here, nvapi_QueryInterface is resolved, and
    // NvAPI_Initialize is called - the same ordering NeuralScreen's worker uses
    // (its error string is "[arch] NvAPI_Initialize failed", and it resolves the
    // nvapi functions by id immediately afterwards).
    bool nv_load_and_initialize();
    void *nv_query(unsigned id);

    // The NeuralScreen-compatible architecture patch: the resolved
    // NvAPI_GPU_GetArchInfo entry is detoured so every query reports the value
    // the DLSSNR runtime accepts. NVIDIA's own code still serves the call - only
    // the returned architecture is rewritten - and the patch is installed only
    // when the real value is not already accepted.
    bool arch_patch_install();
    void arch_patch_remove();
    unsigned arch_real_value();

    // ------------------------------------------------------------- modules

    // The NGX core and the snippet are loaded by explicit path. The snippet is
    // always the caller-supplied --nr-dll, never a copy beside the executable.
    bool ngx_load_core();
    bool ngx_load_snippet(const wchar_t *path);
    HMODULE ngx_core();
    HMODULE ngx_snippet();

    // ------------------------------------------------- NGX D3D12 entry points
    // The D3D12 forms are not in the pinned NVIDIA header tree.

    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_init)(
        unsigned long long InApplicationId,
        const wchar_t *InApplicationDataPath,
        ID3D12Device *InDevice,
        const NVSDK_NGX_FeatureCommonInfo *InFeatureInfo,
        NVSDK_NGX_Version InSDKVersion);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_init_ext)(
        unsigned long long InApplicationId,
        const wchar_t *InApplicationDataPath,
        ID3D12Device *InDevice,
        NVSDK_NGX_Version InSDKVersion,
        const NVSDK_NGX_Parameter *InParameters);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_get_cap_params)(
        NVSDK_NGX_Parameter **OutParameters);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_populate_params)(
        NVSDK_NGX_Parameter *InParameters);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_create_feature)(
        ID3D12GraphicsCommandList *InCmdList,
        NVSDK_NGX_Feature InFeatureID,
        NVSDK_NGX_Parameter *InParameters,
        NVSDK_NGX_Handle **OutHandle);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_release_feature)(
        NVSDK_NGX_Handle *InHandle);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_shutdown1)(
        ID3D12Device *InDevice);

    // ------------------------------------------------------------ logging
    // One log per process, written to stdout AND to the mode's file.

    void log_open(const char *mode_name);
    void log_close();
    void logf(const char *fmt, ...);

    // SHA256 of a file, via CNG. Returns false and fills err on failure.
    bool sha256_file(const wchar_t *path, char out_hex[65], unsigned long *out_size);
}
