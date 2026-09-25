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
    # 断连防线(2026-09-25):先落临时名,成功后原子 Move —— curl 中途断连
    # 留下的截断文件不会再让 Test-Path 短路而永久毒化依赖缓存。
    $tmp = "$dst.download"
    & curl.exe -sSL --fail --retry 8 --retry-all-errors --retry-delay 2 -o $tmp $url
    if ($LASTEXITCODE -ne 0) {
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
        throw "download failed: $url (curl exit $LASTEXITCODE)"
    }
    if (-not (Test-Path $tmp) -or (Get-Item $tmp).Length -eq 0) { throw "download failed: $url" }
    Move-Item $tmp $dst -Force
    Write-Host "fetched: $dst ($((Get-Item $dst).Length) bytes)"
}

# --- NGX SDK headers (github.com/NVIDIA/DLSS,commit 钉死 2026-09-25:
#     此前走 /main 未钉 —— 上游 main 漂移会静默进构建;与同脚本 RTX SDK
#     的 SHA-256 钉死、sm86 的 tag+尺寸双钉对齐)---
$ngxCommit = "a291cc7d2cc6"
$ngxHeaders = @(
    "nvsdk_ngx.h", "nvsdk_ngx_defs.h", "nvsdk_ngx_defs_dlssd.h", "nvsdk_ngx_defs_dlssg.h",
    "nvsdk_ngx_defs_vk.h", "nvsdk_ngx_helpers.h", "nvsdk_ngx_helpers_dlssd.h",
    "nvsdk_ngx_helpers_dlssd_cuda.h", "nvsdk_ngx_helpers_dlssd_vk.h", "nvsdk_ngx_helpers_dlssg.h",
    "nvsdk_ngx_helpers_dlssg_vk.h", "nvsdk_ngx_helpers_vk.h", "nvsdk_ngx_params.h",
    "nvsdk_ngx_params_dlssd.h", "nvsdk_ngx_params_dlssg.h", "nvsdk_ngx_vk.h"
)
foreach ($h in $ngxHeaders) {
    Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/$ngxCommit/include/$h" (Join-Path $ngxInc $h)
}

# --- NGX static core lib (layout mirrors Magpie BuildOptions.props DLSSSdkDir) ---
# COFF 归档魔数断言:HTML 错误页/LFS 指针文件拦下(同类防线见下文 dll)。
$ngxLibPath = Join-Path $ngxLib "nvsdk_ngx_s.lib"
Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/$ngxCommit/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" $ngxLibPath
$libHead = [System.IO.File]::ReadAllBytes($ngxLibPath)[0..7]
if (-not $libHead -or [System.Text.Encoding]::ASCII.GetString($libHead) -ne '!<arch>') {
    throw "nvsdk_ngx_s.lib sanity failed (not a COFF archive; delete and refetch)"
}

# --- Official signed NGX FG runtime (PORTING #8 官方帧生成后端) ---
# 落到 vendor\ngx\ 与模型 DLL 同目录,打包脚本按存在与否选装;官方链是 FG
# 唯一路径,RTX 30/20 由 dlssg_for_sm86 0.3.x hook 代理接管交付(自动档
# 预载)。尺寸门槛:HTML 错误页 / LFS 指针文件能通过非空检查,必须拦下。
$fgOfficial = Join-Path $root "vendor\ngx\nvngx_dlssg.dll"
if (-not (Test-Path $fgOfficial)) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $fgOfficial) | Out-Null
    $tmp = "$fgOfficial.download"
    Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/$ngxCommit/lib/Windows_x86_64/rel/nvngx_dlssg.dll" $tmp
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

# --- RTX Video SDK 1.1(VSR / TrueHDR 官方 NGX feature;公开 NGC 工件,
#     SHA-256 钉死。URL 与校验和取自 Magpie scripts/Fetch-RtxVideoSdk.ps1
#     同一来源:nvidia/multimedia/dlpp:1.5)---
# 产物:
#   vendor\rtxvideo\include\  — vsr/truehdr 的 defs+helpers 头(4 个;依赖
#     既有 dependencies\ngx 的核心头,Feature ID = Reserved14/16 已在位)
#   vendor\ngx\               — nvngx_vsr.dll / nvngx_truehdr.dll(rel,与
#     nvngx_dlssg.dll 同目录;经 NGX core 的 PathList 解析,插件不做 LoadLibrary)
# 不入库,可重建暂存区;NVIDIA 专有许可文本随包留档。
$rtxDir = Join-Path $root "vendor\rtxvideo"
$rtxZipMarker = Join-Path $rtxDir ".sdk-1.1.0-ok"
if (-not (Test-Path $rtxZipMarker)) {
    $rtxArchive = Join-Path $env:TEMP "RTX_Video_SDK_v1.1.0.zip"
    $rtxSha256 = 'ABF4F34E2B5A618E355B0D5A0365D8ECC3DB4396E756E4C850A867E1AE2ED69E'
    $needDownload = -not (Test-Path $rtxArchive)
    if (-not $needDownload) {
        $needDownload = (Get-FileHash -LiteralPath $rtxArchive -Algorithm SHA256).Hash -ne $rtxSha256
    }
    if ($needDownload) {
        # NGC 下载 API 返回 302 → xfiles.ngc.nvidia.com 签名 CDN 地址,curl -L 跟随。
        & curl.exe -sSL --fail --retry 8 --retry-all-errors --retry-delay 2 `
            -o $rtxArchive "https://api.ngc.nvidia.com/v2/models/nvidia/multimedia/dlpp/versions/1.5/files/RTX_Video_SDK_v1.1.0.zip"
        if ($LASTEXITCODE -ne 0) { throw "RTX Video SDK download failed (curl exit $LASTEXITCODE)" }
    }
    $gotSha = (Get-FileHash -LiteralPath $rtxArchive -Algorithm SHA256).Hash
    if ($gotSha -ne $rtxSha256) { throw "RTX Video SDK checksum mismatch ($gotSha); retry the official NGC download" }
    $rtxExtract = Join-Path $env:TEMP "rtx-video-sdk-extract"
    if (Test-Path $rtxExtract) { Remove-Item $rtxExtract -Recurse -Force }
    New-Item -ItemType Directory -Force $rtxExtract | Out-Null
    Expand-Archive -LiteralPath $rtxArchive -DestinationPath $rtxExtract -Force
    # zip 无包装目录(bin/include 直接在根;内嵌同名 zip 是原样冗余):
    # 根含目标头即用根,否则退单包装目录。
    $rtxSdkRoot = $rtxExtract
    if (-not (Test-Path (Join-Path $rtxSdkRoot "include\nvsdk_ngx_defs_vsr.h"))) {
        $wrapper = Get-ChildItem $rtxExtract -Directory | Select-Object -First 1
        if (-not $wrapper) { throw "RTX Video SDK zip layout unexpected" }
        $rtxSdkRoot = $wrapper.FullName
    }
    $rtxInc = Join-Path $rtxDir "include"
    New-Item -ItemType Directory -Force $rtxInc | Out-Null
    foreach ($h in @("nvsdk_ngx_defs_vsr.h", "nvsdk_ngx_defs_truehdr.h",
                     "nvsdk_ngx_helpers_vsr.h", "nvsdk_ngx_helpers_truehdr.h")) {
        $from = Join-Path $rtxSdkRoot "include\$h"
        if (-not (Test-Path $from)) { throw "RTX Video SDK trim: missing include\$h" }
        Copy-Item $from $rtxInc -Force
    }
    foreach ($d in @("nvngx_vsr.dll", "nvngx_truehdr.dll")) {
        $from = Join-Path $rtxSdkRoot "bin\Windows\x64\rel\$d"
        if (-not (Test-Path $from)) { throw "RTX Video SDK trim: missing bin\Windows\x64\rel\$d" }
        # 尺寸断言:HTML/LFS 指针伪 DLL 拦下(与 nvngx_dlssg.dll 同款防线)。
        if ((Get-Item $from).Length -lt 1MB) { throw "RTX Video SDK: $d size sanity failed" }
        Copy-Item $from (Join-Path $root "vendor\ngx") -Force
    }
    $lic = Join-Path $rtxSdkRoot "NVIDIA_RTX_Video_SDK_License.pdf"
    if (Test-Path $lic) { Copy-Item $lic $rtxDir -Force }
    Remove-Item $rtxExtract -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType File -Path $rtxZipMarker -Force | Out-Null
    Write-Host "RTX Video SDK 1.1: headers -> $rtxInc, snippets -> vendor\ngx (SHA256 verified)"
}

# --- VapourSynth R73 headers (runtime is R73 / API4, see mpv-lazy Lib\site-packages\vapoursynth-73.dist-info) ---
foreach ($h in @("VapourSynth4.h", "VSHelper4.h", "VSScript4.h")) {
    Fetch "https://raw.githubusercontent.com/VapourSynth/VapourSynth/R73/include/$h" (Join-Path $vsInc $h)
}

# --- NVOF headers(清单 #6 光流;官方 OpticalFlowSDK 仓库已从 GitHub 撤下,
#     mbucchia/Optical-Flow-SDK 是完整官方镜像,NvOFInterface 即 SDK 头目录)。
#     头文件非空 + C 头魔数无法判别,钉 commit 需要上游确认的固定哈希;
#     当前至少走 Fetch 的临时名原子落盘(截断不再毒化),换 commit 的动作
#     留给下一次有据可依的升级(勿手造哈希)。---
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
