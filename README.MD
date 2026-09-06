![](Temp/mpv_PK.jpg)

# mpv_PlayKit — DLSSNR 定制版

> [hooke007/mpv_PlayKit](https://github.com/hooke007/mpv_PlayKit) 的 fork:在 mpv-lazy 官方发行配置(上游 main⊕lite 基线)上,接入移植自 [Magpie](https://github.com/Blinue/Magpie) experimental 的 **DLSSNR** AI 画质增强。

_管他什么逻辑、什么架构、什么边界条件、什么代码规范，vibe coding就是一把梭，代码是不看的，感觉对了就上线。_

## 这是什么

**vs_dlssnr** — 把 Magpie 的 DLSSNR(NVIDIA DLSS SDK 310.9.0 的 AI 视频增强,NGX Feature 18)移植为 VapourSynth API4 原生插件接入 mpv:

- 纯 D3D12 实现,零 guidance 单帧模式(不需要运动矢量 / 深度)
- 同分辨率处理(非超分),SDR 色彩空间(非 HDR)
- 静态 NGX core + snippet 直连 + IAT hook,完整复刻 Magpie 调用链
- 残差管线:内部推理分辨率 25–100% 可调,Catmull-Rom 残差重建回源分辨率
- 全部参数实时生效(preset 切换走热重建),配套 ImGui 独立调参面板

上游的 mpv 通用折腾内容(配置说明、mpv-lazy 使用等)仍见 [上游 Wiki](https://github.com/hooke007/mpv_PlayKit/wiki) 与 [《win10/11 mpv播放器通用教程》](https://hooke007.github.io/unofficial/mpv_start.html)。

## 下载

[**🏷 Releases 🏷**](../../releases) 提供 `mpv_PlayKit-dlssnr-<版本>-full.zip` 完整整包(portable_config 全目录 + 插件 + 面板 + 模型)。

## 包内容

| 文件 | 说明 |
|---|---|
| `portable_config\` | 完整配置目录(官方发行配置,已接入 DLSSNR 键位与菜单) |
| `vs-plugins\vs_dlssnr.dll` | VapourSynth API4 插件(mpv-lazy 会自动加载) |
| `vs-plugins\dlssnr_panel.exe` | ImGui 独立调参面板(可选,滤镜加载时自动拉起) |
| `vs-plugins\ngx\nvngx_dlssnr.dll` | DLSSNR 模型(必须位于 `ngx\` 子目录,插件按此相对路径寻找) |
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
  - 参数改动实时生效;**「保存为默认值」** 写入 `vs-plugins\dlssnr_ui.ini`,下次加载滤镜自动生效
  - **ini 优先于 vpy 参数**;删除 `dlssnr_ui.ini` 可恢复脚本默认值
  - 面板随滤镜关闭 / mpv 退出自动退出

## 参数(DLSSNR_NV.vpy)

| 参数 | 范围 | 默认 | 说明 |
|---|---|---|---|
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
| `H_Max` | 整数 | 0 | 输出高度限制(源超过该值跳过处理;0 = 不限制) |

调参参考:100% 开缩放 ≈ 关闭缩放(multiplier = 1 时等价);降档位省不了多少帧耗(NGX 固定成本主导),主要影响高频细节,1080p 内容建议 50–75%,25% 仅极端省电场景。

## 性能参考(实测,RTX 3080)

- 1080p:插件内部 ≈16.5ms/帧(≈60fps 能力;pack 2.9 + NGX CPU 0.5 + GPU 10.5 + unpack 2.2)
- 首帧 init ~1s(165MB 模型加载)
- 诊断/测速开关(环境变量):`VSDLSSNR_TIMING=1` 每帧分段耗时写日志(测性能用它,勿用 python 端到端数字)、`VSDLSSNR_D3D12_DEBUG=1` 开 D3D12 debug layer、`VSDLSSNR_NGX_LOG=1` 接 NGX 内部日志、`VSDLSSNR_SKIP_EVAL=1` 跳过推理测管线成本

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

依赖自动落地(`fetch-deps.ps1` 从公开源拉取:NVIDIA/DLSS 官方仓库的头文件 + 静态库、VapourSynth R73 头、ImGui 1.91.9b),无手工下载:

```powershell
# 1. 拉取依赖 (需 pwsh + curl)
pwsh -File native\scripts\fetch-deps.ps1

# 2. 编译 (MSVC x64, /MT 静态 CRT; 需 Visual Studio + CMake 4.0+)
powershell -File native\scripts\build.ps1          # 产物: native\bin\{vs_dlssnr.dll, dlssnr_panel.exe}

# 3. 打包整包 (必须在 dlssnr 分支; 模型 nvngx_dlssnr.dll 需先放入 native\vendor\ngx\)
pwsh -File native\scripts\package.ps1 [-Version 0.2.0]   # 产物: native\dist\mpv_PlayKit-dlssnr-<版本>-full.zip
```

移植清单、架构说明与踩坑记录见 [docs/PORTING.md](docs/PORTING.md)。

## 致谢与许可

- 上游 [hooke007/mpv_PlayKit](https://github.com/hooke007/mpv_PlayKit) 与 [mpv-lazy](https://github.com/hooke007/mpv_PlayKit/releases) — 本 fork 的配置基线
- [Blinue/Magpie](https://github.com/Blinue/Magpie) — DLSSNR 参考实现
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS)、[VapourSynth](https://github.com/vapoursynth/vapoursynth)、[Dear ImGui](https://github.com/ocornut/imgui)

许可沿用上游,见 [LICENSE.MD](LICENSE.MD)。
