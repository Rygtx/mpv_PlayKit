### NVIDIA DLSSNR 同分辨率 AI 画质增强 <RTX N卡专用>
### 依赖_ vs-plugins\vs_dlssnr.dll (VapourSynth 插件, 自动加载)
###       vs-plugins\ngx\nvngx_dlssnr.dll (模型文件, 310.9.0)
### 说明_ 移植自 Magpie experimental 的 DLSSNR (NGX Feature 18), 零 guidance 单帧模式
###       处理为同分辨率 (非超分), SDR 色彩空间 (非 HDR)
### 参数_ preset 0-3 / style 0-2 / intensity 0-2 / 详细见 DLSSNR_AI_Filter 说明

import vapoursynth as vs
from vapoursynth import core

############
# 用户选项 #
############

Preset = 0
Style = 0
Intensity = 1.0
Local_Tone = 1.0
Local_Structure = 1.0
Skin_Structure = -1
Use_Auto_Mask = True
UI_Correction = True
H_Max = 1440
## <0-3> 整数, NR 预设档位
## <0-2> 整数, 风格档位
## <0-2> 浮点, 强度 (默认 1.0)
## <0-2> 浮点, 局部色调强度 (默认 1.0)
## <0-2> 浮点, 局部结构强度 (默认 1.0)
## <-1-2> 浮点, 皮肤结构强度 (-1 = 自动, 默认)
## <True|False> 使用自动蒙版
## <True|False> UI 文字修正
## 整数, 输出高度限制 (填你的显示器高度), 源高度超过该值时跳过处理 (0 = 不限制)

############

clip = video_in

if H_Max and clip.height > H_Max :
	ret = clip
elif clip.format.color_family != vs.YUV :
	ret = clip
else :
	rgb = core.resize.Bilinear(clip=clip, format=vs.RGBS, matrix_in_s="709")
	rgb = core.dlssnr.Enhance(
		rgb,
		preset = Preset,
		style = Style,
		intensity = Intensity,
		local_tone = Local_Tone,
		local_structure = Local_Structure,
		skin_structure = Skin_Structure,
		use_auto_mask = int(Use_Auto_Mask),
		ui_correction = int(UI_Correction),
	)
	ret = core.resize.Bilinear(clip=rgb, format=clip.format.id, matrix_s="709")

ret.set_output()
