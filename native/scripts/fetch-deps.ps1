# 从公开源落地 native/ 依赖:NGX SDK(官方 NVIDIA/DLSS 仓库)+ VapourSynth R73 头
# 用法: powershell -File scripts\fetch-deps.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot   # native/
$ngxInc = Join-Path $root "dependencies\ngx\include"
$ngxLib = Join-Path $root "dependencies\ngx\lib\Windows_x86_64\x64"
$vsInc  = Join-Path $root "dependencies\vapoursynth\include"
New-Item -ItemType Directory -Force $ngxInc, $ngxLib, $vsInc | Out-Null

function Fetch([string]$url, [string]$dst) {
    if (Test-Path $dst) { return }
    # --fail: never persist an HTTP error body (a 404 page would pass the
    # non-empty check below and poison the dependency cache permanently)
    & curl.exe -sSL --fail --retry 8 --retry-all-errors --retry-delay 2 -o $dst $url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $url (curl exit $LASTEXITCODE)" }
    if (-not (Test-Path $dst) -or (Get-Item $dst).Length -eq 0) { throw "download failed: $url" }
    Write-Host "fetched: $dst ($((Get-Item $dst).Length) bytes)"
}

# --- NGX SDK headers (github.com/NVIDIA/DLSS, main @ a291cc7d2cc6) ---
$ngxHeaders = @(
    "nvsdk_ngx.h", "nvsdk_ngx_defs.h", "nvsdk_ngx_defs_dlssd.h", "nvsdk_ngx_defs_dlssg.h",
    "nvsdk_ngx_defs_vk.h", "nvsdk_ngx_helpers.h", "nvsdk_ngx_helpers_dlssd.h",
    "nvsdk_ngx_helpers_dlssd_cuda.h", "nvsdk_ngx_helpers_dlssd_vk.h", "nvsdk_ngx_helpers_dlssg.h",
    "nvsdk_ngx_helpers_dlssg_vk.h", "nvsdk_ngx_helpers_vk.h", "nvsdk_ngx_params.h",
    "nvsdk_ngx_params_dlssd.h", "nvsdk_ngx_params_dlssg.h", "nvsdk_ngx_vk.h"
)
foreach ($h in $ngxHeaders) {
    Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/main/include/$h" (Join-Path $ngxInc $h)
}

# --- NGX static core lib (layout mirrors Magpie BuildOptions.props DLSSSdkDir) ---
Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/main/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" (Join-Path $ngxLib "nvsdk_ngx_s.lib")

# --- Official signed NGX FG runtime (PORTING #8 官方帧生成后端) ---
# 落到 vendor\ngx\ 与模型 DLL 同目录,打包脚本按存在与否选装;插件只在驱动
# 报告 DLSSG 能力(RTX 40/50)时经共享 NGX core 加载,否则回落 dlssg_for_sm86
# proxy。尺寸门槛:HTML 错误页 / LFS 指针文件能通过非空检查,必须拦下。
$fgOfficial = Join-Path $root "vendor\ngx\nvngx_dlssg.dll"
if (-not (Test-Path $fgOfficial)) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $fgOfficial) | Out-Null
    $tmp = "$fgOfficial.download"
    Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/main/lib/Windows_x86_64/rel/nvngx_dlssg.dll" $tmp
    $len = (Get-Item $tmp).Length
    if ($len -lt 1MB) { Remove-Item $tmp -Force; throw "nvngx_dlssg.dll size sanity failed ($len bytes)" }
    Move-Item $tmp $fgOfficial -Force
    Write-Host "official DLSSG runtime: $fgOfficial ($len bytes)"
}

# --- VapourSynth R73 headers (runtime is R73 / API4, see mpv-lazy Lib\site-packages\vapoursynth-73.dist-info) ---
foreach ($h in @("VapourSynth4.h", "VSHelper4.h", "VSScript4.h")) {
    Fetch "https://raw.githubusercontent.com/VapourSynth/VapourSynth/R73/include/$h" (Join-Path $vsInc $h)
}

# --- NVOF headers(清单 #6 光流;官方 OpticalFlowSDK 仓库已从 GitHub 撤下,
#     mbucchia/Optical-Flow-SDK 是完整官方镜像,NvOFInterface 即 SDK 头目录)---
$nvofDir = Join-Path $root "vendor\nvof"
New-Item -ItemType Directory -Force $nvofDir | Out-Null
foreach ($h in @("nvOpticalFlowCommon.h", "nvOpticalFlowD3D12.h", "nvOpticalFlowD3D11.h", "nvOpticalFlowCuda.h")) {
    Fetch "https://raw.githubusercontent.com/mbucchia/Optical-Flow-SDK/main/NvOFInterface/$h" (Join-Path $nvofDir $h)
}

# --- Dear ImGui (independent panel UI), unpacked to dependencies\imgui ---
$imguiDir = Join-Path $root "dependencies\imgui"
if (-not (Test-Path (Join-Path $imguiDir "imgui.h"))) {
    $zip = Join-Path $env:TEMP "imgui.zip"
    Fetch "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip" $zip
    $extract = Join-Path $root "dependencies"
    Expand-Archive $zip $extract -Force
    if (Test-Path $imguiDir) { Remove-Item $imguiDir -Recurse -Force -Confirm:$false }
    Rename-Item (Join-Path $extract "imgui-1.91.9b") "imgui"
    Remove-Item $zip -Force -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "imgui: unpacked to $imguiDir"
}

Write-Host "done."
