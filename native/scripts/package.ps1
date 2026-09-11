# 打包 mpv_PlayKit DLSSNR 完整发行包: dlssnr 分支 portable_config 全目录 + vs_dlssnr 插件三件套
# 全部输入取自仓库树内, 无本机绝对路径
# 用法: pwsh -File scripts\package.ps1 [-Version 2026.09.07]  (必须在 dlssnr 分支上运行)
# 产物: dist\mpv_PlayKit-dlssnr-v<版本>-full.zip
param([string]$Version)

$ErrorActionPreference = "Stop"
$native = Split-Path -Parent $PSScriptRoot   # native/
$repo   = Split-Path -Parent $native         # 仓库根
$branch = git -C $repo rev-parse --abbrev-ref HEAD
if ($branch -ne "dlssnr") { throw "请在 dlssnr 分支上运行 (当前: $branch, 该分支的 portable_config 才是发行基线+DLSSNR 定制)" }

# 版本默认取打包当日日期 (vyyyy.MM.dd); 显式传入时兼容带/不带 v 前缀
if (-not $PSBoundParameters.ContainsKey("Version")) {
    $Version = Get-Date -Format "yyyy.MM.dd"
}
$Version = $Version.TrimStart("v")

$binDll     = Join-Path $native "bin\vs_dlssnr.dll"
$binPanel   = Join-Path $native "bin\dlssnr_panel.exe"
$model      = Join-Path $native "vendor\ngx\nvngx_dlssnr.dll"
$fgDll      = Join-Path $native "vendor\ngx\version.dll"
$fgIni      = Join-Path $native "vendor\ngx\dlssg_sm86.ini"
$fgOfficial = Join-Path $native "vendor\ngx\nvngx_dlssg.dll"
# 全部文件必须齐备(fetch-deps.ps1 自动落地官方 runtime 与 NVOF/NGX 依赖;
# 模型与 proxy 为手工/vendor 部署)。缺任一件 = 打包失败,杜绝残缺发行包。
if (-not (Test-Path $binDll))     { throw "缺少编译产物: $binDll (先运行 scripts\build.ps1)" }
if (-not (Test-Path $binPanel))   { throw "缺少编译产物: $binPanel (先运行 scripts\build.ps1)" }
if (-not (Test-Path $model))      { throw "缺少模型文件: $model (把 nvngx_dlssnr.dll 复制到 native\vendor\ngx\)" }
if (-not (Test-Path $fgDll))      { throw "缺少帧生成代理: $fgDll (dlssg_for_sm86 的 version.dll 放入 native\vendor\ngx\)" }
if (-not (Test-Path $fgIni))      { throw "缺少帧生成代理配置: $fgIni (dlssg_sm86.ini 放入 native\vendor\ngx\)" }
if (-not (Test-Path $fgOfficial)) { throw "缺少官方帧生成运行时: $fgOfficial (先运行 scripts\fetch-deps.ps1 自动下载)" }

$dist  = Join-Path $native "dist"
$stage = Join-Path $dist "stage"
$zip   = Join-Path $dist "mpv_PlayKit-dlssnr-v$Version-full.zip"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force "$stage\vs-plugins\ngx" | Out-Null

# --- portable_config 全目录 (dlssnr 分支 = main+lite 发行基线 + DLSSNR 定制) ---
Copy-Item (Join-Path $repo "portable_config") (Join-Path $stage "portable_config") -Recurse
# 运行时缓存目录只保留空骨架
Get-ChildItem (Join-Path $stage "portable_config\_cache") -Recurse -File -ErrorAction SilentlyContinue | Remove-Item -Force -Confirm:$false

# --- 插件/面板/模型/帧生成全件, 对齐插件内推导的 <插件目录>\ngx\ 相对布局 ---
Copy-Item $binDll     (Join-Path $stage "vs-plugins")
Copy-Item $binPanel   (Join-Path $stage "vs-plugins")
Copy-Item $model      (Join-Path $stage "vs-plugins\ngx")
Copy-Item $fgDll      (Join-Path $stage "vs-plugins\ngx")
Copy-Item $fgIni      (Join-Path $stage "vs-plugins\ngx")
Copy-Item $fgOfficial (Join-Path $stage "vs-plugins\ngx")

# --- 安装说明 ---
$readme = Join-Path $stage "安装说明.txt"
@"
mpv_PlayKit DLSSNR 完整包 v$Version
========================================

基于 mpv-lazy 官方发行配置 (上游 main+lite 基线) + DLSSNR AI 画质增强定制

内容
  portable_config\                    完整配置目录 (官方配置上接入 DLSSNR)
  vs-plugins\vs_dlssnr.dll            VapourSynth 插件 (Magpie DLSSNR 移植, NGX Feature 18)
  vs-plugins\dlssnr_panel.exe         ImGui 独立调参面板 (运行时实时调参)
  vs-plugins\ngx\nvngx_dlssnr.dll     DLSSNR 模型 (NVIDIA DLSS SDK 310.9.0)
  vs-plugins\ngx\version.dll          DLSS 帧生成代理 (dlssg_for_sm86 原生实现, 自签名, RTX 30/20 系)
  vs-plugins\ngx\dlssg_sm86.ini       帧生成代理配置 (Router 由面板 FG 路由选项自动写入)
  vs-plugins\ngx\nvngx_dlssg.dll      DLSS 官方帧生成运行时 (NVIDIA 签名, RTX 40/50 系; 面板 "FG 路由" 可选)

安装 (已有 mpv-lazy, 建议与打包基线同版或更新)
  1. 备份你的 portable_config\ (若有个人修改)
  2. 用本包 portable_config\ 整目录替换原目录 (或逐文件覆盖)
     键位: * 开/关 DLSSNR增强; 右键菜单 > VF 滤镜 > DLSSNR增强 (RTX)
  3. 将 vs-plugins\ 内全部内容合并到 mpv 目录下的 vs-plugins\
     (模型必须位于 vs-plugins\ngx\ 子目录, 插件按此相对路径寻找)
  4. 重启 mpv

使用
  按 * 键或右键菜单中的 DLSSNR增强 开关 (RTX 显卡专用)
  首帧初始化约 1 秒 (模型加载), 属正常现象
  双击 vs-plugins\dlssnr_panel.exe 可在播放时实时调参, "保存为默认值"写入 dlssnr_ui.ini
  删除 vs-plugins\dlssnr_ui.ini 可恢复脚本默认参数
帧生成 (可选)
  vs\DLSSNR_NV.vpy 中 Fg_Enabled = True 开启: 输出帧率 x2-x4 (插值帧挂降噪之后)。
  RTX 40/50 系: 经官方运行时 nvngx_dlssg.dll (NVIDIA 签名, 无需 proxy; 面板 "FG 路由" 可固定官方档);
  RTX 30/20 系: 经 dlssg_for_sm86 代理 version.dll (面板 "FG 路由" 切换: 自动/SM86/SM75/官方 NGX, 默认 SM86, RTX 20 系选 SM75, 重启 mpv 生效);
  自动档初始化失败自动回退 1:1 (显式档不跨后端回退), 降噪不受影响; 建议配合面板光流质量 >= 2 使用

要求: RTX 显卡
本包不含 mpv.exe 与 VapourSynth 运行时, 请使用官方 mpv-lazy 发行包
"@ | Set-Content $readme -Encoding utf8BOM

# --- 压缩并清理暂存 ---
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
Remove-Item $stage -Recurse -Force -Confirm:$false

$zipItem = Get-Item $zip
Write-Host "打包完成: $($zipItem.FullName) ($([math]::Round($zipItem.Length / 1MB, 1)) MB)"
