# 依赖与源码参考

下载文件按获取方式两类分列:拉不到、必须人工放的进「手动下载」;脚本自动落地的进「自动拉取」。「源码参考」只列移植/对照过代码的仓库,与下载无关。
构建入口 [native/scripts/fetch-deps.ps1](../native/scripts/fetch-deps.ps1),所有钉值(commit / tag / SHA-256)都在该脚本内,升级依赖改那里。

## 手动下载的依赖

| 依赖 | 链接 | 用途 | 落地位置 |
|---|---|---|---|
| nvngx_dlssnr.4090.dll | https://discord.com/channels/1408098019194310818/1551422945203855370 | RTX4090 ONLY 改版(RenoDX Discord,需登录并加入服务器);插件按 GPU 探测自动选用([plugin.cpp](../native/src/plugin.cpp) `SelectNgxDllVariant`),**文件名后缀即选档依据,不可改名**;选装,缺失回落原版 | `native\vendor_manual\`(随包进 `vs-plugins\ngx\`) |
| nvngx_dlssnr.40xx.dll | 同上 | RTX4080_70_60 ONLY 改版,其余 Ada(4080/70/60 及移动版等)用;选装规则同上 | `native\vendor_manual\`(随包进 `vs-plugins\ngx\`) |
| nvngx_dlssnr.2030.dll | 同上 | RTX 20,30 PLAIN FP16 改版;选装规则同上 | `native\vendor_manual\`(随包进 `vs-plugins\ngx\`) |

## 自动拉取的依赖

均由 `fetch-deps.ps1` 落地,不入库、可重建;哈希不符自动重取自愈。

| 依赖 | 版本 / 钉值 | 来源 | 用途 | 落地位置 |
|---|---|---|---|---|
| NGX SDK 头文件 | commit `a291cc7d2cc6` | `NVIDIA/DLSS` raw | NGX 接口声明(插件 + 面板) | `native/dependencies/ngx/include/` |
| nvsdk_ngx_s.lib | 同上,SHA-256 钉死 | `NVIDIA/DLSS` raw | NGX 静态核心库 | `native/dependencies/ngx/lib/` |
| nvngx_dlssg.dll(官方运行库) | 同上,SHA-256 钉死 | `NVIDIA/DLSS` raw | 官方帧生成后端(40/50 系直连) | `native/vendor/ngx/` |
| dlssg_for_sm86 代理 | tag `0.3.5`,DLL SHA-256 双钉 | codeload tag 归档 | RTX 30/20 系帧生成接管(version.dll + 出厂 ini) | `native/vendor/ngx/` |
| nvngx_dlssnr.dll(NR 原版模型) | v310.8.0.0,DLL SHA-256 钉死 | Magpie Release 资产 `DLSSNR-DLL-Options-310.8.0.0.zip`(取 `NVIDIA-Original\`;公开 NVIDIA/DLSS 仓库 404) | NR 神经渲染模型,必备兜底;变体缺失/非 40/30 系卡走它 | `native/vendor/ngx/` |
| RTX Video SDK | v1.1.0,zip SHA-256 钉死 | NGC `nvidia/multimedia/dlpp:1.5`(URL/校验和出处:Magpie `scripts/Fetch-RtxVideoSdk.ps1`) | VSR 超分 / TrueHDR 头与运行库(执行模型对照包内 D3D12 样例 CDx12NGXVSR) | `native/vendor/rtxvideo/`、`native/vendor/ngx/` |
| VapourSynth 头 | R73 | `VapourSynth/VapourSynth` raw | 插件 API4 接口(运行时用 mpv-lazy 自带) | `native/dependencies/vapoursynth/include/` |
| NVOF 头 | commit `54e68293` | `mbucchia/Optical-Flow-SDK` raw | NVIDIA 光流接口(D3D12) | `native/vendor/nvof/` |
| FidelityFX SDK | v2.3.0(裁剪 OF 闭包) | codeload tag 归档 | AMD 光流后端 FxofContext | `native/vendor/fidelityfx/` |
| WinPixEventRuntime | NuGet 最新 | nuget.org API | `pix3.h`(FidelityFX 硬依赖) | `native/vendor/fidelityfx/include/` |
| Dear ImGui | v1.91.9b | codeload tag 归档 | 面板 UI | `native/dependencies/imgui/` |

## 源码参考

| 仓库 | 链接 | 参考内容 |
|---|---|---|
| Magpie experimental | https://github.com/SAOG0721/Magpie | DLSSNR 主参考实现(插件整体移植自其 experimental 分支);VSR/TrueHDR 语义对照其 `RTXVideoHdr.cpp`;AMD 光流 API 面对照其 `AmdOpticalFlowProvider` |
| RTX40MFG-Unlock | https://github.com/dashdogy/RTX40MFG-Unlock | RTX 40 系帧生成 count gate 的字节判定与进程内解锁手法,精简移植进 `dlssfg_gate.cpp`(MIT);仅参考,构建不消费其产物,不在自动拉取之列 |
