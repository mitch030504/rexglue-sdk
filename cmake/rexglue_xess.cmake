# rexglue_xess.cmake — Intel XeSS 3.0.2 Super Resolution (Windows)
#
# Intel ships XeSS as a Windows DLL only (libxess.dll), with D3D12 and Vulkan APIs. This downloads
# the SDK release by hash and extracts the headers, the DLL and the licences. The runtime loads the
# DLL when XeSS is chosen (src/ui/xess_runtime.cpp), so a missing DLL never stops the game.
#
# Sets REXGLUE_XESS_INCLUDE_DIR and REXGLUE_XESS_FILES (to stage next to the executable).

set(_rexglue_xess_default OFF)
if(WIN32)
    set(_rexglue_xess_default ON)
endif()
option(REXGLUE_ENABLE_XESS "Intel XeSS Super Resolution (Windows: D3D12 and Vulkan)" ${_rexglue_xess_default})
if(NOT REXGLUE_ENABLE_XESS)
    return()
endif()
if(NOT WIN32)
    message(STATUS "XeSS: Intel ships no Linux binaries; disabled")
    set(REXGLUE_ENABLE_XESS OFF CACHE BOOL "" FORCE)
    return()
endif()

set(_rexglue_xess_version "3.0.2")
set(_rexglue_xess_dir "${CMAKE_BINARY_DIR}/_deps/xess-${_rexglue_xess_version}")
set(_rexglue_xess_zip "${_rexglue_xess_dir}/XeSS_SDK_${_rexglue_xess_version}.zip")
if(NOT EXISTS "${_rexglue_xess_dir}/inc/xess/xess.h")
    message(STATUS "XeSS ${_rexglue_xess_version}: downloading the SDK")
    file(DOWNLOAD
        "https://github.com/intel/xess/releases/download/v${_rexglue_xess_version}/XeSS_SDK_${_rexglue_xess_version}.zip"
        "${_rexglue_xess_zip}"
        EXPECTED_HASH SHA256=88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990
        TLS_VERIFY ON STATUS _rexglue_xess_status)
    list(GET _rexglue_xess_status 0 _rexglue_xess_code)
    if(NOT _rexglue_xess_code EQUAL 0)
        file(REMOVE "${_rexglue_xess_zip}")
        message(FATAL_ERROR "Couldn't download XeSS: ${_rexglue_xess_status}. "
            "Set REXGLUE_ENABLE_XESS=OFF to build without it.")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_rexglue_xess_zip}" DESTINATION "${_rexglue_xess_dir}"
        PATTERNS "inc/xess/*" "bin/libxess.dll" "LICENSE.txt" "third-party-programs.txt")
    file(REMOVE "${_rexglue_xess_zip}")
endif()

set(REXGLUE_XESS_INCLUDE_DIR "${_rexglue_xess_dir}/inc" CACHE INTERNAL "XeSS SDK headers")
set(REXGLUE_XESS_FILES
    "${_rexglue_xess_dir}/bin/libxess.dll"
    CACHE INTERNAL "XeSS runtime to stage next to the executable")
set(REXGLUE_XESS_LICENSES
    "${_rexglue_xess_dir}/LICENSE.txt;xess_license.txt"
    "${_rexglue_xess_dir}/third-party-programs.txt;xess_third_party_programs.txt"
    CACHE INTERNAL "XeSS licence files (source;staged name)")
