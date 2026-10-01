# 过度构建清理:裁定记录(2026-10)

> 用途:2026-10-02 全量过度构建审查(提交 665c40e..4881b94,共 7 个)的"裁定不做 / 刻意保留"清单。
> **下次审查或清理前先读这份** —— 以下条目均已审过并有意保留,不要再当发现重报;每条附重审条件。
> 风格与 PORTING.md 一致:只记结论,不记过程。

---

## 一、刻意保留(审过,有意不做)

### 1. capability 预检"双查"(dlssnr_context 2b 段 + RtxVsr/HdrContext::Initialize)

- 审查曾建议合一,读码后裁定**不合并**:2b 段查 `VSR_Available/TrueHDR_Available` 的同时把 capability 参数块缓存进 `_vsrParams/_hdrParams` 供 4c 段创建 feature 消费;几何降级必须发生在槽资源创建**之前**;Initialize 层复查携带 `FeatureInitResult` 精确归因(写进 `_rtxDetail`)。两查服务不同阶段,不是纯重复。
- 重审条件:仅当 2b 改为延迟取参数块、且 FeatureInitResult 归因信息能完整上移时才可合一。动前必须过 `test_rtx_hdr_diag` 三场景。

### 2. ScaleOutputDuration 的浮点 `_Duration` 回退(plugin.cpp)

- `--input-ipc-server`/整数分数对(`_DurationNum/_DurationDen`)缺失时的浮点路径注释写"非 mpv 宿主"。裁定**保留**:mpv→VS 桥的时长属性形态不由本插件控制,这是 FG 帧节拍的 6 行保险;删了若某路径只给浮点 `_Duration`,插帧节拍静默坏,收益为零风险不为零。

### 3. `VSDLSSNR_NR_FORMAT=fp16/bgra8` env 强制覆盖(d3d12_context::NrColorFormat)

- 为"将来验证"预留的钩子,表面是投机抽象。裁定**保留**:与 `VSDLSSNR_DEBUG/DUMP/PROBE` 同属诊断 env 门族(1b72b95"全链探针"的既定惯例),且是管线色 FP16 化(248b9e4)全矩阵验证的真实工具。

### 4. OfStageFrame 接口的 NVOF 专属参数(`inputWrittenByPostCopy` / `gateOut`,of_backend.h)

- 只有 NVOF 实现消费,FFX 侧 `(void)` 丢弃——接口承载单实现语义。裁定**保留**:光流后端接口化本身是既定意图(a1cc363),两参数表达 ping-pong 输入与门锁移交的真实契约差异,FFX 的丢弃是显式且注释诚实的。
- 重审条件:出现第三个光流后端时,把 NVOF 专属面下沉到 `Kind()==kOfBackendNvof` 的专属通道。

### 5. panel_ipc.h 契约占位字段(`uiCorrection` / `preset`)

- v13 退役后恒写 1 / 预设下拉删除后面板不再暴露,纯占位。裁定**保留**:布局哨兵(头注释明示"退役保留…防布局漂移",字段序 = payload 二进制布局),vpy 侧还有哨兵消费。
- 重审条件:**下一个 IPC magic bump 窗口**,与 testkit `panel_ipc.py` 结构体镜像同步删除,单独提交。

### 6. ini 布尔键宽容解析(`true/false/yes/no/on/off`,dlssnr_ini.h ReadIniBool)

- ini 由机器写入恒 0/1,宽容拼写表面是多余防御。裁定**保留**:`GetPrivateProfileInt` 对非数字返回 0,手编 `"true"` 曾静默关掉功能——10 行换来的手编容错有真实坑背书。(两节各抄一份的重复已合并为 ReadIniBool 单点。)

### 7. hint_status.py 独立脚本

- 与 hdr_tag_manual 的 status 输出部分重叠。裁定**保留转正**:用途不同(一行命令读 mpv HDR 上屏四层实态 vs 打/摘标);已补 README 脚本表条目并纳入 `VSDLSSNR_MPV_PIPE` 管道约定。

---

## 二、本次未动(有外部原因,非裁定)

### 8. fetch-deps.ps1 头文件清单裁剪

- 现拉取清单含约 9 个零 include 的头(VK/dlssd 族 + `nvOpticalFlowD3D11.h/nvOpticalFlowCuda.h`;本工程 D3D12-only,实际消费的 NGX 头仅 6 个)。**未做原因**:该文件当时有未提交的暂存改动,不往里掺;裁剪零风险但收益仅下载量。
- 重审条件:任意时刻可做,注意保留 `nvOpticalFlowCommon.h`(被 D3D12 头间接包含)。

---

## 三、审查误报纠正(记录防复审再报)

- **d3d12 描述符槽 32/33 占位视图**:不是死代码,是防御——堆内绝不留 NULL 描述符(有 TDR 前科,见 d3d12_context.cpp 描述符块注释)。已做的只是把 FG/非 FG 两分支统一为恒绑 outputColor 占位。
- **帧序门"双份"**(of_frame_gate.h vs NvofContext 内联门):当时内联保留的理由("NVOF 是唯一在役链路")已随 FFX 转正默认后端失效,已于 113fd14 迁移归一——此类"注释自辩的理由已过时"模式值得复审时重点核对。

---

## 四、清理范围与验证记录

- 清理提交:`665c40e`(零风险死代码批删)/ `41745ef`(RtxVideoParams 瘦身 + ini 解析合并 + vsrAutoHeight 哨兵修复)/ `85c6413`(恒空参数/GridSize 死字段/SEH 门·栅栏等待·自愈三处合一)/ `113fd14`(帧序门迁移 OfFrameGate)/ `ce491ed`(testkit 三套重复收编)/ `08b3b54`(测试修正)/ `4881b94`(dump footprint 失配根修)。
- 验证:每批 MSVC 全量编译绿;部署(d:\Portable\mpv-lazy)实跑 **8 项测试全 PASS**——verify_stats_keys、test_nvof_switch、test_alloff_state、test_rtx_hdr_diag(hdr/vsr/vsrhdr)、test_format_coverage(7 格式)、test_hdr_hint_sync、verify_pq_roundtrip(luma max|Δcode| 1.16)、verify_chroma_forward(pq.r−y ≈ 0.0001)。
- 附带收获:根修既有 dump bug(FFX 输入 footprint 跨族毒化 ctl 命令列表;同款"调用方传错 footprint"第三次踩,已随 desc 自取根绝;A/B 对照实证与清理无关)。
