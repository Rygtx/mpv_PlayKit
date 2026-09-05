# 打包 mpv_PlayKit DLSSNR 附加包: 配置 + vs_dlssnr 插件 + 面板 + 模型
# 全部输入取自仓库树内, 无本机绝对路径
# 用法: powershell -File scripts\package.ps1 [-Version 0.1.0]
# 产物: dist\mpv_PlayKit-dlssnr-<版本>.zip (目录结构与 mpv-lazy 部署布局对齐)
param([string]$Version)

$ErrorActionPreference = "Stop"
$native = Split-Path -Parent $PSScriptRoot   # native/
$repo   = Split-Path -Parent $native         # 仓库根

# 未显式指定版本时尝试取最近的 git tag (去掉 v 前缀)
if (-not $PSBoundParameters.ContainsKey("Version")) {
    $tag = git -C $repo describe --tags --abbrev=0 2>$null
    if ($LASTEXITCODE -eq 0 -and $tag) { $Version = $tag.TrimStart("v") }
    else { $Version = "0.1.0" }
}

$binDll   = Join-Path $native "bin\vs_dlssnr.dll"
$binPanel = Join-Path $native "bin\dlssnr_panel.exe"
$model    = Join-Path $native "vendor\ngx\nvngx_dlssnr.dll"
if (-not (Test-Path $binDll))   { throw "缺少编译产物: $binDll (先运行 scripts\build.ps1)" }
if (-not (Test-Path $binPanel)) { throw "缺少编译产物: $binPanel (先运行 scripts\build.ps1)" }
if (-not (Test-Path $model))    { throw "缺少模型文件: $model (把 nvngx_dlssnr.dll 复制到 native\vendor\ngx\)" }

$dist  = Join-Path $native "dist"
$stage = Join-Path $dist "stage"
$zip   = Join-Path $dist "mpv_PlayKit-dlssnr-$Version.zip"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force `
    "$stage\portable_config", "$stage\vs-plugins\ngx" | Out-Null

# --- portable_config 全量, 排除运行时缓存 (_cache) ---
robocopy (Join-Path $repo "portable_config") (Join-Path $stage "portable_config") `
    /E /NFL /NDL /NJH /NJS /NP /XD _cache | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy 失败, exit=$LASTEXITCODE" }
$global:LASTEXITCODE = 0   # robocopy 1-7 是成功码, 复位避免污染脚本退出码

# --- 插件/面板/模型, 对齐插件内推导的 <插件目录>\ngx\ 相对布局 ---
Copy-Item $binDll   (Join-Path $stage "vs-plugins")
Copy-Item $binPanel (Join-Path $stage "vs-plugins")
Copy-Item $model    (Join-Path $stage "vs-plugins\ngx")

# --- 安装说明 ---
$readme = Join-Path $stage "安装说明.txt"
@"
mpv_PlayKit DLSSNR 附加包 v$Version
========================================

内容
  portable_config\                    mpv 配置 (含 vs\DLSSNR_NV.vpy 及键位/菜单接入)
  vs-plugins\vs_dlssnr.dll            VapourSynth 插件 (Magpie DLSSNR 移植, NGX Feature 18)
  vs-plugins\dlssnr_panel.exe         ImGui 独立调参面板 (运行时实时调参)
  vs-plugins\ngx\nvngx_dlssnr.dll     DLSSNR 模型 (NVIDIA DLSS SDK 310.9.0)

安装 (基于 mpv-lazy, 或自备带 VapourSynth 支持的 mpv)
  1. 备份你现有的 portable_config (如有自定义)
  2. 将 portable_config\ 覆盖到 mpv.exe 同目录
  3. 将 vs-plugins\ 内全部内容合并到 mpv 目录下的 vs-plugins\
     (模型必须位于 vs-plugins\ngx\ 子目录, 插件按此相对路径寻找)
  4. 重启 mpv

使用
  按 ' 键或右键菜单中的 DLSSNR增强 开关 (RTX 显卡专用)
  首帧初始化约 1 秒 (模型加载), 属正常现象
  双击 vs-plugins\dlssnr_panel.exe 可在播放时实时调参, "保存为默认值"写入 dlssnr_ui.ini
  删除 vs-plugins\dlssnr_ui.ini 与 dlssnr_live.json 可恢复脚本默认参数

本包不含 mpv.exe 与 VapourSynth 运行时, 请自行准备
"@ | Set-Content $readme -Encoding utf8BOM

# --- 压缩并清理暂存 ---
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
Remove-Item $stage -Recurse -Force -Confirm:$false

$zipItem = Get-Item $zip
Write-Host "打包完成: $($zipItem.FullName) ($([math]::Round($zipItem.Length / 1MB, 1)) MB)"

