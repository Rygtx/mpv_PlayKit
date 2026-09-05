cmake_minimum_required(VERSION 4.0)
project(vs_dlssnr LANGUAGES CXX)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "vs_dlssnr: x64 only")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies")
if (NOT EXISTS "${DEP_DIR}/ngx/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NGX SDK missing: run scripts/fetch-deps.ps1 (see scripts/) to populate dependencies/ngx")
endif ()
if (NOT EXISTS "${DEP_DIR}/vapoursynth/include/VapourSynth4.h")
    message(FATAL_ERROR "VapourSynth headers missing: run scripts/fetch-deps.ps1 to populate dependencies/vapoursynth")
endif ()

add_library(vs_dlssnr SHARED
    src/plugin.cpp
    src/dlssnr_context.cpp
    src/iat_hook.cpp
    src/d3d12_context.cpp
    src/bridge.cpp
)

target_include_directories(vs_dlssnr PRIVATE
    src
    "${DEP_DIR}/ngx/include"
    "${DEP_DIR}/vapoursynth/include"
)

# Static NGX core (Init_with_ProjectID / AllocateParameters), Magpie-style layout
target_link_libraries(vs_dlssnr PRIVATE
    "${DEP_DIR}/ngx/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
    d3d12.lib dxgi.lib d3d11.lib dxguid.lib d3dcompiler.lib
)

# --- Independent control panel: ImGui + D3D11, talks to vs_dlssnr.dll via
#     two named shared-memory mappings (panel_ipc.h: params + stats) and
#     dlssnr_ui.ini (saved defaults) ---
set(IMGUI_DIR "${DEP_DIR}/imgui")
add_executable(dlssnr_panel WIN32
    src/panel_app.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)
target_include_directories(dlssnr_panel PRIVATE src "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(dlssnr_panel PRIVATE d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib)
set_property(TARGET dlssnr_panel PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(dlssnr_panel PRIVATE UNICODE _UNICODE)
target_compile_options(dlssnr_panel PRIVATE /utf-8)
target_link_options(dlssnr_panel PRIVATE /DEBUG:FULL)
set_target_properties(dlssnr_panel PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin" PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

# mpv-lazy ships an old vcruntime140.dll that preempts System32 -> static CRT required
set_property(TARGET vs_dlssnr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(vs_dlssnr PRIVATE UNICODE _UNICODE)
target_compile_options(vs_dlssnr PRIVATE /utf-8)
target_link_options(vs_dlssnr PRIVATE /DEBUG:FULL)
set_target_properties(vs_dlssnr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    PREFIX ""
)
