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
###       YUV 原生: 直接吃 YUV420P8/P10 吐同格式, 无 CPU 格式往返;
###       色彩矩阵/范围按源帧属性 (_Matrix/_ColorRange) 处理, 缺省 709 limited
###       HDR (BT.2020/PQ/HLG) 与非 420P8/P10 内容自动直通 (正常播放, 不做增强)
### 调参_ 播放时托盘点击 DLSSNR 图标打开控制面板, 实时调节; 保存后下次自动生效

import vapoursynth as vs
from vapoursynth import core

############
# 用户选项 #
############

NR_Enabled = True
Preset = 0
Style = 0
Intensity = 1.0
Local_Tone = 1.0
Local_Structure = 1.0
Skin_Structure = 0
Use_Auto_Mask = True
Scaling_Enabled = True
Input_Resolution = 100
Residual_Multiplier = 1.0
Motion_Vector_Quality = 0
Ffx_Quality = 0
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
## <True|False> 降噪总开关 (默认 True): False = 跳过降噪推理输出源帧,
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
## <True|False> 内部分辨率缩放总开关 (默认 True)
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
## <0-2> 整数, FFX 光流档位 (默认 0): 0 = 无; 1 = 性能 (半分辨率),
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
##          mpv 侧建议 target-colorspace-hint=yes 上屏 HDR (SDR 屏自动色调映射)
##          依赖 vs-plugins\ngx\nvngx_truehdr.dll; HDR 源内容仍走 vpy 直通守卫
## <0-200> 整数, HDR 对比度 (默认 100)
## <0-200> 整数, HDR 饱和度 (默认 100)
## <10-100> 整数, HDR 中间灰 (默认 50)
## <400-2000> 整数, HDR 峰值亮度 nits (默认 1000)
## 整数, 输出高度限制 (源高度超过该值时跳过处理; 0 = 不限制)
## 注意_ 4K 源建议配合 Input_Resolution 50 使用, 否则帧率过低

############

clip = video_in

# HDR 守卫:BT.2020 矩阵(_Matrix=9)或 PQ/HLG 传输(_Transfer=16/18)的
# 内容按 SDR 709 处理必错色 —— 直通。属性缺失(裸流)时无法判定,按 SDR
# 处理(与旧行为一致)。
def _is_hdr(c) :
	try :
		f = c.get_frame(0)
		props = f.props
		m = props.get("_Matrix")
		t = props.get("_Transfer")
		return (m is not None and int(m) == 9) or (t is not None and int(t) in (16, 18))
	except Exception :
		return False

if H_Max and clip.height > H_Max :
	ret = clip
elif _is_hdr(clip) :
	ret = clip
elif clip.format.id not in (vs.YUV420P8, vs.YUV420P10) :
	# 非 420P8/P10 (422/444/RGB 等) 直通: 滤镜只收这两种格式
	ret = clip
else :
	ret = core.dlssnr.Enhance(
		clip,
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

ret.set_output()
