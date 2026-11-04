# OpenDLSS-NR (MIT) — standalone dlss5vk runner for the DLSS 5 NR network, Linux/Vulkan.
#
# maanHimself/OpenDLSS-NR reimplements the DLSS 5 NR network (71 blocks, 310.8.0-class) in
# Vulkan/GLSL compute. Upstream builds with PowerShell/MSVC only, so this module fetches the
# pinned commit, applies the carried Linux patch (cmake/patches/opendls_nr_linux.patch) to a
# build-dir copy of src/, compiles the 13 shaders/*.comp with glslang and builds the standalone
# `dlss5vk` tool. The tool loads the Vulkan loader at run time through volk, so it links no
# libvulkan and shares no device with the application.
#
# Produces:
#   dlss5vk                    executable at <build>/opendls-nr/dlss5vk
#   rexglue_opendls_nr_data    staged tree <build>/opendls-nr/{shaders,LICENSE.txt,NOTICE}
#                              (the 13 .spv sit next to the binary: Kernels loads
#                               <exe dir>/shaders unless --shaders is given)
#   REXGLUE_OPENDLS_NR_SOURCE / REXGLUE_OPENDLS_NR_PATCHED_SOURCE / REXGLUE_OPENDLS_NR_STAGE_DIR
#
# The NVIDIA model weights are never part of this tree; dlss5vk loads a user-supplied model
# directory at run time (--model). Nothing from this module is packaged.

option(REXGLUE_ENABLE_OPENDLS_NR
       "OpenDLSS-NR standalone dlss5vk tool (Linux x86-64, Vulkan; GLSL-only)" OFF)
if(NOT REXGLUE_ENABLE_OPENDLS_NR)
    return()
endif()

if(NOT UNIX OR APPLE)
    message(STATUS "OpenDLSS-NR: Linux/Vulkan only; disabled")
    set(REXGLUE_ENABLE_OPENDLS_NR OFF CACHE BOOL "" FORCE)
    return()
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(STATUS "OpenDLSS-NR: the pinned tool is x86-64 only; disabled")
    set(REXGLUE_ENABLE_OPENDLS_NR OFF CACHE BOOL "" FORCE)
    return()
endif()
if(NOT MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(STATUS "OpenDLSS-NR: unsupported compiler ${CMAKE_CXX_COMPILER_ID}; disabled")
    set(REXGLUE_ENABLE_OPENDLS_NR OFF CACHE BOOL "" FORCE)
    return()
endif()
if(NOT REXGLUE_USE_VULKAN)
    message(FATAL_ERROR "REXGLUE_ENABLE_OPENDLS_NR requires REXGLUE_USE_VULKAN")
endif()

set(_rexglue_opendls_nr_pin "9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1")

# An explicit tree (a developer checkout, or a CI cache) takes precedence over a network
# fetch. It must be pristine at the pin: the carried patch is applied to a build-dir copy,
# never to this tree.
set(REXGLUE_OPENDLS_NR_SOURCE_DIR "" CACHE PATH
    "Pinned OpenDLSS-NR checkout; empty fetches the pinned commit at configure time")
if(REXGLUE_OPENDLS_NR_SOURCE_DIR)
    set(_src "${REXGLUE_OPENDLS_NR_SOURCE_DIR}")
    if(NOT EXISTS "${_src}/src/vk_context.cpp" OR NOT EXISTS "${_src}/shaders")
        message(FATAL_ERROR
            "REXGLUE_OPENDLS_NR_SOURCE_DIR=${_src} is not an OpenDLSS-NR tree")
    endif()
else()
    include(FetchContent)
    FetchContent_Declare(rexglue_opendls_nr
        GIT_REPOSITORY https://github.com/maanHimself/OpenDLSS-NR.git
        GIT_TAG        ${_rexglue_opendls_nr_pin}
        GIT_SHALLOW    FALSE
        GIT_PROGRESS   TRUE)
    FetchContent_GetProperties(rexglue_opendls_nr)
    if(NOT rexglue_opendls_nr_POPULATED)
        FetchContent_Populate(rexglue_opendls_nr)
    endif()
    set(_src "${rexglue_opendls_nr_SOURCE_DIR}")
endif()

execute_process(COMMAND git -C "${_src}" rev-parse HEAD
    OUTPUT_VARIABLE _opendls_got OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _opendls_got STREQUAL _rexglue_opendls_nr_pin)
    message(FATAL_ERROR
        "OpenDLSS-NR: source is '${_opendls_got}', expected ${_rexglue_opendls_nr_pin}")
endif()

# Dependencies: volk (dlopens the Vulkan loader at run time) and the Vulkan headers, both
# pinned; glslang compiles the 13 compute shaders (system glslangValidator preferred, the
# pinned 16.6.0 release tarball otherwise — 16.6.0 reproduces the upstream SPIR-V).
include(FetchContent)
FetchContent_Declare(rexglue_opendls_volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.4.357.0
    GIT_SHALLOW    FALSE)
FetchContent_Declare(rexglue_opendls_vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.4.363
    GIT_SHALLOW    FALSE)
foreach(_dep IN ITEMS rexglue_opendls_volk rexglue_opendls_vulkan_headers)
    FetchContent_GetProperties(${_dep})
    if(NOT ${_dep}_POPULATED)
        FetchContent_Populate(${_dep})
    endif()
endforeach()
set(_volk_src "${rexglue_opendls_volk_SOURCE_DIR}")
set(_vkheaders_src "${rexglue_opendls_vulkan_headers_SOURCE_DIR}")

find_program(REXGLUE_OPENDLS_GLSLANG glslangValidator)
if(REXGLUE_OPENDLS_GLSLANG)
    set(_glslang "${REXGLUE_OPENDLS_GLSLANG}")
    execute_process(COMMAND "${_glslang}" --version
        OUTPUT_VARIABLE _glslang_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _glslang_version MATCHES "16\\.6\\.0")
        message(WARNING
            "OpenDLSS-NR: system glslangValidator is not 16.6.0 (${_glslang_version}); "
            "the pinned shaders may not compile identically. Install glslang 16.6.0 or let "
            "the module download the pinned tarball (remove the cached "
            "REXGLUE_OPENDLS_GLSLANG to force the download).")
    endif()
else()
    set(_glslang_ver "16.6.0")
    set(_glslang_archive "${CMAKE_BINARY_DIR}/_deps/opendls-nr-glslang-${_glslang_ver}.tar.gz")
    file(DOWNLOAD
        "https://github.com/KhronosGroup/glslang/releases/download/${_glslang_ver}/glslang-${_glslang_ver}-linux-x86_64-release.tar.gz"
        "${_glslang_archive}"
        EXPECTED_HASH SHA256=2c34071f56ecf39d16233294d3739cf43f981fd39080f2fd9dccb782101191ce
        TLS_VERIFY ON STATUS _glslang_status)
    list(GET _glslang_status 0 _glslang_code)
    if(NOT _glslang_code EQUAL 0)
        message(FATAL_ERROR
            "OpenDLSS-NR: no glslangValidator on PATH and cannot download glslang ${_glslang_ver}: ${_glslang_status}")
    endif()
    set(_glslang_root "${CMAKE_BINARY_DIR}/_deps/opendls-nr-glslang")
    file(MAKE_DIRECTORY "${_glslang_root}")
    file(ARCHIVE_EXTRACT INPUT "${_glslang_archive}" DESTINATION "${_glslang_root}")
    set(_glslang "${_glslang_root}/bin/glslangValidator")
    if(NOT EXISTS "${_glslang}")
        message(FATAL_ERROR
            "OpenDLSS-NR: glslang ${_glslang_ver} tarball did not provide bin/glslangValidator")
    endif()
endif()

# Carried Linux patch on a build-dir copy (the pin check above must keep passing on the
# pristine fetch, so patch the copy). `patch -p1` is used because the build tree sits inside
# this repository's work tree, where `git apply` would anchor at the wrong root and silently
# skip every file.
set(_patched_root "${CMAKE_BINARY_DIR}/opendls-nr-patched")
file(REMOVE_RECURSE "${_patched_root}")
file(COPY "${_src}/src" DESTINATION "${_patched_root}")   # -> ${_patched_root}/src
set(_patch_file "${CMAKE_CURRENT_LIST_DIR}/patches/opendls_nr_linux.patch")
find_program(REXGLUE_PATCH_EXECUTABLE patch)
if(NOT REXGLUE_PATCH_EXECUTABLE)
    message(FATAL_ERROR "OpenDLSS-NR: `patch` not found; cannot apply the carried Linux patch")
endif()
execute_process(
    COMMAND "${REXGLUE_PATCH_EXECUTABLE}" -p1 --no-backup-if-mismatch -i "${_patch_file}"
    WORKING_DIRECTORY "${_patched_root}"
    RESULT_VARIABLE _patch_result OUTPUT_VARIABLE _patch_out ERROR_VARIABLE _patch_err)
if(NOT _patch_result EQUAL 0)
    message(FATAL_ERROR
        "OpenDLSS-NR: carried Linux patch does not apply to ${_rexglue_opendls_nr_pin}: ${_patch_err}")
endif()
file(READ "${_patched_root}/src/kernels.h" _patched_kernels_h)
if(NOT _patched_kernels_h MATCHES "OPEN_DLSSNR_PTX_ROUTE")
    message(FATAL_ERROR
        "OpenDLSS-NR: patch reported success but the marker is missing; pin drift?")
endif()

# The staged tool tree: the executable, the 13 .spv next to it and the license files.
set(_stage_dir "${CMAKE_BINARY_DIR}/opendls-nr")

file(GLOB _opendls_shaders CONFIGURE_DEPENDS "${_src}/shaders/*.comp")
file(GLOB _opendls_glsl CONFIGURE_DEPENDS "${_src}/shaders/*.glsl")
list(SORT _opendls_shaders)
list(LENGTH _opendls_shaders _opendls_shader_count)
if(NOT _opendls_shader_count EQUAL 13)
    message(FATAL_ERROR
        "OpenDLSS-NR: expected 13 shaders/*.comp at the pin, found ${_opendls_shader_count}")
endif()
set(_spv_files "")
foreach(_comp IN LISTS _opendls_shaders)
    get_filename_component(_name "${_comp}" NAME_WE)
    set(_spv "${_stage_dir}/shaders/${_name}.spv")
    add_custom_command(OUTPUT "${_spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_dir}/shaders"
        COMMAND "${_glslang}" -V --target-env vulkan1.3 -I"${_src}/shaders" "${_comp}" -o "${_spv}"
        DEPENDS "${_comp}" ${_opendls_glsl}
        COMMENT "OpenDLSS-NR: glslang ${_name}.comp"
        VERBATIM)
    list(APPEND _spv_files "${_spv}")
endforeach()

add_custom_command(
    OUTPUT "${_stage_dir}/.stamp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}/LICENSE" "${_stage_dir}/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}/NOTICE" "${_stage_dir}/NOTICE"
    COMMAND ${CMAKE_COMMAND} -E touch "${_stage_dir}/.stamp"
    DEPENDS ${_spv_files} "${_src}/LICENSE" "${_src}/NOTICE")
add_custom_target(rexglue_opendls_nr_data ALL DEPENDS "${_stage_dir}/.stamp")

file(GLOB _opendls_sources CONFIGURE_DEPENDS "${_patched_root}/src/*.cpp")
add_executable(dlss5vk ${_opendls_sources} "${_volk_src}/volk.c")
# The tool's own output directory: Kernels loads <exe dir>/shaders unless --shaders is given.
set_target_properties(dlss5vk PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${_stage_dir}")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(TOUPPER "${_cfg}" _cfg_upper)
    set_target_properties(dlss5vk PROPERTIES "RUNTIME_OUTPUT_DIRECTORY_${_cfg_upper}" "${_stage_dir}")
endforeach()
target_include_directories(dlss5vk PRIVATE
    "${_patched_root}/src"
    "${_volk_src}"
    "${_vkheaders_src}/include")
# VK_NO_PROTOTYPES: volk provides the entry points, the loader is dlopened, libvulkan is not
# linked. VK_ENABLE_BETA_EXTENSIONS: the NV cooperative-matrix-2 / cuda-kernel-launch structs
# the sources reference. OPEN_DLSSNR_GLSL_ONLY: PTX route compiled out (see the patch).
target_compile_definitions(dlss5vk PRIVATE
    VK_NO_PROTOTYPES
    VK_ENABLE_BETA_EXTENSIONS
    OPEN_DLSSNR_GLSL_ONLY)
find_package(Threads REQUIRED)
target_link_libraries(dlss5vk PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
add_dependencies(dlss5vk rexglue_opendls_nr_data)

set(REXGLUE_OPENDLS_NR_SOURCE "${_src}" CACHE INTERNAL "OpenDLSS-NR pinned source")
set(REXGLUE_OPENDLS_NR_PATCHED_SOURCE "${_patched_root}" CACHE INTERNAL
    "OpenDLSS-NR source copy with the carried Linux patch applied")
set(REXGLUE_OPENDLS_NR_STAGE_DIR "${_stage_dir}" CACHE INTERNAL
    "OpenDLSS-NR staged tool tree (dlss5vk, shaders/*.spv, LICENSE.txt, NOTICE)")

message(STATUS
    "OpenDLSS-NR: enabled (source ${_src}, patched ${_patched_root}, staged ${_stage_dir}, glslang ${_glslang})")
