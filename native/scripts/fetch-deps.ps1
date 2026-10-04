# 从公开源落地 native/ 依赖:NGX SDK(官方 NVIDIA/DLSS 仓库)+ VapourSynth R73 头
# 用法: powershell -File scripts\fetch-deps.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot   # native/
$ngxInc = Join-Path $root "dependencies\ngx\include"
$ngxLib = Join-Path $root "dependencies\ngx\lib\Windows_x86_64\x64"
$vsInc  = Join-Path $root "dependencies\vapoursynth\include"
New-Item -ItemType Directory -Force $ngxInc, $ngxLib, $vsInc | Out-Null

function Fetch([string]$url, [string]$dst, [string]$sha256) {
    if (Test-Path $dst) {
        if (-not $sha256) { return }
        # 哈希钉值在位才短路;缓存内容与钉值不符(旧时代毒化缓存/钉值刚升级)
        # 删了重取,自愈而非等人来删。
        if ((Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash -eq $sha256) { return }
        Remove-Item $dst -Force
    }
    # --fail: never persist an HTTP error body (a 404 page would pass the
    # non-empty check below and poison the dependency cache permanently)
    # 断连防线(2026-09-25):先落临时名,成功后原子 Move —— curl 中途断连
    # 留下的截断文件不会再让 Test-Path 短路而永久毒化依赖缓存。
    # 重试只走 curl 默认暂态集(超时/5xx/429):404 等硬失败秒报,不做 8 次徒劳重试。
    $tmp = "$dst.download"
    & curl.exe -sSL --fail --retry 8 --retry-delay 2 -o $tmp $url
    if ($LASTEXITCODE -ne 0) {
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
        throw "download failed: $url (curl exit $LASTEXITCODE)"
    }
    if (-not (Test-Path $tmp) -or (Get-Item $tmp).Length -eq 0) { throw "download failed: $url" }
    # 哈希钉值:落盘前验临时文件,错字节进不了缓存。
    # 报错带上实际哈希:刻意升级时,改 ref → 跑 → 从这里复制新哈希 → 粘贴钉值。
    if ($sha256) {
        $got = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
        if ($got -ne $sha256) {
            Remove-Item $tmp -Force
            throw "SHA-256 mismatch: $url`ngot  $got`nwant $sha256`n(deliberate upgrade? paste the got hash into the pin)"
        }
    }
    Move-Item $tmp $dst -Force
    Write-Host "fetched: $dst ($((Get-Item $dst).Length) bytes)"
}

# --- NGX SDK headers (github.com/NVIDIA/DLSS,commit 钉死 2026-09-25:
#     此前走 /main 未钉 —— 上游 main 漂移会静默进构建;与同脚本 RTX SDK
#     的 SHA-256 钉死、sm86 的 tag+SHA-256 双钉对齐)---
$ngxCommit = "a291cc7d2cc6"
# 2026-10-02 裁剪(docs/CLEANUP-DECISIONS #8):只拉 include 传递闭包 5 个。
# 闭包:src 直接消费 nvsdk_ngx.h / nvsdk_ngx_defs_dlssg.h,vendor\rtxvideo
# 4 头 → {defs,helpers};ngx.h→defs+params,helpers.h→ngx+defs,其余
# (VK/dlssd 族 + helpers_dlssg/params_dlssg)零 include 不拉。
# 头文件哈希钉值(2026-10-05 评审补齐):与 .lib/.dll 同策略 —— 此前
# "存在即短路",缓存被本机污染/截断时静默进构建(URL commit 钉只护首次)。
# 钉值取自对应 commit 首拉内容;升级 commit 时同步重算。
$ngxHeaders = [ordered]@{
    "nvsdk_ngx.h"            = 'F6014A256F9D75CCEC1278AC6E23D596B398A76CC3960048CA1A274B378B1989'
    "nvsdk_ngx_defs.h"       = 'EA23F33497CD274860D1C25A97644FCE807DCB0037C594547203343103FAD03E'
    "nvsdk_ngx_defs_dlssg.h" = '5E76E5CF0397F0B093887D0392B427A6B3E3F722CEC5F5A4795357EDEB6DE4BA'
    "nvsdk_ngx_helpers.h"    = '2D5661F8B5AB55E1223E485F24146274D48077E09051873826B653D4384FE7D8'
    "nvsdk_ngx_params.h"     = '943BC8CC5CDAE03B6303016FBAD3183636F2335AE27A2D18776798C3B4EFABBC'
}
foreach ($h in $ngxHeaders.Keys) {
    Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/$ngxCommit/include/$h" (Join-Path $ngxInc $h) $ngxHeaders[$h]
}

# --- NGX static core lib (layout mirrors Magpie BuildOptions.props DLSSSdkDir) ---
# SHA-256 钉死:HTML 错误页/LFS 指针/截断/内容漂移全拦,缓存不符自愈重取。
$ngxLibSha256 = '36EAB29264C2A06456BA8415F20086AEA36F0CBB314D020085CB288CC06BB9AE'
$ngxLibPath = Join-Path $ngxLib "nvsdk_ngx_s.lib"
Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/$ngxCommit/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" $ngxLibPath $ngxLibSha256

# --- Official signed NGX FG runtime (PORTING #8 官方帧生成后端) ---
# 落到 vendor\ngx\(与官方 FG 运行库同目录;NR 原版模型同样脚本拉取,
# 社区变体才放 vendor_manual\),打包脚本按存在与否选装;官方链是 FG
# 唯一路径,RTX 30/20 由 dlssg_for_sm86 0.3.x hook 代理接管交付(自动档
# 预载)。SHA-256 钉死(commit 已钉,raw 文件 immutable):HTML 错误页/
# LFS 指针/截断/漂移全拦,与 RTX SDK 的 zip 哈希钉同一机制。
$fgOfficial = Join-Path $root "vendor\ngx\nvngx_dlssg.dll"
$dlssgDllSha256 = '135EAF0733C1E37381A8C28ABCF7A862404A54132B81787C04E35D09EFC5E36F'
New-Item -ItemType Directory -Force (Split-Path -Parent $fgOfficial) | Out-Null
Fetch "https://raw.githubusercontent.com/NVIDIA/DLSS/$ngxCommit/lib/Windows_x86_64/rel/nvngx_dlssg.dll" $fgOfficial $dlssgDllSha256

# --- dlssg_for_sm86 FG hook proxy(RTX 30/20 DLSS-G 接管层;上游仓库直取)---
# version.dll 与出厂 dlssg_sm86.ini 都在上游仓库根(普通 git blob,非 LFS),
# codeload tag 归档直下解出。tag 与 dll SHA-256 双钉:codeload zip 字节会漂移,
# 钉解出物(zip 内容 immutable);升级上游时同步改两处。既有文件哈希不符
# (含手工放置的旧版)一律重取覆盖,vendor 是可重建暂存区,不承载手工修改
# (出厂 ini 同理,部署侧要改请改部署副本)。
$fgProxyDll = Join-Path $root "vendor\ngx\version.dll"
$fgProxyIni = Join-Path $root "vendor\ngx\dlssg_sm86.ini"
$sm86Tag = "0.3.5"
$sm86DllSha256 = 'C3934A09399F022504227C72DF0BF8C0DE55F9A08880DDDDE898C5262CEFA838'
# ini 随 dll 同批复制;中断落在两次 Copy 之间时,dll 哈希命中会把本分支
# 整段跳过,ini 永不补齐(重跑 fetch-deps 无法自愈,2026-10-05 评审修)——
# ini 缺失同样进本分支重取,两件一起重拷(幂等)。
if ((-not (Test-Path $fgProxyDll)) -or
    (Get-FileHash -LiteralPath $fgProxyDll -Algorithm SHA256).Hash -ne $sm86DllSha256 -or
    (-not (Test-Path $fgProxyIni))) {
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
    $gotDllSha = (Get-FileHash -LiteralPath $dllSrc -Algorithm SHA256).Hash
    if ($gotDllSha -ne $sm86DllSha256) { throw "dlssg_for_sm86 zip: version.dll SHA-256 mismatch`ngot  $gotDllSha`nwant $sm86DllSha256`n(deliberate upgrade? paste the got hash into sm86DllSha256)" }
    if (-not (Test-Path $iniSrc)) { throw "dlssg_for_sm86 zip: dlssg_sm86.ini missing" }
    Copy-Item $dllSrc $fgProxyDll -Force
    Copy-Item $iniSrc $fgProxyIni -Force
    Remove-Item $extract -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    Write-Host "dlssg_for_sm86 ${sm86Tag}: $fgProxyDll ($((Get-Item -LiteralPath $fgProxyDll).Length) bytes) + factory ini"
}

# 出厂 ini 的 MaxGeneratedFrames 默认 3(4X);部署默认开到 5(6X)——
# 上限只是钳位,倍数由插件按面板请求,常开无副作用,免去"首次切 5x/6x
# 需重启"的过渡态。仅裁剪部署副本,vendor 是可重建暂存区。
if (Test-Path $fgProxyIni) {
    # 必须显式 UTF8:无 BOM 文件 PS 5.1 默认按 ANSI(GBK)读,中文注释
    # 过一遍会变乱码,且句号尾字节+换行会被当 GBK 双字节吞掉 —— 换行一丢,
    # Optimized=1 粘上前一行,面板"内核档位"下拉就地找不到键,写入必失败。
    $iniText = Get-Content $fgProxyIni -Raw -Encoding UTF8
    if ($iniText -notmatch '(?m)^MaxGeneratedFrames=5\s*$') {
        $iniText = $iniText -replace '(?m)^MaxGeneratedFrames=\d+\s*$', "MaxGeneratedFrames=5"
        # 替换必须命中:上游删键/改名的话静默丢 6X 上限,不如当场炸。
        if ($iniText -notmatch '(?m)^MaxGeneratedFrames=5\s*$') {
            throw "dlssg_sm86.ini: MaxGeneratedFrames key not found (upstream layout changed)"
        }
        # PS 5.1 无 utf8NoBOM 枚举值,用 .NET 无 BOM UTF8 写回(两代 PowerShell 通用)。
        [System.IO.File]::WriteAllText($fgProxyIni, $iniText, [System.Text.UTF8Encoding]::new($false))
        Write-Host "fg proxy ini: MaxGeneratedFrames -> 5 (6X cap)"
    }
}

# --- nvngx_dlssnr 模型(NR 神经渲染;官方 NVIDIA 签名原版)---
# 公开 NVIDIA/DLSS 仓库不含此文件(各 310.x tag 实测 404),唯一可验证的
# 公开载体是 Magpie experimental 的 Release 资产 DLSSNR-DLL-Options zip
# (内含 NVIDIA-Original + 社区改版,只取原版;社区改版由用户手动放置于
# vendor_manual\,命名约定见 plugin.cpp SelectNgxDllVariant)。Release
# 资产上游可重传/可变,zip 本身不钉,钉解出物哈希:变动即炸,不静默。
$nrModel = Join-Path $root "vendor\ngx\nvngx_dlssnr.dll"
$nrModelSha256 = 'E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E'
if (-not (Test-Path $nrModel) -or (Get-FileHash -LiteralPath $nrModel -Algorithm SHA256).Hash -ne $nrModelSha256) {
    $nrZip = Join-Path $env:TEMP "DLSSNR-DLL-Options-310.8.0.0.zip"
    Fetch "https://github.com/SAOG0721/Magpie/releases/download/v0.6.9-experimental/DLSSNR-DLL-Options-310.8.0.0.zip" $nrZip
    $nrExtract = Join-Path $env:TEMP "dlssnr-model-extract"
    if (Test-Path $nrExtract) { Remove-Item $nrExtract -Recurse -Force }
    New-Item -ItemType Directory -Force $nrExtract | Out-Null
    Expand-Archive $nrZip $nrExtract -Force
    $nrSrc = Join-Path $nrExtract "NVIDIA-Original\nvngx_dlssnr.dll"
    if (-not (Test-Path $nrSrc)) { throw "DLSSNR-DLL-Options zip: NVIDIA-Original\nvngx_dlssnr.dll missing" }
    $gotNrSha = (Get-FileHash -LiteralPath $nrSrc -Algorithm SHA256).Hash
    if ($gotNrSha -ne $nrModelSha256) { throw "nvngx_dlssnr.dll SHA-256 mismatch`ngot  $gotNrSha`nwant $nrModelSha256`n(upstream asset changed? re-pin deliberately or pick a new source)" }
    Copy-Item $nrSrc $nrModel -Force
    Remove-Item $nrExtract -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $nrZip -Force -ErrorAction SilentlyContinue
    Write-Host "nvngx_dlssnr 310.8.0.0 (NVIDIA original): $nrModel (SHA256 verified)"
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
        # 重试只走 curl 默认暂态集,404 等硬失败秒报(与 Fetch 同款)。
        & curl.exe -sSL --fail --retry 8 --retry-delay 2 `
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
$vsHeaders = [ordered]@{
    "VapourSynth4.h" = 'A73A23AA9EA8395F475644344CF7B60FBC9DE29A571B9AF131361265C09C8530'
    "VSHelper4.h"    = 'A648BC2123E17365548BE83070A6F1363994CCEF54433D3848AA84F15C419892'
    "VSScript4.h"    = '2C9497111C23784F9A2C3CF94007A1EE9ABBFBA47A1BB33A3E26EAF38C689B36'
}
foreach ($h in $vsHeaders.Keys) {
    Fetch "https://raw.githubusercontent.com/VapourSynth/VapourSynth/R73/include/$h" (Join-Path $vsInc $h) $vsHeaders[$h]
}

# --- NVOF headers(清单 #6 光流;官方 OpticalFlowSDK 仓库已从 GitHub 撤下,
#     mbucchia/Optical-Flow-SDK 是完整官方镜像,NvOFInterface 即 SDK 头目录)。
#     commit 钉死 2026-10-02(git ls-remote 实测 HEAD,与 NGX 同款操作;
#     此前走 /main 漂移。升级时重新 ls-remote 取新 commit,勿凭记忆改)---
$nvofCommit = "54e68293b4898a530bc07e4d7df71efbc5d30f9b"
$nvofDir = Join-Path $root "vendor\nvof"
New-Item -ItemType Directory -Force $nvofDir | Out-Null
# D3D12-only:Common 被 D3D12 头直接包含必留;D3D11/Cuda 是平级喂入接口
# (D3D11 纹理/CUDA 缓冲),不在本工程路径上。
$nvofHeaders = [ordered]@{
    "nvOpticalFlowCommon.h" = 'CDBDBF0797FC02D2DE1051520D4164BB1E36B7722009F46CB16216675319436A'
    "nvOpticalFlowD3D12.h"  = 'F12BBC6C54278CB5D3AE81723CA0363644F672FC3E72B635A709470FFDD6886F'
}
foreach ($h in $nvofHeaders.Keys) {
    Fetch "https://raw.githubusercontent.com/mbucchia/Optical-Flow-SDK/$nvofCommit/NvOFInterface/$h" (Join-Path $nvofDir $h) $nvofHeaders[$h]
}

# --- FidelityFX SDK v2.3.0(AMD 光流后端 FxofContext;裁剪子集,vendor 不入库
#     可重建,仿 vendor/nvof 惯例)。API 面与 Magpie AmdOpticalFlowProvider
#     所用一致(ffx_api 新 C 接口,FSR3 3.x 时代):下载 → 解压 → 裁剪 →
#     断言(头文件 API 符号 + 7 个 pass shader 齐全),任何一步失败即 throw,
#     防止半成品 vendor 毒化构建。---
$ffxDir = Join-Path $root "vendor\fidelityfx"
$ffxMarker = Join-Path $ffxDir ".sdk-2.3.0-ok"
# 门卫用 marker(RTX 段同款)而非探测 ffx_api.h:api\include 是复制顺序第一项,
# 若某次挂在中途,ffx_api.h 已在位 → 整段被跳过,半成品 vendor 绕过全部断言
# 直进构建。marker 最后落盘,复制不完整则下次整段重跑(裁剪复制可 -Force 重入)。
if (-not (Test-Path $ffxMarker)) {
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
    New-Item -ItemType File -Path $ffxMarker -Force | Out-Null
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
$imguiVer = "1.91.9b"
$imguiDir = Join-Path $root "dependencies\imgui"
if (-not (Test-Path (Join-Path $imguiDir "imgui.h"))) {
    # 临时 zip 带版本后缀:TEMP 残留旧包不会在升版本后被 Test-Path 短路复用。
    $zip = Join-Path $env:TEMP "imgui-$imguiVer.zip"
    Fetch "https://github.com/ocornut/imgui/archive/refs/tags/v$imguiVer.zip" $zip
    $extract = Join-Path $root "dependencies"
    Expand-Archive $zip $extract -Force
    if (Test-Path $imguiDir) { Remove-Item $imguiDir -Recurse -Force -Confirm:$false }
    Rename-Item (Join-Path $extract "imgui-$imguiVer") "imgui"
    Remove-Item $zip -Force -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "imgui: unpacked to $imguiDir"
}

Write-Host "done."
