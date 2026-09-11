### NVIDIA DLSSNR 同分辨率 AI 画质增强 <RTX N卡专用>
### 依赖_ vs-plugins\vs_dlssnr.dll (VapourSynth 插件, 自动加载)
###       vs-plugins\ngx\nvngx_dlssnr.dll (模型文件, 310.9.0, 取自 RenoDX)
###       vs-plugins\ngx\version.dll (DLSS FG 原生代理, dlssg_for_sm86; 帧生成可选, 用户自备)
###       vs-plugins\dlssnr_panel.exe (控制面板, 可选; 滤镜加载时自动启动)
### 说明_ 移植自 Magpie experimental 的 DLSSNR (NGX Feature 18)
###       处理为同分辨率 (非超分); 默认零 guidance, 可选 NVOF 硬件光流引导 (0-5 档)
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
Skin_Structure = -1
Use_Auto_Mask = True
UI_Correction = True
Scaling_Enabled = True
Input_Resolution = 100
Residual_Multiplier = 1.0
Motion_Vector_Quality = 0
Nvof_Follow_Scaling = False
Fg_Enabled = False
Fg_Multiplier = 2
Fg_Route = 0
H_Max = 0
## <True|False> 降噪总开关 (默认 True): False = 跳过降噪推理输出源帧,
##          补帧/光流照常工作不受影响; 降噪与帧生成都关时整个滤镜
##          零初始化零开销 (重开需下个 seek); 已激活会话内面板切换
##          立即生效; 面板 "降噪增强" 开关同步此值, 面板保存的设置优先
## <0-3> 整数, NR 预设档位
## <0-2> 整数, 风格档位 (0 默认 / 1 自然 / 2 电影)
## <0-1> 浮点, 强度 (默认 1.0)
## <0-1> 浮点, 局部色调强度 (默认 1.0)
## <0-1> 浮点, 局部结构强度 (默认 1.0)
## <-1-2> 浮点, 皮肤结构强度 (-1 = 自动, 默认)
## <True|False> 使用自动蒙版
## <True|False> UI 文字修正
## <True|False> 内部分辨率缩放总开关 (默认 True)
##          False = 彻底关闭残差管线: 不建中间纹理/compute, 按源尺寸直接处理,
##          且 Input_Resolution 被忽略; 需要零缩放开销时用
## <25-100> 整数, NGX 内部推理分辨率占源尺寸的百分比 (默认 100);
##          由 Catmull-Rom 残差重建恢复到源分辨率; 降低档位减少推理量与细节,
##          但 NGX 固定成本主导, 实测帧耗差异有限
##          注意 100% 仍走残差通道 (Magpie 行为), residual_multiplier > 1 时增强细节
## <1-2> 浮点, 残差乘数 (默认 1.0), 配合内部分辨率缩放使用
## <0-5> 整数, NVIDIA 光流 (真运动矢量) 引导档位 (默认 0)
##          0 = 无 (零 guidance); 1-5 用硬件光流减轻运动场景的时域伪影
##          需要 NVIDIA Turing+; 不支持时自动回退零 guidance
##          播放中可在面板实时切换 (只重建光流会话, 毫秒级)
##          帧生成开启时建议 >= 2 (Balanced): 光流是插值帧的运动来源
## <True|False> 光流输入跟随内部降采样 (默认 False); 帧生成激活时被忽略
## <True|False> DLSS 帧生成 (默认 False)
##          挂降噪之后: 每源帧产出 M 帧 (1 真实 + M-1 插值, 插值帧由
##          DLSS FG 模型合成); 依赖 ngx\version.dll (dlssg_for_sm86),
##          初始化失败自动回退 1:1, 降噪不受影响
## <2-4> 整数, 插帧倍数 (默认 2): 每源帧产出 M 帧, 输出帧时长 = 源时长/M
##          面板可实时改 (当前源帧播完切换); 24fps 源 x3 = 72fps,
##          显示端建议刷新率 >= 输出帧率
##          面板开关: 会话内关 = 插值帧改复制真实帧 (帧数不变);
##          未激活会话内开 = 下次播放生效
## <0-3> 整数, FG 路由 (默认 0): 0 = 自动 (官方优先, 不可用回落代理 SM86),
##          1 = SM86 (RTX 30 系走代理), 2 = SM75 (RTX 20 系走代理),
##          3 = 官方 NGX (RTX 40/50 系, 30/20 系被拒载不回落)
##          插件自动把路由写入 ngx\dlssg_sm86.ini (无需手动改文件);
##          进程级, 切换后重启 mpv 生效; 面板保存的设置优先
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
		ui_correction = int(UI_Correction),
		scaling_enabled = int(Scaling_Enabled),
		input_resolution = Input_Resolution,
		residual_multiplier = Residual_Multiplier,
		motion_vector_quality = Motion_Vector_Quality,
		nvof_follow_scaling = int(Nvof_Follow_Scaling),
		fg_enabled = int(Fg_Enabled),
		fg_multiplier = Fg_Multiplier,
		fg_route = int(Fg_Route),
	)

ret.set_output()
