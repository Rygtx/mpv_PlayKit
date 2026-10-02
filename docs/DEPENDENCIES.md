# 依赖与源码参考

下载文件按获取方式两类分列:拉不到、必须人工放的进「手动下载」;脚本自动落地的进「自动拉取」。「源码参考」只列移植/对照过代码的仓库,与下载无关。
构建入口 [native/scripts/fetch-deps.ps1](../native/scripts/fetch-deps.ps1),所有钉值(commit / tag / SHA-256)都在该脚本内,升级依赖改那里。

## 手动下载的依赖

| 依赖 | 链接 | 用途 | 落地位置 |
|---|---|---|---|
| nvngx_dlssnr.dll | (待填) | DLSSNR 神经渲染模型(NVIDIA DLSS SDK 310.9.0)。公开 NVIDIA/DLSS 仓库不含此文件(钉值 commit 下实测 404),打包必需,`package.ps1` 缺件即断言失败 | `native\vendor_manual\`(随包进 `vs-plugins\ngx\`) |

## 自动拉取的依赖

均由 `fetch-deps.ps1` 落地,不入库、可重建;哈希不符自动重取自愈。

| 依赖 | 版本 / 钉值 | 来源 | 用途 | 落地位置 |
|---|---|---|---|---|
| NGX SDK 头文件 | commit `a291cc7d2cc6` | `NVIDIA/DLSS` raw | NGX 接口声明(插件 + 面板) | `native/dependencies/ngx/include/` |
| nvsdk_ngx_s.lib | 同上,SHA-256 钉死 | `NVIDIA/DLSS` raw | NGX 静态核心库 | `native/dependencies/ngx/lib/` |
| nvngx_dlssg.dll(官方运行库) | 同上,SHA-256 钉死 | `NVIDIA/DLSS` raw | 官方帧生成后端(40/50 系直连) | `native/vendor/ngx/` |
| dlssg_for_sm86 代理 | tag `0.3.5`,DLL SHA-256 双钉 | codeload tag 归档 | RTX 30/20 系帧生成接管(version.dll + 出厂 ini) | `native/vendor/ngx/` |
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
