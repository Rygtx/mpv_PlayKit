# vs_dlssnr 迁移文档(存于 docs/,随仓库分发; gitignore 的 native/PORTING.md 排除规则保留)

> 用途:记录 Magpie experimental → mpv 的迁移项清单与迁移结果。只记结论,不记过程。

参考仓库:`F:\Project\Magpie`(experimental,HEAD 84d9f6ab)
宿主仓库:`F:\Project\mpv_PlayKit`(本仓库)· 部署:`D:\Portable\mpv-lazy`

---

## 一、迁移结果

### DLSSNR 插件(vs_dlssnr.dll,已完成)

- 形态:VapourSynth API4 原生插件(纯 D3D12、零 guidance 单帧、同分辨率、单次提交管线);NGX Feature 18 经静态 core + snippet 直连 + IAT hook 伪装,Magpie 调用链完整移植。
- 参数:preset/style/intensity/local_tone/local_structure/skin_structure/use_auto_mask/ui_correction 全部暴露且可实时生效(preset 为创建参数,切换走热重建);residual_multiplier 参数位已留(依赖清单 #1)。
- 性能(RTX 3080,稳态):720p 18.0ms / 55.7fps;1080p 32.9ms / 30.4fps。首帧 init ~1s。
- 验证:smoke 数值、150 帧零泄漏、mpv 端到端、10bit 链路(vpy 内 YUV↔RGB matrix_in_s=709)。

### 独立控制面板(dlssnr_panel.exe,已完成)

- 形态:ImGui + D3D11 自绘 UI 独立程序(中文、Per-Monitor DPI、悬停参数说明、滑块数值、保存/重置);托盘常驻,点击呼出/隐藏窗口,右键菜单退出。
- 生命周期:插件加载滤镜时自动静默拉起(仅托盘不弹窗);滤镜关闭/mpv 退出后自动退出(命名事件 `vs_dlssnr_bridge_alive` watchdog)。单实例 mutex。
- 通信:面板与插件经两块 512 字节命名共享内存双向通信(`native/src/panel_ipc.h`:参数通道面板写 → 插件 `bridge.cpp` 40ms 轮询应用(seq 门控,下一帧生效);stats 通道插件写 → 面板 0.5s 读,即 Profiler 数据源);「保存设置」→ `dlssnr_ui.ini` → 下次加载滤镜作为默认值(优先于 .vpy;删除 ini 恢复 vpy 控制)。test_bridge 9/9 字段、test_lifecycle 生命周期验证通过。
- 参数改动实时生效路径与保存路径完全分离,保存不影响当前画面。

### 部署布局

```
D:\Portable\mpv-lazy\
├── vs-plugins\{vs_dlssnr.dll, dlssnr_panel.exe, ngx\nvngx_dlssnr.dll, dlssnr_ui.ini(保存后生成)}
└── portable_config\{vs\DLSSNR_NV.vpy, menu.conf, input_uosc.conf(键位 ',45 行)}
本仓库:portable_config\{vs\DLSSNR_NV.vpy, menu.conf, input_uosc.conf(键位 ',45 行)}
构建:native\scripts\{fetch-deps.ps1, build.ps1} → native\bin\
```

### 关键坑(后续迁移先读)

1. READBACK/UPLOAD heap 不能建 TEXTURE2D(回读走 READBACK buffer + placed footprint);UAV 纹理要 `ALLOW_UNORDERED_ACCESS`;RTV clear 要 `ALLOW_RENDER_TARGET`。
2. snippet 创建参数里 ScalingRatioCallback 与 `DLSS.Indicator.Invert.*` 键不能省(否则 evaluate SEH)。
3. 模型 fallback 是颜色问题真凶:NGX 按 (projectID, engineVersion) 解析,失败会 fallback 到 DLSS SR 模型(去饱和蓝偏)。PathList 必须指向只含 nvngx_dlssnr.dll 的干净目录;新版验证标志:`ngx\nvngx.log` 出现 `MapProjectId ... Found cms id 876232c` 且零 fallback。
4. VS API4:`getFrameFilter` 引用必须 `freeFrame`(否则每帧泄一个源帧);`mapGetData` 缺键必须传 error 指针;插件入口 `VapourSynthPluginInit2`。
5. MSVC:`&临时对象` C2102;`WM_QUIT` 必须 `PostThreadMessageW`;测试用 `mpv.exe`(`mpv.com` 是启动器,kill 按名杀 mpv.exe)。

---

## 二、迁移清单

### A. DLSSNR 本体增强

| # | 功能 | Magpie 源码 | 价值 | 工作量 | 状态 |
|---|---|---|---|---|---|
| 1 | Residual 重建 + inputResolutionPercent(25-100% 内部推理 + Lanczos3 残差回源;commit 00be2c15:100% 也走残差通道) | DLSSNRFilter.cpp:100-308、1441-1506、1573-1583 | 高:50% 推理 → eval ~8ms,1080p 有望 60fps;质量/速度可调 | 中:3 个 compute + 中间纹理 + cbuffer;residual_multiplier 参数位已留 | **功能完成**(2026-09-05):三段 compute(Magpie HLSL 原样)+ 单次提交集成 + 热重建 + 面板滑块 + ini/json 持久化;数学验证 output = original + (denoised−color)×M ✓。**性能(文件日志实测,1080p,RTX 3080)**:pack 2.9 + NGX CPU 0.5 + GPU(NGX evaluate + 残差 compute)10.5 + unpack 2.2 ≈ **16.5ms/帧,60fps 能力**——早期 36-38ms 的端到端数字被 python 测试框架自身开销(~20ms 帧搬运)污染。NGX GPU 段对内部分辨率不敏感(100%→50% 仅降 ~1ms,模型固定成本主导 GPU 段);4K 源 python 端 ~105ms(含 74MB×N 帧搬运),mpv 真实路径待实测;vpy H_Max 已放开(=0)。0.01 步进实测有效(NGX 参数为连续 float,Magpie 的 STEP 0.05 仅是其 UI 粒度),面板滑块不吸附。坑:SRV 表若为连续 N 描述符则 t1 只能取相邻槽(vertical 需要 input+horizontal 不相邻)→ 改两个独立单描述符表;NGX evaluate 会重绑自己的 heap/root signature,其后自绘 dispatch 前必须重绑;`SetComputeRoot32BitConstants` 多 DWORD 必须用真数组;**compute dispatch 前忘 SetPipelineState 则 dispatch 被静默忽略**(全 0 输出根因,debug layer 不报);**D3D12 timestamp query 与 NGX evaluate 同命令列表 = 必现 SEH**(GPU 纯时长不可用,延迟指标改用 ExecuteAndWait 墙钟;调用已注释留说明) |
| 2 | guidanceMode 运行时切换(Available/Force Zero/Motion Only/Depth Only) | SelectGuidance cpp:1813-1842 | 低(随 #6 才有意义) | 小 | 待做 |
| 3 | GPU timestamp 遥测 | DLSSNRFilter.cpp:1673-1706、FrameGuidancePerformance.h | 低(已有 QPC 分段计时) | 小 | 搁置 |

### B. 新能力

| # | 功能 | Magpie 源码 | 价值 | 工作量 | 状态 |
|---|---|---|---|---|---|
| 4 | 可编程 VSR/降噪(VFX SDK:NvCVImage + NvVFX,qualityLevel 映射 RTX Video 档位;D3D11/CUDA interop) | RTXVideoDenoiser.cpp(261 行)+ VFX 运行时(部署目录带许可证) | 高:显式调用 VSR,不受驱动呈现层触发条件限制;附赠驱动级降噪 | 中 | 待做 |
| 5 | DLSS SR 真 AI 超分(ZeroMV 形态:无 jitter、零 MV/零深度、Preset J;标准 NGX 公开 feature,无需 IAT hook) | DLSSSRUpscaler.cpp(359 行)+ nvngx_dlss.dll(部署目录已有) | 高:可控真超分,放大低分辨率片源的正解 | 中大 | 待做 |
| 6 | NVOF 光流 guidance(真运动矢量) | NvidiaOpticalFlowProvider.cpp(703 行)+ FrameGuidanceD3D12Interop | 中高:消除零 guidance 的运动时域瑕疵 | 大 | 待做 |
| 7 | DAV2 深度 guidance | DepthAnythingV2Provider.cpp + FrameGuidanceService.cpp | 低-中;掉卡事件元凶 | 很大 | **不计划** |

### 建议顺序

1 → 4 → 5 → 6;7 不计划。(1 与 4/5 无依赖,可并行评估)

> 已否决:GPU 调度优先级 REALTIME(Magpie Renderer.cpp 的 D3DKMTSetProcessSchedulingPriorityClass)——用户确认不需要。
