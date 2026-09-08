# vs_dlssnr 迁移文档(存于 docs/,随仓库分发; gitignore 的 native/PORTING.md 排除规则保留)

> 用途:记录 Magpie experimental → mpv 的迁移项清单与迁移结果。只记结论,不记过程。

参考仓库:`F:\Project\Magpie`(experimental,HEAD 9824d758,2026-09-06 对齐)
宿主仓库:`F:\Project\mpv_PlayKit`(本仓库)· 部署:`D:\Portable\mpv-lazy`

---

## 一、迁移结果

### DLSSNR 插件(vs_dlssnr.dll,已完成)

- 形态:VapourSynth API4 原生插件(纯 D3D12、零 guidance 单帧、同分辨率、单次提交管线);NGX Feature 18 经静态 core + snippet 直连 + IAT hook 伪装,Magpie 调用链完整移植。
- 参数:preset/style/intensity/local_tone/local_structure/skin_structure/use_auto_mask/ui_correction 全部暴露且可实时生效(preset 为创建参数,切换走热重建);residual_multiplier + 残差精调 4 项(residual_saturation/residual_lightness/shadow_structure/reflection_glow,r1-r10 新增)实时生效;intensity/local_tone/local_structure 范围已随上游 r2-fix1 收紧为 0-1。
- 性能(RTX 3080,稳态):720p 18.0ms / 55.7fps;1080p 32.9ms / 30.4fps。首帧 init ~1s。
- 验证:smoke 数值、150 帧零泄漏、mpv 端到端;YUV 原生化(2026-09-08)后滤镜直收 YUV420P8/P10、CPU 零像素转换,数值验收 check_yuv_convert.py(两段转换 ≤1 LSB);2026-09-06 4-pass 对齐后 smoke/IPC 契约/生命周期/泄漏复验通过。

### 独立控制面板(dlssnr_panel.exe,已完成)

- 形态:ImGui + D3D11 自绘 UI 独立程序(中文、Per-Monitor DPI、悬停参数说明、滑块数值、保存/重置);托盘常驻,点击呼出/隐藏窗口,右键菜单退出。
- 生命周期:插件加载滤镜时自动静默拉起(仅托盘不弹窗);滤镜关闭/mpv 退出后自动退出(命名事件 `vs_dlssnr_bridge_alive` watchdog)。单实例 mutex。
- 通信:面板与插件经两块 512 字节命名共享内存双向通信(`native/src/panel_ipc.h`:参数通道面板写 → 插件 `bridge.cpp` 40ms 轮询应用(seq 门控,下一帧生效);stats 通道插件写 → 面板 0.5s 读,即 Profiler 数据源);「保存设置」→ `dlssnr_ui.ini` → 下次加载滤镜作为默认值(优先于 .vpy;删除 ini 恢复 vpy 控制)。test_bridge 9/9 字段、test_lifecycle 生命周期验证通过。
- 参数改动实时生效路径与保存路径完全分离,保存不影响当前画面。

### 部署布局

```
D:\Portable\mpv-lazy\
├── vs-plugins\{vs_dlssnr.dll, dlssnr_panel.exe, ngx\nvngx_dlssnr.dll, dlssnr_ui.ini(保存后生成)}
└── portable_config\{vs\DLSSNR_NV.vpy, menu.conf, input_uosc.conf(键位 *,45 行)}
本仓库:portable_config\{vs\DLSSNR_NV.vpy, menu.conf, input_uosc.conf(键位 *,45 行)}
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
| 1 | Residual 重建 + inputResolutionPercent(25-100% 内部推理 + 残差回源;commit 00be2c15:100% 也走残差通道) | DLSSNRFilter.cpp:100-308、1441-1506、1573-1583 | 高:50% 推理 → eval ~8ms,1080p 有望 60fps;质量/速度可调 | 中:3 个 compute + 中间纹理 + cbuffer;residual_multiplier 参数位已留 | **功能完成**(2026-09-05):三段 compute(Magpie HLSL 原样)+ 单次提交集成 + 热重建 + 面板滑块 + ini/json 持久化;数学验证 output = original + (denoised−color)×M ✓。**性能(文件日志实测,1080p,RTX 3080)**:pack 2.9 + NGX CPU 0.5 + GPU(NGX evaluate + 残差 compute)10.5 + unpack 2.2 ≈ **16.5ms/帧,60fps 能力**——早期 36-38ms 的端到端数字被 python 测试框架自身开销(~20ms 帧搬运)污染。NGX GPU 段对内部分辨率不敏感(100%→50% 仅降 ~1ms,模型固定成本主导 GPU 段);4K 源 python 端 ~105ms(含 74MB×N 帧搬运),mpv 真实路径待实测;vpy H_Max 已放开(=0)。0.01 步进实测有效(NGX 参数为连续 float,Magpie 的 STEP 0.05 仅是其 UI 粒度),面板滑块不吸附。坑:SRV 表若为连续 N 描述符则 t1 只能取相邻槽(vertical 需要 input+horizontal 不相邻)→ 改两个独立单描述符表;NGX evaluate 会重绑自己的 heap/root signature,其后自绘 dispatch 前必须重绑;`SetComputeRoot32BitConstants` 多 DWORD 必须用真数组;**compute dispatch 前忘 SetPipelineState 则 dispatch 被静默忽略**(全 0 输出根因,debug layer 不报);**D3D12 timestamp query 与 NGX evaluate 同命令列表 = 必现 SEH**(GPU 纯时长不可用,延迟指标改用 ExecuteAndWait 墙钟;调用已注释留说明)。**2026-09-06 对齐 v0.6.6(2d37f8c0/r2-fix1/1cde1bae)**:①管线 3→4 pass,新增 PrepareResidual——色调控制(饱和度/亮度/阴影结构/反射辉光)改为在内部低分辨率域、Catmull-Rom 插值前逐像素应用(HLSL 原样移植,含"有符号分段在前、HSL 差值在后"顺序),存 ControlledResidual(FP16);等宽时跳过 horizontal pass(垂直 pass 直读 controlledRes);内核 Lanczos3→Catmull-Rom;根签名 root constants 8→12。②**颜色降采样单 pass area-average → 两 pass 可分离 Lanczos2**(1cde1bae,colorDownsample=lanczos2-aa;负瓣经 FP16 中间纹理保留——移植端复用 horizontalRes 充当中间纹理,与上游 resampleIntermediate 复用方式一致;等尺寸时 Lanczos2 退化为精确拷贝故无需跳过分支)。新验证 validate_v066.py + check_downsample_kernel.py 全过 |
| 2 | ~~guidanceMode 运行时切换(Available/Force Zero/Motion Only/Depth Only)~~ | SelectGuidance cpp:1813-1842 | 低(随 #6 才有意义) | 小 | **作废**(2026-09-06):上游 r1-r10 删除 `guidanceMode`/`depthInferenceInterval`,参数面改为 `motionVectorQuality`(0-5,OF 质量);等效能力并入 #6 |
| 3 | GPU timestamp 遥测 | DLSSNRFilter.cpp:1673-1706、FrameGuidancePerformance.h | 低(已有 QPC 分段计时) | 小 | 搁置 |

### B. 新能力

| # | 功能 | Magpie 源码 | 价值 | 工作量 | 状态 |
|---|---|---|---|---|---|
| 4 | 可编程 VSR/降噪(VFX SDK:NvCVImage + NvVFX,qualityLevel 映射 RTX Video 档位;D3D11/CUDA interop) | RTXVideoDenoiser.cpp(261 行)+ VFX 运行时(部署目录带许可证) | 高:显式调用 VSR,不受驱动呈现层触发条件限制;附赠驱动级降噪 | 中 | 待做 |
| 5 | DLSS SR 真 AI 超分(ZeroMV 形态:无 jitter、零 MV/零深度、Preset J;标准 NGX 公开 feature,无需 IAT hook) | DLSSSRUpscaler.cpp(359 行)+ nvngx_dlss.dll(部署目录已有) | 高:可控真超分,放大低分辨率片源的正解 | 中大 | 待做 |
| 6 | NVOF 光流 guidance(真运动矢量) | NvidiaOpticalFlowProvider.cpp(703 行)+ FrameGuidanceD3D12Interop | 中高:消除零 guidance 的运动时域瑕疵 | 大 | **功能完成**(2026-09-07):D3D12 原生 NVOF(`nvofapi64.dll` 的 D3D12 API,设备级会话 + 栅栏同步),不用 Magpie 的 D3D11 互操作层(单消费者纯 D3D12,FrameGuidanceService/Interop 的多消费者 + D3D11 渲染器假设不适用;移植的是 NVOF provider 的会话链/densify/失败语义 + guidance 降采样)。motionVectorQuality 0-5 全链(vpy `motion_vector_quality`/ini `motion_vector_quality`/面板"光流质量"下拉/IPC DSSL5);0=零 guidance,>0 懒建会话,**切换只重建光流会话(PoolHold 内毫秒级)不动 NGX feature**;非 NVIDIA/驱动缺 OF/init 失败/连续 3 败 → 优雅回退零 guidance。densify HLSL 原样移植(S10.5 网格 + 前后向一致性 + cost 置信度);缩放启用时 guidance 置信度加权降采样到内部尺寸(深度输出裁掉:depth 恒零纹理是死重)。时序:帧序门(mutex+cv,超时跳帧)串行 execute;拷贝(槽 upload→NVOF 注册纹理)在专用 CL 上按帧序提交;execute 后 **CPU 等输出栅栏**(官方样例同款;队列级 Wait 实测不可靠);densify 在槽 CL 上执行(SubmitFrame 前栅栏已落位)。实现坑:nvofapi 模块/栅栏进程级永不卸载(FreeLibrary 死锁、fence 释放段错误);**nvOFDestroy 后进程内继续 GPU 工作会触发驱动访问违例** → 旧会话退役不销毁;CreateShaderResourceView(NULL,NULL) 本机驱动触发异步 TDR → 全部用真实占位视图;BGRA 管线切换后 dump footprint 格式必须跟着改(CopyTextureRegion 跨格式 E_INVALIDARG)。实测 320x240:gpu 8.9→9.0ms,nvof 段 ~1.3ms CPU;dump_motion 99.6% 非零运动向量 |
| 7 | DAV2 深度 guidance | DepthAnythingV2Provider.cpp + FrameGuidanceService.cpp | 低-中;掉卡事件元凶 | 很大 | **不计划** |

### 建议顺序

1 → 4 → 5 → 6;7 不计划。(1 与 4/5 无依赖,可并行评估;2 已作废并入 6)

> 已否决:GPU 调度优先级 REALTIME(Magpie Renderer.cpp 的 D3DKMTSetProcessSchedulingPriorityClass)——用户确认不需要。

### 关键坑(2026-09-07 清单 #6 实测新增)

11. D3D12 根签名:同一 descriptor table 内两个 range 的 `OffsetInDescriptorsFromTableStart` 都为 0 = 重叠范围(非法),驱动侧表现为 dispatch 挂死(GPU hang,无报错);u0/u1 必须各开一个表参数,且两个参数不得共用同一个 range 结构(寄存器 0/1 各一份),否则序列化直接 E_FAIL。根签名里**所有**参数都必须绑定,漏绑 = dispatch 读垃圾描述符。
12. `CreateShaderResourceView(nullptr, nullptr, h)` NULL 描述符(文档合法)在本机驱动(RTX 3080)与 NGX snippet 共存时触发**异步 TDR**(DEVICE_HUNG,初始化期瞬间死)——一律用真实资源的占位视图替代,由 cbuffer 旗标守卫不读取。
13. NVOF D3D12 的输出栅栏点在**队列级 Wait** 语义下可能永不满足(管线卡死);官方样例的 CPU 等待模式(execute 后 `SetEventOnCompletion` + WaitForSingleObject)可靠——CPU 等待落位后再提交消费方 CL。
14. `nvOFDestroy` 之后进程内继续 GPU 工作(NGX evaluate 等)会触发驱动内部访问违例(nvwgf2umx);`FreeLibrary(nvofapi64.dll)` 在引擎 worker 存活时死锁。对策:NVOF 模块/栅栏/旧会话一律进程级存活,永不销毁、永不卸载。
15. NVOF 输入格式的 "ABGR8" = DXGI `B8G8R8A8_UNORM`;管线切到 BGRA 后,`CopyTextureRegion` 的 placed footprint 格式必须同步(Cross-format → Close 时 E_INVALIDARG,表现为 dump 静默失败)。

## 三、2026-09-06 对齐记录(84d9f6ab → 9824d758,v0.6.6)

**对齐范围复核方法**:17 条新提交逐条 `git show` 核对源码(diff 实文,非文档/评审);DLSSNR 实质改动仅 1cde1bae/2d37f8c0/9824d758 三条;移植的 3 段残差 HLSL 与上游 HEAD 逐行比对一致(check_hlsl_vs_upstream.py)。

- **已移植(均为上游源码实改,非文档)**:①残差 4-pass 管线(清单 #1,见上);②两 pass Lanczos2 颜色降采样(1cde1bae,初版对齐时遗漏、复核补上);③残差精调 4 参数全链(vpy/ini/IPC 面板/cbuffer);④intensity 类参数 MAX 收紧 0-1(r2-fix1,ClampFinite);⑤**NgxRuntimeGuard**(9824d758,进程级 NGX 故障闩锁:首个 SDK 内 SEH 后所有 SDK 调用快速失败、故障模块跳过 shutdown,宿主重启才复位;与 43fe80a PoolHold 竞态修复互补)。IPC payload 布局加 4 float,`PAYLOAD_MAGIC` DSSL1→DSSL3(拒绝旧版面板的 reserved 零值被解码为 saturation=0)。
- **决策**:preset 保留 0-3(出处为上游 P8 v0.5.7 的 `DLSSNR.Hint.Render.Preset` 设计;1cde1bae 实改确认 hint 仍写入只是 `FIXED_PRESET=0`,r2-fix1 仅取消参数暴露),作为移植端扩展功能。
- **确认不适用(逐条对源码核实)**:r9 WGC 捕获修复(GraphicsCaptureFrameSource/FramePresentationTiming/EffectsProfiler——mpv 直供帧无 WGC)、r10 参数延迟重启(Renderer 整组 teardown 生命周期,Magpie 特有)、PR #4 实时参数编辑(EffectParametersViewModel/Renderer 实时队列——面板共享内存已覆盖;ApplyLiveParameters 的结果复用缓存插件无此机制)、PR #16 参数本地化(resw/XAML)、帧同步/Front Edge Sync(FramePacingOptions/OverlayDrawer——mpv 自管呈现)、FrameTrace/FramePacing 诊断、DLSSFrameGenerator/GraphicsCaptureFrameSource(v0.6.6 部分)、ZeroFrameGuidanceProvider/FrameGuidanceService(插件为静态零 guidance 纹理,等价 motionVectorQuality=None)、colorConvert(插件链路本身是 RGBA8)。
- **清单变化**:#2 guidanceMode 作废(上游删参数→motionVectorQuality 0-5,并入 #6);#6 目标接口改为共享 OF 服务。
- **测试状态**:test_smoke / test_ipc(新布局)/ test_lifecycle / test_leak / validate_v066 / check_downsample_kernel(高频图案验证内核路径)/ check_hlsl_vs_upstream(HLSL 逐行比对)全过;test_bridge、test_residual、test_4k 等 10 个引用 `dlssnr_live.json` 机制的陈旧死测试(旧 JSON 桥,已被共享内存桥取代)已清理。
