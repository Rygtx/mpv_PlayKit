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
    & curl.exe -sSL --retry 8 --retry-all-errors --retry-delay 2 -o $dst $url
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

# --- VapourSynth R73 headers (runtime is R73 / API4, see mpv-lazy Lib\site-packages\vapoursynth-73.dist-info) ---
foreach ($h in @("VapourSynth4.h", "VSHelper4.h", "VSScript4.h")) {
    Fetch "https://raw.githubusercontent.com/VapourSynth/VapourSynth/R73/include/$h" (Join-Path $vsInc $h)
}

Write-Host "done."
