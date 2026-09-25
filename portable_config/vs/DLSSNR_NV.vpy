### NVIDIA DLSSNR 同分辨率 AI 画质增强 <RTX N卡专用>
### 依赖_ vs-plugins\vs_dlssnr.dll (VapourSynth 插件, 自动加载)
###       vs-plugins\ngx\nvngx_dlssnr.dll (模型文件, 310.9.0, 取自 RenoDX)
###       vs-plugins\ngx\version.dll (DLSS FG hook 代理 0.3.x, dlssg_for_sm86; 帧生成可选, 用户自备)
###       vs-plugins\ngx\nvngx_vsr.dll / nvngx_truehdr.dll (RTX Video SDK 1.1, VSR/HDR 可选, fetch-deps 自动)
###       vs-plugins\dlssnr_panel.exe (控制面板, 可选; 滤镜加载时自动启动)
### 说明_ 移植自 Magpie experimental 的 DLSSNR (NGX Feature 18)
###       处理为同分辨率 (非超分); 默认零 guidance, 可选硬件光流引导 (0-5 档)
###       DLSS 帧生成 (可选): 挂降噪之后, 输出帧率 x2 (插值帧由 DLSS FG 模型合成);
###       需要光流引导 (Motion_Vector_Quality > 0) 才有意义, 否则插值帧 = 复制真实帧
###       YUV 原生直吃: 计划族 {420,422,444} × 8-16bit 直传滤镜, 无 CPU
###       格式往返; GRAY/packed RGB 由 709 矩阵转 YUV444 兜底;
###       色彩矩阵/范围按源帧属性处理, 缺省 709 limited
###       HDR (BT.2020/PQ/HLG) 照常处理 (NGX NR 不感知传输函数, 矩阵 709
###       近似往返一致; 原直通守卫已撤)
### 调参_ 播放时托盘点击 DLSSNR 图标打开控制面板, 实时调节; 保存后下次自动生效

import vapoursynth as vs
from vapoursynth import core

############
# 用户选项 #
############

NR_Enabled = False
Preset = 0
Style = 0
Intensity = 1.0
Local_Tone = 1.0
Local_Structure = 1.0
Skin_Structure = 0
Use_Auto_Mask = True
Scaling_Enabled = False
Input_Resolution = 100
Residual_Multiplier = 1.0
Motion_Vector_Quality = 0
Ffx_Quality = 2
Nvof_Follow_Scaling = False
Of_Backend = 0
Fg_Enabled = False
Fg_Multiplier = 2
Fg_Route = 0
VSR_Mode = 1
VSR_Scale = 2.0
VSR_Strength = 2
HDR_Enabled = False
HDR_Contrast = 100
HDR_Saturation = 100
HDR_MiddleGray = 50
HDR_PeakNits = 1000
H_Max = 0
## <True|False> 降噪总开关 (默认 False): False = 跳过降噪推理输出源帧,
##          补帧/光流照常工作不受影响; 降噪与帧生成都关时整个滤镜
##          零初始化零开销 (重开需下个 seek); 已激活会话内面板切换
##          立即生效; 面板 "降噪增强" 开关同步此值, 面板保存的设置优先
## <0-3> 整数, NR 预设档位
## <0-2> 整数, 风格档位 (0 默认 / 1 自然 / 2 电影)
## <0-2> 浮点, 强度 (默认 1.0)
## <0-2> 浮点, 局部色调强度 (默认 1.0)
## <0-2> 浮点, 局部结构强度 (默认 1.0)
## <0-2> 浮点, 皮肤结构强度 (默认 0; 上游 beta3 起 -1=auto 已移除)
## <True|False> 使用自动蒙版
## <True|False> 内部分辨率缩放总开关 (默认 False)
##          False = 彻底关闭残差管线: 不建中间纹理/compute, 按源尺寸直接处理,
##          且 Input_Resolution 被忽略; 需要零缩放开销时用
## <25-100> 整数, NGX 内部推理分辨率占源尺寸的百分比 (默认 100);
##          由 Catmull-Rom 残差重建恢复到源分辨率; 降低档位减少推理量与细节,
##          但 NGX 固定成本主导, 实测帧耗差异有限
##          注意 100% 仍走残差通道 (Magpie 行为), residual_multiplier > 1 时增强细节
## <1-2> 浮点, 残差乘数 (默认 1.0), 配合内部分辨率缩放使用
## <0-5> 整数, NVOF 光流 (真运动矢量) 引导档位 (默认 0; 仅后端 = NVOF 时消费)
##          0 = 无 (零 guidance); 1-5 用硬件光流减轻运动场景的时域伪影
##          后端: FFX (AMD FidelityFX, 跨厂商通用, 默认, 档位见 Ffx_Quality) /
##          NVOF (NVIDIA Turing+, 面板"光流后端"切换)
##          播放中可在面板实时切换 (只重建光流会话, 毫秒级)
##          帧生成开启时建议 >= 2 (Balanced): 光流是插值帧的运动来源
## <0-2> 整数, FFX 光流档位 (默认 2): 0 = 无; 1 = 性能 (半分辨率),
##          2 = 质量 (全分辨率); 面板"光流质量"按当前后端读写对应档位
## <True|False> 光流输入跟随内部降采样 (默认 False); 帧生成激活时被忽略
## <0-1> 整数, 光流后端 (默认 0): 0 = FFX (AMD FidelityFX, 跨厂商通用);
##          1 = NVOF (NVIDIA 专属引擎); 面板"光流后端"同步此值
##          注意: 以上四参的注释顺序与变量顺序一致
##          (Motion_Vector_Quality → Ffx_Quality → Nvof_Follow_Scaling → Of_Backend)
## <True|False> DLSS 帧生成 (默认 False)
##          挂降噪之后: 每源帧产出 M 帧, 插值帧由 DLSS FG 模型合成,
##          落在相邻两真实帧之间; 依赖 ngx\version.dll (dlssg_for_sm86
##          0.3.x hook 代理, 自动档预载接管 DLSS-G),
##          初始化失败自动回退 1:1, 降噪不受影响
## <2-6> 整数, 插帧倍数 (默认 2): 每源帧产出 M 帧, 输出帧时长 = 源时长/M
##          24fps 源 x3 = 72fps, 显示端建议刷新率 >= 输出帧率
##          运行库上限默认开到 6x (部署 ini MaxGeneratedFrames=5),
##          改倍数原地重载即时生效
##          输出帧数/节奏随创建档位定格; 面板改档位/开关会自动触发 mpv
##          原地重载即时生效 (需 mpv.conf 启用 input-ipc-server; 未启用时
##          关/降档退化为会话内复制真实帧, 升档需手动 seek)
## <0-1> 整数, FG 路由 (默认 0): 0 = 自动 (按 GPU 族分流: RTX 30/20 系预载
##          dlssg_for_sm86 0.3.x hook 代理由其接管 DLSS-G, 仍走官方签名链;
##          RTX 40 系走官方运行库, 插件做 mfg gate 解锁放行 3-6x
##          [仅 Ada 触发, 50 系原生多帧不碰], 未命中回落 2x),
##          1 = 纯官方 (不预载代理, 直连官方运行时, 同样解锁;
##          30/20 系被拒载不回落)
##          进程级, 切换后重启 mpv 生效; 面板保存的设置优先
## <0-2> 整数, RTX Video 超分 (NVIDIA RTX Video SDK 1.1 的 NGX VSR, 默认 1):
##          0 = 关; 1 = 自动 (插件直读 mpv 窗口客户区 = osd-dimensions,
##          按实际显示矩形适配并 clamp 到所在显示器; 源更高时自动旁路
##          —— VSR 只做放大); 2 = 手动倍率 (VSR_Scale)
##          挂降噪之后: 源尺寸降噪 → VSR 放大 → (可选 HDR) → 帧生成,
##          temporal 模型全部只吃真实源帧; 输出帧尺寸 = 目标尺寸
##          依赖 vs-plugins\ngx\nvngx_vsr.dll (fetch-deps.ps1 自动拉取);
##          需要 RTX 20 系+ 与 r550.58+ 驱动, capability 不过自动降级直通
##          窗口换屏/改尺寸后的生效点 = 下一次链重建 (seek/换片)
## <1.0-4.0> 浮点, VSR 手动放大倍率 (VSR_Mode=2 时消费; 默认 2.0;
##          官方单 pass 上限 4x)
## <1-4> 整数, VSR 质量档 (默认 2): 1-4 = AI 档 (4 = Ultra 最优最慢);
##          关闭走 VSR_Mode=0, 官方 bicubic 0 档不暴露;
##          >4x 放大先 VSR 到 4x 中间尺寸再线性收尾
## <True|False> RTX Video HDR (NGX TrueHDR, SDR→HDR, 默认 False):
##          挂 VSR 之后 (官方契约: TrueHDR 不能吃 HDR 输入, 必须在 VSR 后);
##          输出切换 YUV420P10 BT.2020 PQ, HDR10 元数据随帧 props,
##          HDR 上屏(打标 + 色彩空间 hint)由面板自动同步, 无需改 mpv.conf
##          (SDR 屏自动色调映射)
##          依赖 vs-plugins\ngx\nvngx_truehdr.dll; HDR 源内容照常走全链
## <0-200> 整数, HDR 对比度 (默认 100)
## <0-200> 整数, HDR 饱和度 (默认 100)
## <10-100> 整数, HDR 中间灰 (默认 50)
## <400-2000> 整数, HDR 峰值亮度 nits (默认 1000)
## 整数, 输出高度限制 (源高度超过该值时跳过处理; 0 = 不限制)
## 注意_ 4K 源建议配合 Input_Resolution 50 使用, 否则帧率过低

############

clip = video_in

# 原生直吃(2026-09-24,vpy 预转换退役):插件已原生收编 YUV 计划族
# {420,422,444} × 8/10/12/14/16bit,直传 Enhance,不做任何预转换。
# HDR(BT.2020/PQ/HLG)照常处理 —— NGX NR 不感知传输函数,矩阵 709
# 近似往返一致。本部署 VS(R73)无计划 RGB,packed RGB/GRAY 等由
# 709 矩阵转 YUV444 兜底(转换失败原样直通)。
def _to_native(c) :
	f = c.format
	bits = 16 if f.sample_type == vs.FLOAT else f.bits_per_sample
	# 本部署 VS(R73)无计划 RGB(RGBP* 缺,RGB 恒 packed 单平面),mpv→VS
	# 桥对 RGB 源也先行转 YUV —— packed RGB/GRAY 统一 709 矩阵转 YUV444。
	out_fmt = vs.YUV444P10 if bits > 8 else vs.YUV444P8
	conv = c.resize.Bicubic(format=out_fmt, matrix_s="709") \
		if f.color_family == vs.RGB else c.resize.Bicubic(format=out_fmt)
	# resize 不透传帧 props,DLL 读 _ColorRange 判 limited/full:从首帧
	# 搬关键字段回填(属性流内恒定,静态回填足够)。_Primaries/_Transfer
	# 一并回填(2026-09-25:缺它们时 RTX TrueHDR/HDR 语义不完整)。
	try :
		props = c.get_frame(0).props
		keep = {k : int(props[k]) for k in ("_ColorRange", "_Matrix",
		                                    "_Primaries", "_Transfer") if k in props}
		if keep :
			conv = conv.std.SetFrameProps(**keep)
	except Exception :
		pass
	return conv

if H_Max and clip.height > H_Max :
	ret = clip
else :
	f = clip.format
	# native 收编面 = 插件原生支持面(YUV 计划族 × {420,422,444} × 8-16bit)。
	# 2026-09-25 删恒假 RGB 分支:本部署 VS(R73)无计划 RGB(RGBP* 缺,RGB
	# 恒 packed 单平面),num_planes==3 的 RGB 恒不出现;且插件收编面只有
	# YUV —— 若该分支命中会直传 Enhance 而插件不收,反而破坏直通兜底。
	native = (f.color_family == vs.YUV
	          and f.sample_type == vs.INTEGER
	          and 8 <= f.bits_per_sample <= 16
	          and f.subsampling_w <= 1 and f.subsampling_h <= 1)
	try :
		proc = clip if native else _to_native(clip)
	except Exception :
		proc = clip  # 极冷门族转换失败:回退原样,由下方格式门直通
	if native or proc.format.id in (vs.YUV444P10, vs.YUV444P8) :
		ret = core.dlssnr.Enhance(
			proc,
		nr_enabled = int(NR_Enabled),
		preset = Preset,
		style = Style,
		intensity = Intensity,
		local_tone = Local_Tone,
		local_structure = Local_Structure,
		skin_structure = Skin_Structure,
		use_auto_mask = int(Use_Auto_Mask),
		scaling_enabled = int(Scaling_Enabled),
		input_resolution = Input_Resolution,
		residual_multiplier = Residual_Multiplier,
		motion_vector_quality = Motion_Vector_Quality,
		ffx_quality = Ffx_Quality,
		nvof_follow_scaling = int(Nvof_Follow_Scaling),
		of_backend = Of_Backend,
		fg_enabled = int(Fg_Enabled),
		fg_multiplier = Fg_Multiplier,
		fg_route = int(Fg_Route),
		vsr_mode = int(VSR_Mode),
		vsr_scale = float(VSR_Scale),
		vsr_strength = int(VSR_Strength),
		hdr_enabled = int(HDR_Enabled),
		hdr_contrast = int(HDR_Contrast),
		hdr_saturation = int(HDR_Saturation),
		hdr_middle_gray = int(HDR_MiddleGray),
		hdr_peak_nits = int(HDR_PeakNits),
	)
	else :
		ret = clip  # 无法收编(Alpha 族等):原样直通,不挂脚本

ret.set_output()
