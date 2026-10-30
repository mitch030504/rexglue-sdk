# rexglue_fidelityfx_prebuilt.cmake — AMD FidelityFX SDK 2.4 prebuilt runtime (Windows)
#
# FSR 4.1.2 (with FSR 3.1 as its fallback) only ships as AMD's signed DLLs, one loader + upscaler
# pair per graphics API. This downloads just those and the API headers, pinned by hash, instead of
# the whole 550 MB SDK repository. The runtime picks the loader per API at startup
# (src/ui/ffx_runtime.cpp), so one build serves both D3D12 and Vulkan. There are no Linux binaries;
# Linux keeps the source-built FSR 3.1 (rexglue_fidelityfx.cmake).
#
# Sets REXGLUE_FIDELITYFX_PREBUILT_INCLUDE_DIR (forwarding headers for <ffx_api/...>) and
# REXGLUE_FIDELITYFX_PREBUILT_FILES (DLLs and licence to stage next to the executable).

set(_rexglue_ffx_prebuilt_default OFF)
if(WIN32)
    set(_rexglue_ffx_prebuilt_default ON)
endif()
option(REXGLUE_FIDELITYFX_PREBUILT
    "Use AMD's prebuilt FidelityFX SDK 2.4 runtime (FSR 4.1.2) instead of building FSR 3.1" ${_rexglue_ffx_prebuilt_default})
if(NOT REXGLUE_FIDELITYFX_PREBUILT)
    return()
endif()
if(NOT WIN32)
    message(WARNING "REXGLUE_FIDELITYFX_PREBUILT: AMD ships Windows DLLs only; using the source build")
    set(REXGLUE_FIDELITYFX_PREBUILT OFF CACHE BOOL "" FORCE)
    return()
endif()

set(_rexglue_ffx_version "v2.4.0")
set(_rexglue_ffx_url
    "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${_rexglue_ffx_version}")
set(_rexglue_ffx_dir "${CMAKE_BINARY_DIR}/_deps/fidelityfx-prebuilt-${_rexglue_ffx_version}")
# path;sha256 pairs, relative to the repository root.
set(_rexglue_ffx_files
    "Kits/FidelityFX/api/include/ffx_api.h;4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf"
    "Kits/FidelityFX/api/include/ffx_api_types.h;53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c"
    "Kits/FidelityFX/api/include/ffx_api_loader.h;18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e"
    "Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h;c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"
    "Kits/FidelityFX/api/include/vk/ffx_api_vk.h;09ac1855bc4c2f711243d6982909385072ca3f8d30db44d1a82ee5bd637832ad"
    "Kits/FidelityFX/upscalers/include/ffx_upscale.h;13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d"
    "Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll;e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb"
    "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll;aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace"
    "Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll;38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9"
    "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll;4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"
    "Kits/FidelityFX/docs/license.md;32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb"
)
foreach(_rexglue_ffx_entry IN LISTS _rexglue_ffx_files)
    # Each list entry is "path;hash", which foreach splits; re-pair them.
    if(NOT DEFINED _rexglue_ffx_path)
        set(_rexglue_ffx_path "${_rexglue_ffx_entry}")
        continue()
    endif()
    set(_rexglue_ffx_dest "${_rexglue_ffx_dir}/${_rexglue_ffx_path}")
    if(NOT EXISTS "${_rexglue_ffx_dest}")
        message(STATUS "FidelityFX ${_rexglue_ffx_version}: downloading ${_rexglue_ffx_path}")
        file(DOWNLOAD "${_rexglue_ffx_url}/${_rexglue_ffx_path}" "${_rexglue_ffx_dest}"
            EXPECTED_HASH SHA256=${_rexglue_ffx_entry} TLS_VERIFY ON STATUS _rexglue_ffx_status)
        list(GET _rexglue_ffx_status 0 _rexglue_ffx_code)
        if(NOT _rexglue_ffx_code EQUAL 0)
            file(REMOVE "${_rexglue_ffx_dest}")
            message(FATAL_ERROR "Couldn't download FidelityFX ${_rexglue_ffx_path}: "
                "${_rexglue_ffx_status}. Set REXGLUE_FIDELITYFX_PREBUILT=OFF to build FSR 3.1 instead.")
        endif()
    endif()
    unset(_rexglue_ffx_path)
endforeach()

# The code includes <ffx_api/...> (the 1.1.4 source layout); point those names at the 2.4 kit.
set(_rexglue_ffx_include "${_rexglue_ffx_dir}/include")
set(_rexglue_ffx_kit "${_rexglue_ffx_dir}/Kits/FidelityFX")
foreach(_rexglue_ffx_pair
        "ffx_api.h;api/include/ffx_api.h"
        "ffx_api_types.h;api/include/ffx_api_types.h"
        "ffx_api_loader.h;api/include/ffx_api_loader.h"
        "ffx_upscale.h;upscalers/include/ffx_upscale.h"
        "dx12/ffx_api_dx12.h;api/include/dx12/ffx_api_dx12.h"
        "vk/ffx_api_vk.h;api/include/vk/ffx_api_vk.h")
    list(GET _rexglue_ffx_pair 0 _rexglue_ffx_name)
    list(GET _rexglue_ffx_pair 1 _rexglue_ffx_target)
    file(CONFIGURE OUTPUT "${_rexglue_ffx_include}/ffx_api/${_rexglue_ffx_name}"
        CONTENT "#pragma once\n#include \"${_rexglue_ffx_kit}/${_rexglue_ffx_target}\"\n")
endforeach()

set(REXGLUE_FIDELITYFX_PREBUILT_INCLUDE_DIR "${_rexglue_ffx_include}" CACHE INTERNAL
    "Forwarding headers for the prebuilt FidelityFX SDK")
set(REXGLUE_FIDELITYFX_PREBUILT_FILES
    "${_rexglue_ffx_kit}/signedbin/amd_fidelityfx_loader_dx12.dll"
    "${_rexglue_ffx_kit}/signedbin/amd_fidelityfx_upscaler_dx12.dll"
    "${_rexglue_ffx_kit}/signedbin/amd_fidelityfx_loader_vk.dll"
    "${_rexglue_ffx_kit}/signedbin/amd_fidelityfx_upscaler_vk.dll"
    CACHE INTERNAL "Prebuilt FidelityFX DLLs to stage next to the executable")
set(REXGLUE_FIDELITYFX_PREBUILT_LICENSE "${_rexglue_ffx_kit}/docs/license.md" CACHE INTERNAL
    "Licence of the prebuilt FidelityFX DLLs")
