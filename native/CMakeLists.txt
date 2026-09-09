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
# NVOF headers(vendor/nvof,fetch-deps.ps1 从公开镜像拉取)——清单 #6
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvof/nvOpticalFlowD3D12.h")
    message(FATAL_ERROR "NVOF headers missing: run scripts/fetch-deps.ps1 to populate vendor/nvof")
endif ()

add_library(vs_dlssnr SHARED
    src/plugin.cpp
    src/dlssnr_context.cpp
    src/nvof_context.cpp
    src/iat_hook.cpp
    src/d3d12_context.cpp
    src/bridge.cpp
    src/plugin.rc             # 版本信息 (build/version.h 由 CMake configure 注入)
)

target_include_directories(vs_dlssnr PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvof"
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

# 版本号 = configure 当天日期, 对齐 package.ps1 的 vyyyy.MM.dd(零填充+本地时间)。
# 字符串版带 v 前缀(与包名 mpv_PlayKit-dlssnr-vYYYY.MM.DD-full.zip 完全一致);
# 数值版剥前导零(且不可能带 v):rc 预处理按 C 规则把前导 0 当八进制, "09" 直接报错。
# DLL 与面板共用 build/version.h —— 同一次 configure 生成,两产物版本必然成对
string(TIMESTAMP _pv_year "%Y")
string(TIMESTAMP _pv_month "%m")
string(TIMESTAMP _pv_day "%d")
set(PANEL_VERSION_STR "v${_pv_year}.${_pv_month}.${_pv_day}")
string(REGEX REPLACE "^0+" "" _pv_num_month "${_pv_month}")
string(REGEX REPLACE "^0+" "" _pv_num_day "${_pv_day}")
set(PANEL_FILEVERSION "${_pv_year},${_pv_num_month},${_pv_num_day},0")
message(STATUS "vs_dlssnr / dlssnr_panel version: ${PANEL_VERSION_STR}")
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)
# rc.exe 的依赖扫描对 build 目录里的生成头不可靠,显式挂上:
# 跨天后重新 configure 会改写 version.h,此依赖保证两个 rc 重编
set_source_files_properties(src/panel.rc src/plugin.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/version.h"
)

add_executable(dlssnr_panel WIN32
    src/panel_app.cpp
    src/panel.rc              # 图标 + 版本信息 (IDI_PANEL_ICON, ico 由 tools/make_panel_icon.py 生成)
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)
target_include_directories(dlssnr_panel PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}" "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
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
