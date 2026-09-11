[English](README.en.md) | 简体中文

![](Temp/mpv_PK.jpg)

# mpv_PlayKit — DLSSNR 定制版

> [hooke007/mpv_PlayKit](https://github.com/hooke007/mpv_PlayKit) 的 fork:在 mpv-lazy 官方发行配置(上游 main⊕lite 基线)上,接入移植自 [Magpie](https://github.com/SAOG0721/Magpie) experimental 的 **DLSSNR** AI 画质增强。

_管他什么逻辑、什么架构、什么边界条件、什么代码规范，vibe coding就是一把梭，代码是不看的，感觉对了就上线。_

## 这是什么

**vs_dlssnr** — 把 Magpie 的 DLSSNR(NVIDIA DLSS SDK 310.9.0 的 AI 视频增强,NGX Feature 18)移植为 VapourSynth API4 原生插件接入 mpv:

- 纯 D3D12 实现;默认零 guidance 单帧模式,可选 **NVOF 硬件光流引导**(0–5 档,RTX Turing+;不支持时自动回退零 guidance)
- **YUV 原生**:直接处理 YUV420P8/P10、输出同格式,CPU 侧零像素转换(纯行拷贝);色彩矩阵/范围按源帧属性(_Matrix/_ColorRange,709/601 + limited/full)处理,缺省 709 limited
- 同分辨率处理(非超分)
- 静态 NGX core + snippet 直连 + IAT hook,完整复刻 Magpie 调用链
- 残差管线:内部推理分辨率 25–100% 可调,Catmull-Rom 残差重建回源分辨率
- 全部参数实时生效(preset 切换走热重建),配套 ImGui 独立调参面板

### 支持的视频颜色格式

| 内容 | 处理 |
|---|---|
| YUV 4:2:0 8-bit(YUV420P8,nv12/yuv420p) | ✅ 增强(输出同格式) |
| YUV 4:2:0 10-bit(YUV420P10,HEVC 10-bit 等) | ✅ 增强(输出同格式) |
| 色彩矩阵 BT.709 / BT.601 / XYZ(按帧属性自动识别,缺失按 709) | ✅ |
| 色彩范围 limited / full(按帧属性) | ✅ |
| HDR(BT.2020 矩阵或 PQ/HLG 传输,按帧属性) | ⏭️ 自动直通(按 SDR 处理会错色) |
| 4:2:2 / 4:4:4 / 12-bit 及以上 / RGB | ⏭️ 自动直通 |

> "直通" = 正常播放、不做增强;属性缺失的裸流无法判定色彩元数据,按 709 limited 处理(与主流播放器缺省一致)。

上游的 mpv 通用折腾内容(配置说明、mpv-lazy 使用等)仍见 [上游 Wiki](https://github.com/hooke007/mpv_PlayKit/wiki) 与 [《win10/11 mpv播放器通用教程》](https://hooke007.github.io/unofficial/mpv_start.html)。

## 下载

[**🏷 Releases 🏷**](../../releases) 提供 `mpv_PlayKit-dlssnr-v<版本>-full.zip` 完整整包(portable_config 全目录 + 插件 + 面板 + 模型)。

## 包内容

| 文件 | 说明 |
|---|---|
| `portable_config\` | 完整配置目录(官方发行配置,已接入 DLSSNR 键位与菜单) |
| `vs-plugins\vs_dlssnr.dll` | VapourSynth API4 插件(mpv-lazy 会自动加载) |
| `vs-plugins\dlssnr_panel.exe` | ImGui 独立调参面板(可选,滤镜加载时自动拉起) |
| `vs-plugins\ngx\nvngx_dlssnr.dll` | DLSSNR 模型(必须位于 `ngx\` 子目录,插件按此相对路径寻找;取自 RenoDX 项目) |
| `vs-plugins\ngx\nvngx_dlssg.dll` | DLSS 官方帧生成运行时(NVIDIA 签名,RTX 40/50 系;面板"FG 路由"可选) |
| `vs-plugins\ngx\version.dll` | DLSS 帧生成 proxy(dlssg_for_sm86,RTX 30/20 系;自签名) |
| `vs-plugins\ngx\dlssg_sm86.ini` | 帧生成 proxy 配置(Router 由面板"FG 路由"选项自动写入) |
| `portable_config\vs\DLSSNR_NV.vpy` | 滤镜脚本(参数见下表) |

## 安装(已有 mpv-lazy)

1. 备份你的 `portable_config\`(若有个人修改)
2. 用包内 `portable_config\` 整目录替换原目录(或逐文件覆盖)
3. 将 `vs-plugins\` 内全部内容合并到 mpv 目录下的 `vs-plugins\`
4. 重启 mpv

> 手改 `portable_config\` 下的 `conf`/`vpy` 文件(含中文注释)时,请保持 **UTF-8 编码**;用记事本另存为 ANSI/GBK 会导致 mpv 解析异常。

本包不含 mpv.exe 与 VapourSynth 运行时,请使用官方 [mpv-lazy](https://github.com/hooke007/mpv_PlayKit/releases) 发行包(建议与打包基线同版或更新,当前基线 mpv-lazy-20260510)。

## 使用

- 按 **`*`** 键或右键菜单 **VF 滤镜 > DLSSNR增强 (RTX)** 开关滤镜
- 首帧初始化约 1 秒(模型加载),属正常现象
- 播放时双击 `vs-plugins\dlssnr_panel.exe` 实时调参(滤镜加载时面板也会自动静默拉起,托盘常驻,点击呼出)
  - 参数改动实时生效;**「保存设置」** 写入 `vs-plugins\dlssnr_ui.ini`,下次加载滤镜自动生效;**「重置默认」** 恢复出厂参数
  - **ini 优先于 vpy 参数**;删除 `dlssnr_ui.ini` 可恢复脚本默认值
  - 光流质量下拉(0–5)与"光流跟随降采样"开关实时可调;光流不可用时显示"已降级为零 guidance"
  - 处理用时时间线图(gpu/光流/推理分段);"写入性能日志"开关控制 `dlssnr_timing.log`
  - 面板随滤镜关闭 / mpv 退出自动退出

## 参数(DLSSNR_NV.vpy)

| 参数 | 范围 | 默认 | 说明 |
|---|---|---|---|
| `NR_Enabled` | True/False | True | 降噪总开关;False = 跳过降噪推理输出源帧,**补帧/光流照常工作不受影响**;降噪与帧生成都关时整个滤镜零初始化零开销;已激活会话内面板切换立即生效 |
| `Preset` | 0–3 | 0 | NR 预设档位(创建参数,切换触发热重建) |
| `Style` | 0–2 | 0 | 风格档位(0 默认 / 1 自然 / 2 电影) |
| `Intensity` | 0–1 | 1.0 | 强度 |
| `Local_Tone` | 0–1 | 1.0 | 局部色调强度 |
| `Local_Structure` | 0–1 | 1.0 | 局部结构强度 |
| `Skin_Structure` | −1–2 | −1 | 皮肤结构强度(−1 = 自动) |
| `Use_Auto_Mask` | True/False | True | 使用自动蒙版 |
| `UI_Correction` | True/False | True | UI 文字修正 |
| `Scaling_Enabled` | True/False | True | 残差管线总开关;False = 彻底关闭(不建中间纹理,按源尺寸直接处理) |
| `Input_Resolution` | 25–100 | 100 | NGX 内部推理分辨率占源尺寸的百分比,残差重建恢复到源分辨率 |
| `Residual_Multiplier` | 1.0–2.0 | 1.0 | 残差乘数,配合内部分辨率缩放补偿细节 |
| `Motion_Vector_Quality` | 0–5 | 0 | NVIDIA 光流引导档位(0 = 零 guidance;1–5 用硬件光流减轻运动场景时域伪影,档位越高越精确也越耗时) |
| `Nvof_Follow_Scaling` | True/False | False | 光流输入跟随内部降采样(需 Scaling_Enabled;大幅降低光流引擎占用,运动精度略降) |
| `Fg_Enabled` | True/False | False | DLSS 帧生成(挂降噪之后,输出帧率 ×2–×4;依赖 `ngx\version.dll`,初始化失败自动回退 1:1) |
| `Fg_Multiplier` | 2–4 | 2 | 插帧倍数(面板实时改,源帧边界生效;24fps ×3 = 72fps) |
| `Fg_Route` | 0–3 | 0 | FG 路由(0 = 自动,官方优先回落代理 SM86;1 = SM86/RTX 30 系走代理;2 = SM75/RTX 20 系走代理;3 = 官方 NGX/RTX 40/50 系,被拒载不回落。进程级,重启 mpv 生效) |
| `H_Max` | 整数 | 0 | 输出高度限制(源超过该值跳过处理;0 = 不限制) |

调参参考:100% 开缩放 ≈ 关闭缩放(multiplier = 1 时等价);降档位省不了多少帧耗(NGX 固定成本主导),主要影响高频细节,1080p 内容建议 50–75%,25% 仅极端省电场景;4K 源建议配合 `Input_Resolution = 50`。4K 全分辨率(res=100%)推理约 200ms/帧,物理性上限,非卡顿。

## 性能参考(实测,RTX 3080)

- 1080p:插件内部 ≈16.5ms/帧(≈60fps 能力;YUV 原生化后 pack 0.2 + NGX 推理 + GPU 光流 + unpack 0.3,CPU 侧每帧像素转换为零)
- 4K(res=50%):gpu ≈11–13ms/帧,可实时;res=100% 约 200ms/帧(NGX 4K 全分辨率物理成本)
- 首帧 init ~1s(165MB 模型加载)
- 诊断/测速开关(环境变量):
  - `VSDLSSNR_TIMING=1` 每帧分段耗时写 `dlssnr_timing.log`(测性能用它,勿用 python 端到端数字)
  - `VSDLSSNR_DUMP=1` 关键纹理 dump 到文件(数值验收)
  - `VSDLSSNR_PROBE=1` 逐帧级探针行(时序类问题定位)
  - `VSDLSSNR_SKIP_EVAL=1` 跳过推理测管线成本
  - `VSDLSSNR_D3D12_DEBUG=1` 开 D3D12 debug layer
  - `VSDLSSNR_NGX_LOG=1` 接 NGX 内部日志
  - `VSDLSSNR_NO_PANEL=1` 禁面板自动拉起(无头/验收场景)

## 显卡支持矩阵

两个 AI 功能对显卡的要求不同:

- **DLSSNR 降噪**:NVIDIA NGX 官方特性,需 Tensor Core,RTX 全系(Turing+)在官方支持范围内,非 RTX 不可用
- **DLSS 插帧(帧生成)**:双后端 —— RTX 40/50 系走**官方 NGX 分支**(Magpie 同款:官方签名 `nvngx_dlssg.dll` 经共享 NGX core 加载 + NVOF 自产运动矢量,无 proxy 无自签);RTX 30/20 系官方拒载,走 [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) 破解 proxy(自带推理资源与 PTX)。面板"FG 路由"单控件全覆盖:自动(官方优先,回落代理 SM86)/ SM86(30 系)/ SM75(20 系)/ 官方 NGX(40/50 系) —— 进程级,重启 mpv 生效

| 显卡 | 架构 | DLSSNR 降噪 | DLSS 插帧 |
|---|---|---|---|
| RTX 50 | Blackwell(SM120) | 🟡 理论上支持(官方范围内,未实测) | 🟡 理论上支持(官方 NGX 分支,未实测) |
| RTX 40 | Ada(SM89) | 🟡 理论上支持(官方范围内,未实测) | 🟡 理论上支持(官方 NGX 分支,未实测) |
| RTX 30 | Ampere(SM86) | ✅ **实测可用**(RTX 3080) | ✅ **实测可用**(RTX 3080,proxy SM86 路由) |
| RTX 20 | Turing(SM75) | 🟡 理论上支持(官方范围内,未实测) | 🟡 理论上支持(proxy SM75 路由;仅完成 PTX 前向验证,物理 Turing 卡未实测) |
| GTX / 非 RTX | — | ❌ 不可用(无 Tensor Core) | ❌ 不可用 |

图例:✅ 已实测可用 · 🟡 理论上支持(未实测) · ❌ 不可用

> - NVOF 硬件光流引导(RTX Turing+)全系原生支持;不支持或非 N 卡时自动回退零 guidance,不挡功能
> - 官方 NGX 分支要求 `nvngx_dlssg.dll`(NVIDIA 签名,发行包已含)与模型 DLL 同目录;3080 实测确认其被架构门禁正常拒载(自动档随后回落 proxy,不影响 30/20 系),面板"FG 路由"固定官方档可跳过该双探
> - RTX 30/20 系的 DLSS 插帧属于破解路径:proxy 不加载原厂 `nvngx_dlssg.dll`
> - 实测基线为 RTX 3080;其余档位为理论推断,欢迎反馈实测结果

## 环境要求

- Windows 10/11,**NVIDIA RTX 显卡**(非 RTX 不可用),建议保持较新驱动
- mpv-lazy(自带 VapourSynth R73 运行时)

## 分支

| 分支 | 用途 |
|---|---|
| `dlssnr` | **发行分支**:上游 main⊕lite 合并基线 + DLSSNR 定制(配置、插件源码、打包脚本),README 所述内容均在此分支 |
| `Dev` | 插件开发分支(历史开发线,功能已并入 `dlssnr`) |
| `main` | 与上游同步的干净主线 |

## 从源码构建

依赖自动落地(`fetch-deps.ps1` 从公开源拉取:NVIDIA/DLSS 官方仓库的 NGX 头文件 + 静态库、mbucchia/Optical-Flow-SDK 镜像的 NVOF 头、VapourSynth R73 头、ImGui 1.91.9b),无手工下载:

```powershell
# 1. 拉取依赖 (需 pwsh + curl)
pwsh -File native\scripts\fetch-deps.ps1

# 2. 编译 (MSVC x64, /MT 静态 CRT; 需 Visual Studio + CMake 4.0+)
powershell -File native\scripts\build.ps1          # 产物: native\bin\{vs_dlssnr.dll, dlssnr_panel.exe}

# 3. 打包整包 (必须在 dlssnr 分支; 模型 nvngx_dlssnr.dll 需先放入 native\vendor\ngx\)
pwsh -File native\scripts\package.ps1 [-Version 2026.09.08]   # 产物: native\dist\mpv_PlayKit-dlssnr-v<版本>-full.zip(默认版本号取当日日期)
```

移植清单、架构说明与踩坑记录见 [docs/PORTING.md](docs/PORTING.md)。

## 致谢与许可

- 上游 [hooke007/mpv_PlayKit](https://github.com/hooke007/mpv_PlayKit) 与 [mpv-lazy](https://github.com/hooke007/mpv_PlayKit/releases) — 本 fork 的配置基线
- [Magpie](https://github.com/SAOG0721/Magpie)(experimental fork,[Blinue/Magpie](https://github.com/Blinue/Magpie) 上游)— DLSSNR 参考实现
- **RenoDX** — `nvngx_dlssnr.dll` 模型来源
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS)、[VapourSynth](https://github.com/vapoursynth/vapoursynth)、[Dear ImGui](https://github.com/ocornut/imgui)

许可沿用上游,见 [LICENSE.MD](LICENSE.MD)。
