# rexui

# Core UI sources
set(REXUI_CORE_SOURCES
    graphics_upload_buffer_pool.cpp
    graphics_util.cpp
    image_decode.cpp
    imgui_dialog.cpp
    imgui_drawer.cpp
    immediate_drawer.cpp
    keybinds.cpp
    menu_item.cpp
    presenter.cpp
    renderdoc_api.cpp
    sdl_virtual_key.cpp
    temporal_jitter.cpp
    ffx_runtime.cpp
    xess_runtime.cpp
    dlss_runtime.cpp
    dlss5_amd_runtime.cpp
    window.cpp
    window_sdl.cpp
    windowed_app.cpp
    windowed_app_context.cpp
    windowed_app_context_sdl.cpp
)

# Optional overlay dialogs
set(REXUI_OVERLAY_SOURCES
    overlay/achievement_icon_cache.cpp
    overlay/achievement_toast.cpp
    overlay/achievements_overlay.cpp
    overlay/debug_overlay.cpp
    overlay/console_overlay.cpp
    overlay/console_commands.cpp
    overlay/settings_overlay.cpp
)

# Vulkan UI backend
set(REXUI_VULKAN_SOURCES
    vulkan/linked_type_descriptor_set_allocator.cpp
    vulkan/vulkan_moltenvk.cpp
    vulkan/single_layout_descriptor_set_pool.cpp
    vulkan/spirv_tools_context.cpp
    vulkan/ui_samplers.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_immediate_drawer.cpp
    vulkan/vulkan_instance.cpp
    vulkan/vulkan_mem_alloc.cpp
    vulkan/vulkan_presenter.cpp
    vulkan/vulkan_temporal_inputs.cpp
    vulkan/vulkan_provider.cpp
    vulkan/vulkan_submission_tracker.cpp
    vulkan/vulkan_upload_buffer_pool.cpp
    vulkan/vulkan_util.cpp
)

# D3D12 UI backend
if(REXGLUE_USE_D3D12)
    set(REXUI_D3D12_SOURCES
        d3d12/d3d12_cpu_descriptor_pool.cpp
        d3d12/d3d12_descriptor_heap_pool.cpp
        d3d12/d3d12_immediate_drawer.cpp
        d3d12/d3d12_presenter.cpp
        d3d12/d3d12_temporal_inputs.cpp
        d3d12/d3d12_provider.cpp
        d3d12/d3d12_submission_tracker.cpp
        d3d12/d3d12_upload_buffer_pool.cpp
        d3d12/d3d12_util.cpp
    )
endif()

# Platform-specific sources (windowing itself is SDL3, platform-independent)
if(WIN32)
    set(REXUI_PLATFORM_SOURCES
        surface_win.cpp
    )
elseif(APPLE)
    set(REXUI_PLATFORM_SOURCES
        surface_mac.cpp
    )
else()
    set(REXUI_PLATFORM_SOURCES
        surface_gnulinux.cpp
    )
endif()

# Combine sources
set(REXUI_SOURCES
    ${REXUI_CORE_SOURCES}
    ${REXUI_OVERLAY_SOURCES}
    ${REXUI_PLATFORM_SOURCES}
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXUI_SOURCES ${REXUI_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXUI_SOURCES ${REXUI_D3D12_SOURCES})
endif()

add_library(rexui OBJECT ${REXUI_SOURCES})
add_library(rex::ui ALIAS rexui)

rex_target_pch(rexui <rex/math.h>)

target_include_directories(rexui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexui PRIVATE
    ${PROJECT_BINARY_DIR}/include  # Generated macOS Vulkan path data
    ${REXGLUE_ROOT}/src  # For ui/shaders/bytecode includes
    ${REXGLUE_ROOT}/thirdparty/stb  # stb_image.h for icon decoding
)

target_link_libraries(rexui PUBLIC
    rexcore
    renderdoc
    imgui::imgui
    SDL3::SDL3
)
target_link_libraries(rexui PRIVATE utf8cpp)

if(REXGLUE_FIDELITYFX_SOURCE_DIR AND EXISTS "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include")
    target_include_directories(rexui PRIVATE
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include"
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/ffx-api/include"
    )
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_SDK=1)
endif()

if(REXGLUE_ENABLE_XESS AND REXGLUE_XESS_INCLUDE_DIR)
    target_include_directories(rexui PRIVATE "${REXGLUE_XESS_INCLUDE_DIR}")
    target_compile_definitions(rexui PRIVATE REX_HAS_XESS=1)
endif()

if(REXGLUE_ENABLE_DLSS AND REXGLUE_DLSS_LIBRARY)
    target_include_directories(rexui PRIVATE "${REXGLUE_DLSS_INCLUDE_DIR}")
    target_compile_definitions(rexui PRIVATE REX_HAS_DLSS=1)
    target_link_libraries(rexui PRIVATE "${REXGLUE_DLSS_LIBRARY}")
    if(UNIX)
        target_link_libraries(rexui PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

set(REXUI_HAS_FIDELITYFX_RUNTIME OFF)
if(REXGLUE_FIDELITYFX_PREBUILT AND REXGLUE_FIDELITYFX_PREBUILT_INCLUDE_DIR)
    # Ahead of the 1.1.4 ffx-api/include, which has the same <ffx_api/...> names.
    target_include_directories(rexui BEFORE PRIVATE "${REXGLUE_FIDELITYFX_PREBUILT_INCLUDE_DIR}")
    target_compile_definitions(rexui PRIVATE REX_FIDELITYFX_PREBUILT=1)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(TARGET amd_fidelityfx_vk)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_vk)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(TARGET amd_fidelityfx_dx12)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_dx12)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(REXUI_HAS_FIDELITYFX_RUNTIME)
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_RUNTIME=1)
endif()

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexui PUBLIC
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
    )
endif()

if(TARGET rex::dlss5_amd)
    set_source_files_properties(dlss5_amd_runtime.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${REXGLUE_DLSS5_AMD_INCLUDE_DIR}")
    target_link_libraries(rexui PRIVATE rex::dlss5_amd)
    target_compile_definitions(rexui PRIVATE REX_HAS_DLSS5_AMD=1)
endif()

# Platform-specific dependencies
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    # Required, not optional: vulkan/api.h pulls in <wayland-client.h> on every
    # Linux build.
    pkg_check_modules(WAYLAND REQUIRED wayland-client)

    target_include_directories(rexui PUBLIC
        ${X11_XCB_INCLUDE_DIRS}
        ${WAYLAND_INCLUDE_DIRS}
    )
    target_link_libraries(rexui PUBLIC
        ${X11_XCB_LIBRARIES}
        ${WAYLAND_LIBRARIES}
    )
elseif(APPLE)
    target_link_libraries(rexui PRIVATE "-framework CoreFoundation")
endif()

if(WIN32)
    target_link_libraries(rexui PUBLIC dxgi dxguid)
endif()

# D3D12 backend dependencies
if(REXGLUE_USE_D3D12)
    target_link_libraries(rexui PUBLIC dxc-headers)
endif()

