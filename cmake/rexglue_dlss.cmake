# rexglue_dlss.cmake — NVIDIA DLSS Super Resolution (NGX, DLSS SDK 310.9.16)
#
# Linux and Windows, D3D12 and Vulkan. NGX is a static library linked into the runtime; the DLSS
# model itself (nvngx_dlss.dll / libnvidia-ngx-dlss.so.*) is loaded by the NVIDIA driver from next
# to the executable when DLSS is created. Needs an RTX GPU; elsewhere NGX reports DLSS unavailable
# and the upscaler falls back to FSR. Only the files used are downloaded, pinned by hash.
#
# Sets REXGLUE_DLSS_INCLUDE_DIR, REXGLUE_DLSS_LIBRARY and REXGLUE_DLSS_FILES (to stage).

option(REXGLUE_ENABLE_DLSS "NVIDIA DLSS Super Resolution (NGX)" ON)
if(NOT REXGLUE_ENABLE_DLSS)
    return()
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(STATUS "DLSS: NGX ships x86-64 libraries for this SDK; disabled")
    set(REXGLUE_ENABLE_DLSS OFF CACHE BOOL "" FORCE)
    return()
endif()

set(_rexglue_dlss_version "310.9.16")
set(_rexglue_dlss_url "https://raw.githubusercontent.com/NVIDIA/DLSS/v${_rexglue_dlss_version}")
set(_rexglue_dlss_dir "${CMAKE_BINARY_DIR}/_deps/dlss-${_rexglue_dlss_version}")
# path;sha256 pairs, relative to the repository root.
set(_rexglue_dlss_files
    "LICENSE.txt;d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"
    "include/nvsdk_ngx.h;dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"
    "include/nvsdk_ngx_defs.h;ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"
    "include/nvsdk_ngx_defs_dlssd.h;2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716"
    "include/nvsdk_ngx_defs_dlssg.h;c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1"
    "include/nvsdk_ngx_defs_vk.h;0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031"
    "include/nvsdk_ngx_helpers.h;5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"
    "include/nvsdk_ngx_helpers_cuda.h;f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042"
    "include/nvsdk_ngx_helpers_d3d.h;ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"
    "include/nvsdk_ngx_helpers_dlssd.h;a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532"
    "include/nvsdk_ngx_helpers_dlssd_cuda.h;7e35cde214d905346a487b9a28c05d35e1c55b480e237fe3de69580a7e332730"
    "include/nvsdk_ngx_helpers_dlssd_d3d.h;6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293"
    "include/nvsdk_ngx_helpers_dlssd_vk.h;11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b"
    "include/nvsdk_ngx_helpers_dlssg.h;6881d21419c40c78ec948b326902ece09fc112b0223144d51c4905f3fced7a1f"
    "include/nvsdk_ngx_helpers_dlssg_d3d.h;6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220"
    "include/nvsdk_ngx_helpers_dlssg_vk.h;b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316"
    "include/nvsdk_ngx_helpers_vk.h;c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623"
    "include/nvsdk_ngx_loader.h;abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75"
    "include/nvsdk_ngx_params.h;943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"
    "include/nvsdk_ngx_params_dlssd.h;d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e"
    "include/nvsdk_ngx_params_dlssg.h;c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad"
    "include/nvsdk_ngx_standalone_common.h;38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53"
    "include/nvsdk_ngx_standalone_cuda.h;b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499"
    "include/nvsdk_ngx_vk.h;2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d"
)
if(WIN32)
    set(_rexglue_dlss_library "lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    set(_rexglue_dlss_runtime "lib/Windows_x86_64/rel/nvngx_dlss.dll")
    list(APPEND _rexglue_dlss_files
        "${_rexglue_dlss_library};bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c"
        "${_rexglue_dlss_runtime};d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e")
else()
    set(_rexglue_dlss_library "lib/Linux_x86_64/libnvsdk_ngx.a")
    set(_rexglue_dlss_runtime "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.${_rexglue_dlss_version}")
    list(APPEND _rexglue_dlss_files
        "${_rexglue_dlss_library};d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df"
        "${_rexglue_dlss_runtime};30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6")
endif()

foreach(_rexglue_dlss_entry IN LISTS _rexglue_dlss_files)
    # Each list entry is "path;hash", which foreach splits; re-pair them.
    if(NOT DEFINED _rexglue_dlss_path)
        set(_rexglue_dlss_path "${_rexglue_dlss_entry}")
        continue()
    endif()
    set(_rexglue_dlss_dest "${_rexglue_dlss_dir}/${_rexglue_dlss_path}")
    if(NOT EXISTS "${_rexglue_dlss_dest}")
        message(STATUS "DLSS ${_rexglue_dlss_version}: downloading ${_rexglue_dlss_path}")
        file(DOWNLOAD "${_rexglue_dlss_url}/${_rexglue_dlss_path}" "${_rexglue_dlss_dest}"
            EXPECTED_HASH SHA256=${_rexglue_dlss_entry} TLS_VERIFY ON STATUS _rexglue_dlss_status)
        list(GET _rexglue_dlss_status 0 _rexglue_dlss_code)
        if(NOT _rexglue_dlss_code EQUAL 0)
            file(REMOVE "${_rexglue_dlss_dest}")
            message(FATAL_ERROR "Couldn't download DLSS ${_rexglue_dlss_path}: "
                "${_rexglue_dlss_status}. Set REXGLUE_ENABLE_DLSS=OFF to build without it.")
        endif()
    endif()
    unset(_rexglue_dlss_path)
endforeach()

set(REXGLUE_DLSS_INCLUDE_DIR "${_rexglue_dlss_dir}/include" CACHE INTERNAL "NGX headers")
set(REXGLUE_DLSS_LIBRARY "${_rexglue_dlss_dir}/${_rexglue_dlss_library}" CACHE INTERNAL
    "NGX static library")
set(REXGLUE_DLSS_FILES "${_rexglue_dlss_dir}/${_rexglue_dlss_runtime}" CACHE INTERNAL
    "DLSS runtime to stage next to the executable")
set(REXGLUE_DLSS_LICENSE "${_rexglue_dlss_dir}/LICENSE.txt" CACHE INTERNAL "DLSS SDK licence")
