# DLSS 5 neural rendering on AMD (DLSSNR-AMD, MIT; Linux/Vulkan).
#
# Native Vulkan/GLSL reimplementation of the DLSS 5 NR network, sharing the
# application's VkDevice/VkQueue. Linux x86-64 only: the pinned runtime needs
# FP8 cooperative matrices, wave32 and Vulkan 1.3.
#
# Produces:
#   rex_dlss5_amd          static library (nr::Runtime, native plan)
#   rexglue-dlss5-amd-data staged tree  <build>/dlssnr-amd/{shaders,LICENSE,...}
#   REXGLUE_DLSS5_AMD_INCLUDE_DIR / REXGLUE_DLSS5_AMD_STAGE_DIR
#
# The NVIDIA model weights are never part of this tree; the runtime loads
# ROOT/dlssnr-amd/dlssnr-small.bin from a user-supplied root at run time.

option(REXGLUE_ENABLE_DLSS5_AMD
       "DLSS 5 neural rendering on AMD (native Vulkan; Linux, RDNA4)" OFF)
if(NOT REXGLUE_ENABLE_DLSS5_AMD)
    return()
endif()

if(NOT UNIX OR APPLE)
    message(STATUS "DLSS 5 AMD: Linux/Vulkan only; disabled")
    set(REXGLUE_ENABLE_DLSS5_AMD OFF CACHE BOOL "" FORCE)
    return()
endif()
if(NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "REXGLUE_ENABLE_DLSS5_AMD requires REXGLUE_USE_VULKAN")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(STATUS "DLSS 5 AMD: the pinned runtime is x86-64 only; disabled")
    set(REXGLUE_ENABLE_DLSS5_AMD OFF CACHE BOOL "" FORCE)
    return()
endif()
if(NOT MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(STATUS "DLSS 5 AMD: unsupported compiler ${CMAKE_CXX_COMPILER_ID}; disabled")
    set(REXGLUE_ENABLE_DLSS5_AMD OFF CACHE BOOL "" FORCE)
    return()
endif()

set(_rexglue_dlss5_amd_pin "191b97b54175d50c7ec5992b54322dd3eb0ef8ac")

# An explicit tree (a developer checkout, or a CI cache) takes precedence over a
# network fetch. It must already be at the pin.
set(REXGLUE_DLSS5_AMD_SOURCE_DIR "" CACHE PATH
    "Pinned DLSSNR-AMD checkout; empty fetches the pinned commit at configure time")
if(REXGLUE_DLSS5_AMD_SOURCE_DIR)
    set(_src "${REXGLUE_DLSS5_AMD_SOURCE_DIR}")
    if(NOT EXISTS "${_src}/linux/src/core/nr_runtime.cpp")
        message(FATAL_ERROR
            "REXGLUE_DLSS5_AMD_SOURCE_DIR=${_src} is not a DLSSNR-AMD tree")
    endif()
else()
    include(FetchContent)
    FetchContent_Declare(rexglue_dlss5_amd
        GIT_REPOSITORY https://github.com/Overclockers/DLSSNR-AMD.git
        GIT_TAG        ${_rexglue_dlss5_amd_pin}
        GIT_SHALLOW    FALSE
        GIT_PROGRESS   TRUE)
    FetchContent_GetProperties(rexglue_dlss5_amd)
    if(NOT rexglue_dlss5_amd_POPULATED)
        FetchContent_Populate(rexglue_dlss5_amd)
    endif()
    set(_src "${rexglue_dlss5_amd_SOURCE_DIR}")
endif()

execute_process(COMMAND git -C "${_src}" rev-parse HEAD
    OUTPUT_VARIABLE _dlss5_got OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _dlss5_got STREQUAL _rexglue_dlss5_amd_pin)
    message(FATAL_ERROR
        "DLSS 5 AMD: source is '${_dlss5_got}', expected ${_rexglue_dlss5_amd_pin}")
endif()

# The pinned shader toolchain. The SDK's glslang is a 2020 revision and does not
# reproduce the upstream SPIR-V; build_network.py hardcodes
# toolchain/glslang/bin/glslang inside the tree.
set(_glslang_ver "16.5.0")
if(NOT EXISTS "${_src}/toolchain/glslang/bin/glslang")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_glslang_archive "${CMAKE_BINARY_DIR}/_deps/glslang-${_glslang_ver}.tar.gz")
    file(DOWNLOAD
        "https://github.com/KhronosGroup/glslang/releases/download/${_glslang_ver}/glslang-${_glslang_ver}-linux-x86_64-release.tar.gz"
        "${_glslang_archive}"
        EXPECTED_HASH SHA256=b9b1f96acb898a62251b171f7695efcecfc206a530299054071919b06820f657
        TLS_VERIFY ON STATUS _glslang_status)
    list(GET _glslang_status 0 _glslang_code)
    if(NOT _glslang_code EQUAL 0)
        message(FATAL_ERROR "DLSS 5 AMD: cannot download glslang ${_glslang_ver}: ${_glslang_status}")
    endif()
    file(MAKE_DIRECTORY "${_src}/toolchain/glslang")
    file(ARCHIVE_EXTRACT INPUT "${_glslang_archive}" DESTINATION "${_src}/toolchain/glslang")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_network_dir "${CMAKE_BINARY_DIR}/dlss5-amd-network")
set(_stage_dir "${CMAKE_BINARY_DIR}/dlssnr-amd")

file(GLOB _dlss5_shader_sources CONFIGURE_DEPENDS
    "${_src}/linux/shaders/rdna4/*.comp"
    "${_src}/linux/shaders/rdna4/include/*.glsl"
    "${_src}/linux/shaders/passes/*.comp"
    "${_src}/linux/shaders/passes/include/*.glsl")

add_custom_command(
    OUTPUT "${_network_dir}/.stamp"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_network_dir}"
    COMMAND ${Python3_EXECUTABLE} "${_src}/linux/build/build_network.py" rdna4
            --out "${_network_dir}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_network_dir}/.stamp"
    DEPENDS
        "${_src}/linux/build/build_network.py"
        "${_src}/linux/shaders/rdna4/pipelines.json"
        "${_src}/toolchain/glslang/bin/glslang"
        ${_dlss5_shader_sources}
    COMMENT "Building DLSS 5 AMD shaders (72 SPIR-V modules)"
    VERBATIM)
add_custom_target(rexglue_dlss5_amd_shaders DEPENDS "${_network_dir}/.stamp")

add_custom_command(
    OUTPUT "${_stage_dir}/.stamp"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_stage_dir}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_dir}/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_network_dir}" "${_stage_dir}/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}/LICENSE"
            "${_stage_dir}/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}/THIRD_PARTY.md"
            "${_stage_dir}/THIRD_PARTY.md"
    COMMAND ${CMAKE_COMMAND} -E touch "${_stage_dir}/.stamp"
    DEPENDS "${_network_dir}/.stamp")
add_custom_target(rexglue_dlss5_amd_data ALL DEPENDS "${_stage_dir}/.stamp")

# The host must be compiled with the same constants as the shaders
# (linux/build/arch/rdna4.sh). Keep this list in sync with that file; the
# configure check below catches a silent pin bump.
set(_dlss5_defines
    NR_GEMM_WIDE_MT=64 NR_GEMM_WIDE_NT=256
    NR_GEMM_PROJ_MT=32 NR_GEMM_PROJ_NT=128
    NR_PROJW_MIN_TOKENS=768 NR_GEMM_REMAP_HOST=1
    NR_FFWD_WGW=4 NR_FFWD_GMAJOR=1
    NR_UPSVIEW_VEC=1 NR_REPACK_VEC=1
    NR_WIDE_UPS_MASK=7 NR_PERSIST_DF=1 NR_DS_FUSE=15
    NR_DECUPS_VEC=16 NR_DECQ_SMALL=1
    NR_PERSIST_DS_MASK=7 NR_PERSIST_UPS_MASK=7 NR_REPACK_FOLD=1
    NR_STRAG_PCT=25 NR_PERSIST_ONE_MASK=6
    NR_NOISE_FIELD=1 NR_FFWD_FM2_MIN_TOKENS=2560
    NR_POST_ALPHA=1 NR_TCHAIN_TILES=1)

add_library(rex_dlss5_amd STATIC
    "${_src}/linux/src/core/nr_runtime.cpp"
    "${_src}/linux/src/core/nr_native_plan.cpp")
add_library(rex::dlss5_amd ALIAS rex_dlss5_amd)
target_include_directories(rex_dlss5_amd PRIVATE "${_src}/linux/src/core")
target_link_libraries(rex_dlss5_amd PRIVATE Vulkan::Headers)
# The runtime calls vk* directly; the SDK itself loads the loader at run time,
# so this library must link the loader explicitly.
find_library(REXGLUE_VULKAN_LOADER NAMES vulkan vulkan-1 REQUIRED)
target_link_libraries(rex_dlss5_amd PUBLIC "${REXGLUE_VULKAN_LOADER}")
target_compile_features(rex_dlss5_amd PRIVATE cxx_std_17)
set_source_files_properties("${_src}/linux/src/core/nr_runtime.cpp"
    PROPERTIES COMPILE_DEFINITIONS "${_dlss5_defines}")
add_dependencies(rex_dlss5_amd rexglue_dlss5_amd_data)

option(REXGLUE_DLSS5_AMD_TOOL "Build the nr_probe one-shot NR tool" ON)
if(REXGLUE_DLSS5_AMD_TOOL)
    add_executable(nr_probe "${PROJECT_SOURCE_DIR}/tools/nr_probe/main.cpp")
    target_include_directories(nr_probe PRIVATE "${_src}/linux/src/core")
    target_link_libraries(nr_probe PRIVATE rex_dlss5_amd Vulkan::Headers)
    add_dependencies(nr_probe rexglue_dlss5_amd_data)
endif()

set(REXGLUE_DLSS5_AMD_INCLUDE_DIR "${_src}/linux/src/core" CACHE INTERNAL
    "DLSS 5 AMD runtime headers")
set(REXGLUE_DLSS5_AMD_STAGE_DIR "${_stage_dir}" CACHE INTERNAL
    "DLSS 5 AMD staged shader/notice tree")
set(REXGLUE_DLSS5_AMD_SOURCE "${_src}" CACHE INTERNAL "DLSS 5 AMD pinned source")

message(STATUS "DLSS 5 AMD: enabled (source ${_src}, staged ${_stage_dir})")
