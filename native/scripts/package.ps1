# 打包 mpv_PlayKit DLSSNR 增量包: 相对上游 fork 基点变更的配置 + vs_dlssnr 插件三件套
# 全部输入取自仓库树内, 无本机绝对路径
# 用法: powershell -File scripts\package.ps1 [-Version 0.1.0]
# 产物: dist\mpv_PlayKit-dlssnr-<版本>-update.zip
param([string]$Version, [string]$Base = "4921c67")   # ← 上游 fork 基点, 同步上游后手动更新

$ErrorActionPreference = "Stop"
$native = Split-Path -Parent $PSScriptRoot   # native/
$repo   = Split-Path -Parent $native         # 仓库根
Write-Host "fork 基点: $Base"

# 未显式指定版本时尝试取最近的 git tag (去掉 v 前缀)
if (-not $PSBoundParameters.ContainsKey("Version")) {
    $tag = git -C $repo describe --tags --abbrev=0 2>$null
    if ($LASTEXITCODE -eq 0 -and $tag) { $Version = $tag.TrimStart("v") }
    else { $Version = "0.1.0" }
}
$global:LASTEXITCODE = 0

$binDll   = Join-Path $native "bin\vs_dlssnr.dll"
$binPanel = Join-Path $native "bin\dlssnr_panel.exe"
$model    = Join-Path $native "vendor\ngx\nvngx_dlssnr.dll"
if (-not (Test-Path $binDll))   { throw "缺少编译产物: $binDll (先运行 scripts\build.ps1)" }
if (-not (Test-Path $binPanel)) { throw "缺少编译产物: $binPanel (先运行 scripts\build.ps1)" }
if (-not (Test-Path $model))    { throw "缺少模型文件: $model (把 nvngx_dlssnr.dll 复制到 native\vendor\ngx\)" }

# --- 相对基点变更/新增的 portable_config 文件 (排除已删除) ---
$changed = git -C $repo diff --name-status "$Base" HEAD -- portable_config |
    Where-Object { $_ -notmatch '^D' } |
    ForEach-Object { ($_ -split "`t")[-1] }
if (-not $changed) { throw "基点 $Base 之后 portable_config 无变更, 无需打包" }
Write-Host "变更文件:`n$($changed | ForEach-Object { "  $_" })"

$dist  = Join-Path $native "dist"
$stage = Join-Path $dist "stage"
$zip   = Join-Path $dist "mpv_PlayKit-dlssnr-$Version-update.zip"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force "$stage\vs-plugins\ngx" | Out-Null

# --- 按原目录结构复制变更的配置文件 ---
foreach ($rel in $changed) {
    $dst = Join-Path $stage (($rel -replace '/', '\'))
    New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
    Copy-Item (Join-Path $repo ($rel -replace '/', '\')) $dst
}

# --- 插件/面板/模型, 对齐插件内推导的 <插件目录>\ngx\ 相对布局 ---
Copy-Item $binDll   (Join-Path $stage "vs-plugins")
Copy-Item $binPanel (Join-Path $stage "vs-plugins")
Copy-Item $model    (Join-Path $stage "vs-plugins\ngx")

# --- 安装说明 ---
$readme = Join-Path $stage "安装说明.txt"
@"
mpv_PlayKit DLSSNR 增量包 v$Version
========================================

本包只含相对上游 (hooke007/mpv_PlayKit) 变更的文件, 供已使用 mpv-lazy 的用户覆盖

内容
  portable_config\                    仅变更的配置 (按键位/菜单接入 + vs\DLSSNR_NV.vpy)
  vs-plugins\vs_dlssnr.dll            VapourSynth 插件 (Magpie DLSSNR 移植, NGX Feature 18)
  vs-plugins\dlssnr_panel.exe         ImGui 独立调参面板 (运行时实时调参)
  vs-plugins\ngx\nvngx_dlssnr.dll     DLSSNR 模型 (NVIDIA DLSS SDK 310.9.0)

安装 (已有 mpv-lazy)
  1. 将 portable_config\ 下文件按相同相对路径覆盖到你的 portable_config\
     (注意: input_list.conf / menu.conf 为整文件覆盖, 基于打包时的上游版本,
      若你的 mpv-lazy 版本较新, 请自行核对差异)
  2. 将 vs-plugins\ 内全部内容合并到 mpv 目录下的 vs-plugins\
     (模型必须位于 vs-plugins\ngx\ 子目录, 插件按此相对路径寻找)
  3. 重启 mpv

使用
  按 ' 键或右键菜单中的 DLSSNR增强 开关 (RTX 显卡专用)
  首帧初始化约 1 秒 (模型加载), 属正常现象
  双击 vs-plugins\dlssnr_panel.exe 可在播放时实时调参, "保存为默认值"写入 dlssnr_ui.ini
  删除 vs-plugins\dlssnr_ui.ini 与 dlssnr_live.json 可恢复脚本默认参数

本包不含 mpv.exe 与 VapourSynth 运行时, 请自行准备
全新安装请直接使用本仓库 portable_config 完整目录
"@ | Set-Content $readme -Encoding utf8BOM

# --- 压缩并清理暂存 ---
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
Remove-Item $stage -Recurse -Force -Confirm:$false

$zipItem = Get-Item $zip
Write-Host "打包完成: $($zipItem.FullName) ($([math]::Round($zipItem.Length / 1MB, 1)) MB)"
