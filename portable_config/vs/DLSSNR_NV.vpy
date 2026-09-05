### NVIDIA DLSSNR 同分辨率 AI 画质增强 <RTX N卡专用>
### 依赖_ vs-plugins\vs_dlssnr.dll (VapourSynth 插件, 自动加载)
###       vs-plugins\ngx\nvngx_dlssnr.dll (模型文件, 310.9.0)
###       vs-plugins\dlssnr_panel.exe (控制面板, 可选; 滤镜加载时自动启动)
### 说明_ 移植自 Magpie experimental 的 DLSSNR (NGX Feature 18), 零 guidance 单帧模式
###       处理为同分辨率 (非超分), SDR 色彩空间 (非 HDR)
### 调参_ 播放时托盘点击 DLSSNR 图标打开控制面板, 实时调节 (不写回本文件)
###       本文件显式设置的参数优先于面板保存的 dlssnr_ui.ini

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
Scaling_Enabled = True
Input_Resolution = 100
Residual_Multiplier = 1.0
H_Max = 0
## <0-3> 整数, NR 预设档位
## <0-2> 整数, 风格档位 (0 默认 / 1 自然 / 2 电影)
## <0-2> 浮点, 强度 (默认 1.0)
## <0-2> 浮点, 局部色调强度 (默认 1.0)
## <0-2> 浮点, 局部结构强度 (默认 1.0)
## <-1-2> 浮点, 皮肤结构强度 (-1 = 自动, 默认)
## <True|False> 使用自动蒙版
## <True|False> UI 文字修正
## <True|False> 内部分辨率缩放总开关 (默认 True)
##          False = 彻底关闭残差管线: 不建中间纹理/compute, 按源尺寸直接处理,
##          且 Input_Resolution 被忽略; 需要零缩放开销时用
## <25-100> 整数, NGX 内部推理分辨率占源尺寸的百分比 (默认 100);
##          由 Lanczos3 残差重建恢复到源分辨率; 降低档位减少推理量与细节,
##          但 NGX 固定成本主导, 实测帧耗差异有限
##          注意 100% 仍走残差通道 (Magpie 行为), residual_multiplier > 1 时增强细节
## <1-2> 浮点, 残差乘数 (默认 1.0), 配合内部分辨率缩放使用
## 整数, 输出高度限制 (源高度超过该值时跳过处理; 0 = 不限制)
## 注意_ 4K 源建议配合 Input_Resolution 50 使用, 否则帧率过低

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
		scaling_enabled = int(Scaling_Enabled),
		input_resolution = Input_Resolution,
		residual_multiplier = Residual_Multiplier,
	)
	ret = core.resize.Bilinear(clip=rgb, format=clip.format.id, matrix_s="709")

ret.set_output()
