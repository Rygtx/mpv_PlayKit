# DLSSNR on AMD 调研(实现参考)

调研日期 2026-10-03。结论先行:**本项目暂不适配 AMD**——维护者手头没有 AMD 显卡,无法实测验证;本文档仅作为将来做 AMD 适配时的第一参考。注意该生态迭代极快(一个月内 RDNA3 后端、Anywhere 模式相继落地),动手前务必重查各仓库最新状态。

## 背景

DLSSNR(NVIDIA NGX Feature 18,神经渲染)官方仅支持 RTX。2026-09 起社区跑通了在 AMD RDNA 卡上原生执行该网络的多条路线:权重统一取自用户自备的 `nvngx_dlssnr.dll`(310.8.0),网络本体逐块逆向重实现,不运行 NVIDIA 代码——与本项目 fetch-deps 拉原版 DLL 的思路同源,法律形态一致。

## 生态总览

| 项目 | 协议 | 星 | 覆盖架构 | 形态 |
|---|---|---|---|---|
| [danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD) | 闭源(NOASSERTION) | 2.2k | RDNA4(主)/ RDNA3 | 游戏 MOD 安装器,**无源码** |
| [lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) | **MIT** | 123 | RDNA4(gfx1200/1201) | HIP 模块链 + 三种宿主包装,**首选参考** |
| [mochizuki0323/DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD) | **MIT** | 74 | RDNA4 | Vulkan GLSL + NGX 兼容核心 |
| [3zwr1/AMD-NR---OptiScaler](https://github.com/3zwr1/AMD-NR---OptiScaler)(AMDNR) | GPL-3.0 | 382 | RDNA4 / RDNA3 / 掌机 APU | OptiScaler fork(集成者) |
| [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) | GPL-3.0 | 759 | 随宿主 | OptiScaler-NR 集成框架,可挂 mochizuki 后端 |

## danielblnc(闭源,不可集成)

仓库无源码,只发 `dlssnr_on_amd_setup.exe`。THIRD_PARTY.txt 自曝架构:Microsoft Detours 静态链入代理 DLL(hook)+ AMD FidelityFX SDK 头文件(ffx_api / ffx_api_upscale)——即代理 DLL 拦截游戏的 FSR 3/4 调用链,截取颜色/运动矢量/历史帧,用自研 HIP kernel 跑 NR 网络后交回。9070 XT 1080p 实测约 33 FPS(网络单帧 ~30ms,性能垫底)。**闭源、无库接口、游戏注入形态,对本项目零可复用性**;但它是"RDNA 能原生跑通此网络"的首个公开验证。其闭源运行时(`dlssnr_amd_pass1..3.dll`)经作者许可被 AMDNR 分发。

## lmxxf(首选参考,MIT)

71 块 Swin/ViT 网络逐块逆向,重实现为 29 个 HIP 模块/架构。对本项目的关键价值:

- **零环境依赖**:用 AMD 驱动自带的 `amdhip64_7.dll`(HIP 7 运行时),无需装 HIP SDK / Agility SDK / 预览版 DXC
- **D3D12 原生参考链**:≤0.15 版整个网络是 **D3D12 SM6.10 wave-matrix shader**(存于 `shaders/dx12-network/`,位精确参考)——与本项目 D3D12 管线同构;0.20 起转 HIP 提速 ~8%
- **宿主参考即"库"**:`Development/HIP/benchmark_live_capture.cpp` 是独立 D3D12 宿主(RGBA16F 纹理进 → encode/网络/decode → 纹理出,可选运动矢量与逐帧 reset),与 [dlssnr_context.cpp](../native/src/dlssnr_context.cpp) 的 NGX Evaluate 角色完全对应
- **有 Magpie 宿主包装**:官方三包装之一即 Magpie 便携版(网络插 `FSR3_SR` 效果槽)——本项目 DLSSNR 本就移植自 Magpie,该路径已有人趟过
- 性能(9070 XT,0.39 版离线整网):900 档 **6.8ms**,1080 档 **9.5ms**;VRAM ~1.2GB @900p(权重 0.6GB + 激活 0.3GB)。README 明确警告:**显存耗尽后帧率下降且不恢复**——本项目刚修过 RecreateFeature 残留泄漏,同类坑,适配时重建/释放纪律必须照搬
- 对 NVIDIA 原版 PSNR 47.55dB @1080p 单帧(全 71 块 + fast numeric);位精确模式、跳块(42/43/46)、降档等开关全有量化代价表

网络几何事实(适配时直接用):输入在档位 110% 内即用该档——720 档上限 1408×792、900 档 1760×990、其余 1080 档;NVIDIA 1080 档几何 1152 行(72 镜像行),社区改用 1088 行(8 镜像行)省 5.6% 算力、代价 ~54dB(对 NVIDIA 47.4→45.8)。

## mochizuki0323(质量验证最硬,MIT)

网络重写为 Vulkan GLSL:`VK_KHR_cooperative_matrix` + `VK_EXT_shader_float8` 编译到 RDNA4 WMMA,约 150 层顺序链(仅一对相邻层可并行),精度逐算子跟原版(FP8 处用 FP8,余 FP16/FP32)。

- **对拍方法学最认真**:同一输入在 RTX 5090 真机 NGX 上跑原版对比,单帧 PSNR 45.6~49.1dB(1080p→4K)、序列 46~51dB、SSIM 0.996+;方法与数据见其 `docs/ngx-verification/NGX-VERIFICATION.md`——将来本项目做 AMD 版,照此法验证
- **NGX 兼容核心形态**:提供 `dlssnr_core.dll` 实现 NGX 入口、内部换自家网络(故意不叫 `_nvngx.dll` 防 Streamline 误认)——与本项目动态 GetProcAddress 加载 nvngx 的做法能直接对接
- 权重提取工具 `linux/package/model-tools/unpack_*.py` 把 NR 网络结构(preblock / splitswin / vit / postblock)全部摸清,通用的逆向资料
- 短板:Linux(Proton/DXVK 互操作)为主版本;Windows 版要给游戏塞 DXVK 造 Vulkan 底座,对原生 D3D12 应用(本项目)不适用;性能低于 lmxxf

## AMDNR(GPL-3.0,集成者,RDNA3 唯一开源活路)

OptiScaler fork,集成了 lmxxf(RDNA4 默认,RX 9000 上比上代快 ~20%)+ danielblnc 闭源运行时 + 自研 RDNA3 兼容后端。RDNA3 方案:compat 头把 RDNA4 FP8 矩阵运算搬到 RDNA3 **F16 矩阵单元**,代价大(7800 XT 1440p 网络耗时 52.2ms)。有 "Anywhere" 捕获宿主模式(preview)与中文 README。**GPL 传染:代码只能参考思路,不可链接进本项目**。

## 接入路线判断(将来做的时候)

1. **形态无未知数**:接入点 = 本项目 NGX Evaluate 处换"D3D12 共享纹理 → HIP 模块链 → 共享纹理",`benchmark_live_capture.cpp` 已是完整参考;FFX 光流引导本就跨厂商,运动矢量侧无新问题
2. **硬件覆盖是硬伤**:三条开源路线全部只到 RDNA4(RX 9000 系);RDNA3 经 AMDNR 慢速兼容可达,RX 7000 以下无解。AMD 用户主力在 RX 6000/7000,收益面窄
3. **工程量清单**:HIP↔D3D12 外部内存/同步互操作(Windows)、RGBA16F 帧格式与本项目 YUV→RGBA16F 管线对齐、29 模块链调度与显存纪律、RDNA3 是否值得做(大概率不值得)
4. **验证方法**:照 mochizuki 的对拍法——同输入跑 NVIDIA 原版与 AMD 实现比 PSNR/SSIM,别只看观感

## 排雷记录

- danielblnc 仓库 README 的 "no NVIDIA code or data" 指其分发物不含 NVIDIA 代码;权重仍取自用户自备 DLL,与本项目同构
- mochizuki 的 `dlssnr_core.dll` 故意不叫 `_nvngx.dll`(Streamline 会把已加载的同名模块当官方核心,顶掉游戏的 DLSS 选项)——将来做兼容层时同样要避开此命名
- lmxxf 网络档位跟随输入分辨率(110% 阈值),与本项目"内部推理分辨率 25–100% 可调 + 残差重建"的设计不同源,参数不可直接互译
