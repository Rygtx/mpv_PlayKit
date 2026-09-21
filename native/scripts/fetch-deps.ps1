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
# 落到 vendor\ngx\ 与模型 DLL 同目录,打包脚本按存在与否选装;官方链是 FG
# 唯一路径,RTX 30/20 由 dlssg_for_sm86 0.3.x hook 代理接管交付(自动档
# 预载)。尺寸门槛:HTML 错误页 / LFS 指针文件能通过非空检查,必须拦下。
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

# --- dlssg_for_sm86 FG hook proxy(RTX 30/20 DLSS-G 接管层;上游仓库直取)---
# version.dll 与出厂 dlssg_sm86.ini 都在上游仓库根(普通 git blob,非 LFS),
# codeload tag 归档直下解出。tag 与 dll 尺寸双钉:升级上游时同步改两处;
# 既有文件尺寸不符(含手工放置的旧版)一律重取覆盖,vendor 是可重建暂存区,
# 不承载手工修改(出厂 ini 同理,部署侧要改请改部署副本)。
$fgProxyDll = Join-Path $root "vendor\ngx\version.dll"
$fgProxyIni = Join-Path $root "vendor\ngx\dlssg_sm86.ini"
$sm86Tag = "0.3.5"
$sm86DllBytes = 30021920
if (-not (Test-Path $fgProxyDll) -or (Get-Item $fgProxyDll).Length -ne $sm86DllBytes) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $fgProxyDll) | Out-Null
    $zip = Join-Path $env:TEMP "dlssg_for_sm86-$sm86Tag.zip"
    Fetch "https://codeload.github.com/sdli1995/dlssg_for_sm86/zip/refs/tags/$sm86Tag" $zip
    $extract = Join-Path $env:TEMP "sm86-extract"
    if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
    New-Item -ItemType Directory -Force $extract | Out-Null
    Expand-Archive $zip $extract -Force
    $repoRootDir = Get-ChildItem $extract -Directory | Select-Object -First 1
    if (-not $repoRootDir) { throw "dlssg_for_sm86 zip layout unexpected" }
    $dllSrc = Join-Path $repoRootDir.FullName "version.dll"
    $iniSrc = Join-Path $repoRootDir.FullName "dlssg_sm86.ini"
    if (-not (Test-Path $dllSrc)) { throw "dlssg_for_sm86 zip: version.dll missing" }
    if ((Get-Item $dllSrc).Length -ne $sm86DllBytes) { throw "dlssg_for_sm86 zip: version.dll size mismatch (tag/binary drift; update sm86Tag/sm86DllBytes)" }
    if (-not (Test-Path $iniSrc)) { throw "dlssg_for_sm86 zip: dlssg_sm86.ini missing" }
    Copy-Item $dllSrc $fgProxyDll -Force
    Copy-Item $iniSrc $fgProxyIni -Force
    Remove-Item $extract -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    Write-Host "dlssg_for_sm86 ${sm86Tag}: $fgProxyDll ($sm86DllBytes bytes) + factory ini"
}

# 出厂 ini 的 MaxGeneratedFrames 默认 3(4X);部署默认开到 5(6X)——
# 上限只是钳位,倍数由插件按面板请求,常开无副作用,免去"首次切 5x/6x
# 需重启"的过渡态。仅裁剪部署副本,vendor 是可重建暂存区。
if (Test-Path $fgProxyIni) {
    $iniText = Get-Content $fgProxyIni -Raw
    if ($iniText -notmatch '(?m)^MaxGeneratedFrames=5\s*$') {
        $iniText = $iniText -replace '(?m)^MaxGeneratedFrames=\d+\s*$', "MaxGeneratedFrames=5"
        Set-Content -Path $fgProxyIni -Value $iniText -NoNewline -Encoding utf8NoBOM
        Write-Host "fg proxy ini: MaxGeneratedFrames -> 5 (6X cap)"
    }
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

# --- FidelityFX SDK v2.3.0(AMD 光流后端 FxofContext;裁剪子集,vendor 不入库
#     可重建,仿 vendor/nvof 惯例)。API 面与 Magpie AmdOpticalFlowProvider
#     所用一致(ffx_api 新 C 接口,FSR3 3.x 时代):下载 → 解压 → 裁剪 →
#     断言(头文件 API 符号 + 7 个 pass shader 齐全),任何一步失败即 throw,
#     防止半成品 vendor 毒化构建。---
$ffxDir = Join-Path $root "vendor\fidelityfx"
if (-not (Test-Path (Join-Path $ffxDir "api\include\ffx_api.h"))) {
    $zip = Join-Path $env:TEMP "FidelityFX-SDK-v2.3.0.zip"
    Fetch "https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/zip/refs/tags/v2.3.0" $zip
    $extract = Join-Path $env:TEMP "ffx-sdk-extract"
    if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
    New-Item -ItemType Directory -Force $extract | Out-Null
    Expand-Archive $zip $extract -Force
    $sdkRoot = Get-ChildItem $extract -Directory | Select-Object -First 1
    if (-not $sdkRoot) { throw "FidelityFX SDK zip layout unexpected" }
    $kit = Join-Path $sdkRoot.FullName "Kits\FidelityFX"
    # 裁剪:只留 OF 编译闭包(api 头/内部实现+gpu 头、dx12 backend、fsr3 OF
    # 头/gpu 头/OF 源码与 shader)。frameinterpolation 等无关组件不进 vendor。
    $map = @(
        @{ src = "api\include";  dst = "api\include" },
        @{ src = "api\internal"; dst = "api\internal" },
        @{ src = "backend\dx12"; dst = "backend\dx12" },
        @{ src = "framegeneration\fsr3\include"; dst = "framegeneration\fsr3\include" }
    )
    foreach ($m in $map) {
        $from = Join-Path $kit $m.src
        $to = Join-Path $ffxDir $m.dst
        if (-not (Test-Path $from)) { throw "FidelityFX SDK trim: missing $m.src" }
        New-Item -ItemType Directory -Force $to | Out-Null
        Copy-Item "$from\*" $to -Recurse -Force
    }
    foreach ($f in @("ffx_opticalflow.cpp", "ffx_opticalflow_private.h",
                     "ffx_opticalflow_shaderblobs.cpp", "ffx_opticalflow_shaderblobs.h")) {
        $from = Join-Path $kit "framegeneration\fsr3\internal\$f"
        if (-not (Test-Path $from)) { throw "FidelityFX SDK trim: missing $f" }
        $to = Join-Path $ffxDir "framegeneration\fsr3\internal"
        New-Item -ItemType Directory -Force $to | Out-Null
        Copy-Item $from $to -Force
    }
    $shaderDst = Join-Path $ffxDir "framegeneration\fsr3\internal\shaders"
    New-Item -ItemType Directory -Force $shaderDst | Out-Null
    Copy-Item (Join-Path $kit "framegeneration\fsr3\internal\shaders\ffx_opticalflow_*.hlsl") $shaderDst -Force
    # 仓库根无 LICENSE 文件,MIT 许可文本与第三方声明都在 3rdpartynotice.md。
    $notice = Join-Path $sdkRoot.FullName "3rdpartynotice.md"
    if (Test-Path $notice) { Copy-Item $notice $ffxDir -Force }
    Remove-Item $extract -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    # 断言 1:API 面(Magpie AmdOpticalFlowProvider 依赖的符号逐个在位)。
    $ofHeader = Join-Path $ffxDir "framegeneration\fsr3\include\ffx_opticalflow.h"
    $apiText = Get-Content $ofHeader -Raw
    foreach ($sym in @("FfxOpticalflowContextDescription", "FFX_OPTICALFLOW_CONTEXT_COUNT",
                       "ffxOpticalflowGetSharedResourceDescriptions", "ffxOpticalflowContextDispatch",
                       "backbufferTransferFunction", "minMaxLuminance")) {
        if ($apiText -notmatch [regex]::Escape($sym)) { throw "FidelityFX SDK assert: $sym missing from ffx_opticalflow.h" }
    }
    # 断言 2:7 个 pass shader(Generate 脚本硬编码清单,缺一即显式失败)。
    $passes = @(
        "ffx_opticalflow_compute_luminance_pyramid_pass",
        "ffx_opticalflow_compute_optical_flow_advanced_pass_v5",
        "ffx_opticalflow_compute_scd_divergence_pass",
        "ffx_opticalflow_filter_optical_flow_pass_v5",
        "ffx_opticalflow_generate_scd_histogram_pass",
        "ffx_opticalflow_prepare_luma_pass",
        "ffx_opticalflow_scale_optical_flow_advanced_pass_v5"
    )
    foreach ($p in $passes) {
        if (-not (Test-Path (Join-Path $shaderDst "$p.hlsl"))) { throw "FidelityFX SDK assert: shader $p.hlsl missing" }
    }
    Write-Host "FidelityFX SDK v2.3.0 trimmed to $ffxDir"
}

# --- pix3.h(FidelityFX 的 ffx_dx12.cpp 无条件 #include;SDK zip 不带。
#     官方发行渠道是 WinPixEventRuntime NuGet 包(MIT),解出头文件链)---
$pixDir = Join-Path $root "vendor\fidelityfx\include"
if (-not (Test-Path (Join-Path $pixDir "pix3.h"))) {
    New-Item -ItemType Directory -Force $pixDir | Out-Null
    $pkg = Join-Path $env:TEMP "WinPixEventRuntime.nupkg"
    Fetch "https://www.nuget.org/api/v2/package/WinPixEventRuntime/" $pkg
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $pkgZip = [System.IO.Compression.ZipFile]::OpenRead($pkg)
    try {
        $entries = $pkgZip.Entries | Where-Object { $_.FullName -like "Include/WinPixEventRuntime/*" }
        if (-not $entries) { throw "WinPixEventRuntime nupkg layout unexpected" }
        foreach ($e in $entries) {
            $dst = Join-Path $pixDir ([System.IO.Path]::GetFileName($e.FullName))
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dst, $true)
        }
    } finally { $pkgZip.Dispose() }
    Remove-Item $pkg -Force -ErrorAction SilentlyContinue
    Write-Host "pix3 headers: $pixDir"
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
