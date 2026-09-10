# mpv_PlayKit native 测试工具箱(testkit)

`native/tests/` 是**临时**实验场:一次性补丁脚本、dump 产物、崩溃转储、
截图日志,用完即弃,不入库。**可复用**的验收工具沉淀在本目录,全部
零硬编码路径,任何人可按下面的约定直接运行。

## 运行约定(解决"每个人的 mpv 部署路径都不一样")

路径发现按优先级:

1. **用 mpv-lazy 自带的 `python.exe` 运行**(推荐)——脚本以
   `sys.executable` 所在目录为部署根:

   ```
   <你的部署根>\python.exe <仓库>\native\testkit\test_smoke.py
   ```

   mpv-lazy 发行包里 `python.exe` 与 `mpv.exe` 同目录,VapourSynth
   运行时与 vs-plugins 插件加载都按该根解析,所以无需任何配置。

2. **环境变量 `VSDLSSNR_TEST_ROOT`** —— 非 mpv-lazy 布局或用系统
   python 运行时,指向部署根(含 `vs-plugins\` 的目录):

   ```powershell
   $env:VSDLSSNR_TEST_ROOT = "C:\where\mpv\lives"
   python <仓库>\native\testkit\test_smoke.py
   ```

### 环境前提检查

脚本开头调用 `testenv.require_env()` / `require_nvidia()`:部署根解析
失败、插件缺失或无 N 卡时打印 **SKIP**(退出码 0),而不是报错——环境
缺失不是测试失败。

### 与插件运行时事实的对应关系

| 路径 | 解析规则 | 对应插件源码 |
|---|---|---|
| `dlssnr_timing.log` | 宿主 exe 旁(`GetModuleFileNameW(nullptr)`)——测试进程内跑滤镜 = python.exe 旁;拉起 mpv 验证 = mpv.exe 旁 | `dlssnr_context.cpp` TimingLog |
| `vs-plugins\dlssnr_ui.ini` | 部署根 + vs-plugins | `panel_app.cpp` |
| `vs-plugins\dlssnr_panel.exe` | 部署根 + vs-plugins | `bridge.cpp` LaunchPanelSilently |
| `vs-plugins\ngx\nvngx_dlssnr.dll` | 部署根 + vs-plugins + ngx | `plugin.cpp` |

## 脚本清单

### 基础链路(无需 mpv / 面板)

| 脚本 | 验证点 |
|---|---|
| `test_smoke.py` | 插件加载 -> NGX init -> 单帧往返,输出平面可读 |
| `test_perf.py` | 稳态 ms/frame 基准(720p/1080p),对照 README 性能数据 |
| `test_leak.py` | 150 帧工作集平台化(>0.5 MB/帧判 LEAK) |
| `check_downsample_kernel.py` | 高频图案下 res100/res50/mult1.5 输出必须分歧(核判别) |
| `validate_params.py` | 参数面验收:每配置一个 worker 进程(NGX 时域历史跨实例延续,同进程哈希被污染),det/DIFF/clamp 断言 |

### IPC / 面板(共享内存契约)

| 脚本 | 验证点 |
|---|---|
| `test_ipc.py` | F2 契约:payload 推参 -> 恰好一次 recreate;saveRequest -> ini 落盘;logEnabled=0 -> timing log 停增 |
| `test_e2e_panel.py` | F3:真实面板自动拉起,stats 通道 JSON + params 通道 magic/seq/gen |
| `test_seek_adopt.py` | seek 回归:新实例 create 时 adopt 面板 payload(不回退 ini),热交接零 recreate |
| `test_nvof_switch.py` | NVOF 质量热切换只重建 OF 会话,NGX feature 不动 |
| `verify_stats_keys.py` | stats JSON 键位:filter_state / of_mode / 必败路径上报 |
| `test_lifecycle.py` | 滤镜加载自动拉起面板;杀 mpv 后面板 watchdog 自退 |
| `test_res_crash.py` | 内部分辨率逐档下发,mpv 存活 + recreate 真实发生(空心测试防线) |
| `test_fg_fallback.py` | DLSS 帧生成降级:fg_enabled=1 且 proxy(version.dll)缺失 -> 优雅回退 1:1,输出帧数/格式不变 |

`panel_ipc.py` 是 `native/src/panel_ipc.h` 的 python 字节级镜像
(92 字节 packed struct + 自检 assert)。**改头文件必须同步这里**——
布局错位的教训:payload 字段错位时"存活断言"会空心通过。

### 数值验收(需 `VSDLSSNR_DUMP=1` 等环境开关 + numpy)

| 脚本 | 验证点 |
|---|---|
| `check_yuv_convert.py` | ConvertIn/ConvertOut 两段转换 vs numpy 参考(<=2 LSB,709 limited) |
| `check_p10_roundtrip.py` | P10 色块条带 SKIP_EVAL 往返 vs zimg(<=2 LSB;VSDLSSNR_SKIP_EVAL=1) |
| `check_nvof_downsample_gpu.py` | GPU 光流输入降采样 vs 参考双线性(<=1 LSB,alpha 恒 255) |

dump 文件写在**宿主 exe 旁**(插件 `GetModuleFileNameW(nullptr)` 逻辑),
`check_yuv_convert.py` 的 dump 目录参数缺省即该位置。

### 面板 UI 工具(PowerShell,交互排查非断言)

`panel_ui_tools.ps1` —— 面板是 ImGui 自绘 UI 无控件树,只能 Win32 枚举:

```
powershell -File panel_ui_tools.ps1 -Action capture   # PrintWindow 截图
powershell -File panel_ui_tools.ps1 -Action size|poll|dpi|hittest|windows
```

按窗口类 `vs_dlssnr_panel_app` 定位,不依赖安装路径。

## 插件侧诊断环境变量(与 testkit 配合)

| 变量 | 作用 |
|---|---|
| `VSDLSSNR_TIMING=1` | 每帧 timing 行(mpv 侧须开;python 侧 add_log_handler 已见) |
| `VSDLSSNR_DUMP=1` | 首帧管线中间纹理落盘(宿主 exe 旁,dump_*.bin) |
| `VSDLSSNR_PROBE=1` | init 细分探针行(DEVICE_HUNG 时序定位) |
| `VSDLSSNR_NO_PANEL=1` | 禁面板自动拉起(数值验收/无头场景) |
| `VSDLSSNR_SKIP_EVAL=1` | 跳过 NGX eval(纯转换往返验收) |
| `VSDLSSNR_NGX_LOG=1` | NGX SDK 日志 |
| `VSDLSSNR_D3D12_DEBUG=1` | D3D12 debug layer |

## 历史教训(写新测试前先读)

1. **同进程哈希断言会空心通过**:NGX 时域历史跨实例延续,连续两个
   "不同配置"实例可能输出相同哈希 → 每配置一个进程(`validate_params.py` 模式)。
2. **payload 布局错位 = 空心存活**:切换类测试必须同时断言"动作真实发生"
   (timing log recreate 行数),只断言存活无意义。
3. **mpv.com 是控制台壳**:树杀要用 `taskkill /T` 或补杀 `mpv.exe`,
   否则孤儿进程锁 DLL。
4. **timing log 跨进程可读是设计契约**:DENYNO 共享,测试应积极利用
   日志水位(`log_size`/`new_lines`)做断言,而不是 sleep 猜。
