#pragma once
// d3d12_context 的全部 HLSL compute shader(2026-10-04 自 d3d12_context.cpp
// 抽离:原 ~1430 行字面量占实现文件约三成,与编排逻辑无关,只被
// CreateComputeObjects 的 PSO 创建消费。常量形态/匿名命名空间(internal
// linkage)与原位一致;改 shader 仍在本文件,无需再翻 4.5K 行实现。
#include "d3d12_context.h"

namespace vsdlssnr {
namespace {


// Upstream COLOR_DOWNSAMPLE_HLSL verbatim: vertical pass writes the shared
// FP16 intermediate (negative lobes survive), horizontal pass lands the
// reduced color in the NGX input texture.
constexpr char DOWNSAMPLE_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

float Sinc(float x) {
    if (abs(x) < 1e-5) return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}

float Lanczos2(float x) {
    return abs(x) < 2.0 ? Sinc(x) * Sinc(x * 0.5) : 0.0;
}

[numthreads(8, 8, 1)]
void DownsampleColorVertical(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    float scale = float(TargetExtent.y) / float(SourceExtent.y);
    float position = (float(tid.y) + 0.5) / scale - 0.5;
    float support = 2.0 / scale;
    int first = int(ceil(position - support));
    int last = int(floor(position + support));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int y = first; y <= last; ++y) {
        float weight = Lanczos2((float(y) - position) * scale);
        total += InputColor.Load(int3(tid.x,
            clamp(y, 0, int(SourceExtent.y) - 1), 0)) * weight;
        totalWeight += weight;
    }
    // Keep negative lobes in the shared FP16 intermediate, including alpha.
    OutputColor[tid.xy] = total / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0);
}

[numthreads(8, 8, 1)]
void DownsampleColorHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float scale = float(TargetExtent.x) / float(SourceExtent.x);
    float position = (float(tid.x) + 0.5) / scale - 0.5;
    float support = 2.0 / scale;
    int first = int(ceil(position - support));
    int last = int(floor(position + support));
    float4 total = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int x = first; x <= last; ++x) {
        float weight = Lanczos2((float(x) - position) * scale);
        total += InputColor.Load(int3(
            clamp(x, 0, int(SourceExtent.x) - 1), tid.y, 0)) * weight;
        totalWeight += weight;
    }
    OutputColor[tid.xy] = total / (abs(totalWeight) > 1e-6 ? totalWeight : 1.0);
}
)";

constexpr char RESIDUAL_PREPARE_HLSL[] = R"(
Texture2D<float4> ReducedColor : register(t0);
Texture2D<float4> ReducedDenoised : register(t1);
RWTexture2D<float4> ControlledResidual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

float3 RGBToHSL(float3 color) {
    float maximum = max(color.r, max(color.g, color.b));
    float minimum = min(color.r, min(color.g, color.b));
    float delta = maximum - minimum;
    float lightness = (maximum + minimum) * 0.5;
    if (delta <= 1e-6) {
        return float3(0.0, 0.0, lightness);
    }

    float hue = 0.0;
    if (maximum == color.r) {
        hue = (color.g - color.b) / delta;
        if (hue < 0.0) hue += 6.0;
    } else if (maximum == color.g) {
        hue = (color.b - color.r) / delta + 2.0;
    } else {
        hue = (color.r - color.g) / delta + 4.0;
    }
    float saturation = delta / max(1.0 - abs(2.0 * lightness - 1.0), 1e-6);
    return float3(hue / 6.0, saturate(saturation), saturate(lightness));
}

float HueToRGB(float p, float q, float hue) {
    hue = frac(hue);
    if (hue < 1.0 / 6.0) return p + (q - p) * 6.0 * hue;
    if (hue < 1.0 / 2.0) return q;
    if (hue < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - hue) * 6.0;
    return p;
}

float3 HSLToRGB(float3 hsl) {
    if (hsl.y <= 1e-6) {
        return float3(hsl.z, hsl.z, hsl.z);
    }
    float q = hsl.z < 0.5 ?
        hsl.z * (1.0 + hsl.y) : hsl.z + hsl.y - hsl.z * hsl.y;
    float p = 2.0 * hsl.z - q;
    return saturate(float3(
        HueToRGB(p, q, hsl.x + 1.0 / 3.0),
        HueToRGB(p, q, hsl.x),
        HueToRGB(p, q, hsl.x - 1.0 / 3.0)));
}

float3 ToLinear(float3 color) {
    return float3(
        color.r <= 0.04045 ? color.r / 12.92 : pow(max(color.r + 0.055, 0.0) / 1.055, 2.4),
        color.g <= 0.04045 ? color.g / 12.92 : pow(max(color.g + 0.055, 0.0) / 1.055, 2.4),
        color.b <= 0.04045 ? color.b / 12.92 : pow(max(color.b + 0.055, 0.0) / 1.055, 2.4));
}

float3 ApplyResidualControls(float3 original, float3 residual) {
    residual *= ResidualMultiplier;
    if (all(residual == 0.0)) return original;
    float4 fineControls = float4(
        ResidualSaturation, ResidualLightness,
        ShadowStructureMultiplier, ReflectionGlowMultiplier);
    // Neutral fine controls preserve the multiplied residual in this low-resolution domain.
    float3 output = saturate(original + residual);
    [branch]
    if (any(abs(fineControls - 1.0) >= 1e-6)) {
        // Classify the whole pixel before directional/HSL controls. The
        // reference cannot depend on the multiplier selected by this branch.
        float deltaY = dot(ToLinear(output) - ToLinear(original),
            float3(0.2126, 0.7152, 0.0722));
        float directionalMultiplier = deltaY < 0.0 ? ShadowStructureMultiplier :
            (deltaY > 0.0 ? ReflectionGlowMultiplier : 1.0);
        float3 controlledResidual = residual * directionalMultiplier;
        float3 candidate = saturate(original + controlledResidual);
        [branch]
        if (abs(ResidualSaturation - 1.0) >= 1e-6 ||
            abs(ResidualLightness - 1.0) >= 1e-6) {
            // The SRVs are non-sRGB UNORM views, so HSL operates on normalized
            // stored SDR RGB values without an implicit transfer conversion.
            float3 originalHSL = RGBToHSL(original);
            float3 candidateHSL = RGBToHSL(candidate);
            candidateHSL.y = saturate(originalHSL.y +
                (candidateHSL.y - originalHSL.y) * ResidualSaturation);
            candidateHSL.z = saturate(originalHSL.z +
                (candidateHSL.z - originalHSL.z) * ResidualLightness);
            candidate = HSLToRGB(candidateHSL);
        }
        output = candidate;
    }
    return output;
}

[numthreads(8, 8, 1)]
void PrepareResidual(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float3 original = ReducedColor.Load(int3(tid.xy, 0)).rgb;
    float3 denoised = ReducedDenoised.Load(int3(tid.xy, 0)).rgb;
    // Apply every residual control once per low-resolution pixel, before
    // either Catmull-Rom pass. Keep signed differences in an FP16 texture.
    ControlledResidual[tid.xy] = float4(
        ApplyResidualControls(original, denoised - original) - original, 0.0);
}
)";

constexpr char RESIDUAL_HORIZONTAL_HLSL[] = R"(
Texture2D<float4> ControlledResidual : register(t0);
RWTexture2D<float4> HorizontalResidual : register(u0);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

[numthreads(8, 8, 1)]
void UpsampleResidualHorizontal(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SourceExtent.x || tid.y >= TargetExtent.y) return;
    if (SourceExtent.x == TargetExtent.x) {
        HorizontalResidual[tid.xy] =
            ControlledResidual.Load(int3(tid.xy, 0));
        return;
    }
    float reducedPosition = (float(tid.x) + 0.5) *
        float(TargetExtent.x) / float(SourceExtent.x) - 0.5;
    int center = int(floor(reducedPosition));
    float3 residual = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int x = -1; x <= 2; ++x) {
        float weight = CatmullRom(reducedPosition - float(center + x));
        int sampleX = clamp(center + x, 0, int(TargetExtent.x) - 1);
        int3 samplePixel = int3(sampleX, tid.y, 0);
        residual += ControlledResidual.Load(samplePixel).rgb * weight;
        totalWeight += weight;
    }
    residual /= abs(totalWeight) > 1e-6 ? totalWeight : 1.0;
    HorizontalResidual[tid.xy] = float4(residual, 0.0);
}
)";

constexpr char RESIDUAL_VERTICAL_HLSL[] = R"(
Texture2D<float4> OriginalColor : register(t0);
Texture2D<float4> HorizontalResidual : register(t1);
RWTexture2D<float4> OutputColor : register(u0);
cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

float CatmullRom(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

[numthreads(8, 8, 1)]
void CompositeResidualVertical(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float4 storedOriginal = OriginalColor.Load(int3(tid.xy, 0));
    // A typed BGRA SRV already returns logical RGBA components.
    float3 original = storedOriginal.rgb;
    float3 residual = 0.0;
    if (SourceExtent.y == TargetExtent.y) {
        residual = HorizontalResidual.Load(int3(tid.xy, 0)).rgb;
    } else {
        float reducedPosition = (float(tid.y) + 0.5) *
            float(TargetExtent.y) / float(SourceExtent.y) - 0.5;
        int center = int(floor(reducedPosition));
        residual = 0.0;
        float totalWeight = 0.0;
        [unroll]
        for (int y = -1; y <= 2; ++y) {
            float weight = CatmullRom(reducedPosition - float(center + y));
            int sampleY = clamp(center + y, 0, int(TargetExtent.y) - 1);
            residual += HorizontalResidual.Load(
                int3(tid.x, sampleY, 0)).rgb * weight;
            totalWeight += weight;
        }
        residual /= abs(totalWeight) > 1e-6 ? totalWeight : 1.0;
    }
    OutputColor[tid.xy] = float4(
        saturate(original + residual), storedOriginal.a);
}
)";

// Magpie NVOF_Densify(NvidiaOpticalFlowProvider.cpp:15-92)原样移植:S10.5
// 网格光流 + 前后向一致性校验 → 逐像素稠密运动(R16G16_FLOAT)+ 置信度
// (R8_UNORM)。Turing 无 cost 输出时的 0.65 基线保留。
constexpr char DENSIFY_HLSL[] = R"(
Texture2D<int2> ForwardFlow : register(t0);
Texture2D<int2> BackwardFlow : register(t1);
Texture2D<uint> ForwardCost : register(t2);
Texture2D<uint> BackwardCost : register(t3);
RWTexture2D<float2> DenseMotion : register(u0);
RWTexture2D<float> DenseConfidence : register(u1);

cbuffer Params : register(b0) {
    uint2 SourceExtent;   // 稠密目标尺寸(源)
    uint2 FlowExtent;     // 流场网格尺寸(= 会话输入)
    uint HasForwardCost;
    uint HasBackward;
    uint HasBackwardCost;
    float2 MotionScale;   // 流向量单位换算(会话输入像素 → 源像素);未降采样 = (1,1)
};

float2 LoadFlow(Texture2D<int2> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float2(field.Load(int3(p, 0))) / 32.0;
}

float2 SampleFlow(Texture2D<int2> field, float2 sourcePixel) {
    // 源像素 → 流场网格坐标:流场均匀覆盖会话输入范围,会话输入被线性
    // 映射到源范围(MotionScale 同比),故按 FlowExtent/SourceExtent 比例
    // 采样(FlowExtent = ceil(会话输入/GridSize),公式只依赖两尺寸)。
    float2 gridPos = sourcePixel * (float2(FlowExtent) / float2(SourceExtent)) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadFlow(field, p0), LoadFlow(field, p0 + int2(1, 0)), f.x),
        lerp(LoadFlow(field, p0 + int2(0, 1)),
             LoadFlow(field, p0 + int2(1, 1)), f.x),
        f.y);
}

float LoadCost(Texture2D<uint> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float(field.Load(int3(p, 0))) / 255.0;
}

float SampleCost(Texture2D<uint> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel * (float2(FlowExtent) / float2(SourceExtent)) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadCost(field, p0), LoadCost(field, p0 + int2(1, 0)), f.x),
        lerp(LoadCost(field, p0 + int2(0, 1)),
             LoadCost(field, p0 + int2(1, 1)), f.x),
        f.y);
}

[numthreads(8, 8, 1)]
void Densify(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;

    float2 p = float2(tid.xy) + 0.5;
    // 流向量按 MotionScale 换算到源像素单位(下游 MotionScale/NGX 契约);
    // 未降采样时 (1,1),逐位等价旧公式。
    float2 forward = SampleFlow(ForwardFlow, p) * MotionScale;
    // Turing has no hardware cost output. A conservative non-zero baseline
    // still lets downstream consumers use coherent motion while reset frames
    // remain explicitly zero-confidence.
    float confidence = HasForwardCost != 0 ?
        1.0 - SampleCost(ForwardCost, p) : 0.65;

    if (HasBackward != 0) {
        float2 referencePixel = p + forward;
        bool inside = all(referencePixel >= 0.0) &&
            all(referencePixel < float2(SourceExtent));
        float2 backward = SampleFlow(BackwardFlow, referencePixel) * MotionScale;
        float fbError = length(forward + backward);
        float threshold = 0.75 + 0.05 * length(forward);
        confidence *= inside ? saturate(1.0 - fbError / threshold) : 0.0;
        if (HasBackwardCost != 0) {
            confidence *= 1.0 - SampleCost(BackwardCost, referencePixel);
        }
    }

    DenseMotion[tid.xy] = forward;
    DenseConfidence[tid.xy] = saturate(confidence);
}
)";

// Magpie DownsampleGuidance(DLSSNRFilter.cpp:230-291)移植。与上游差异:
// 深度输出被裁掉 —— 本宿主的 depth 恒为零纹理(depth=zero-contract),
// 降采样深度是死重,NGX 直接消费静态零纹理(源尺寸或内部尺寸)。
constexpr char GUIDANCE_DOWNSAMPLE_HLSL[] = R"(
Texture2D<float2> InputMotion : register(t0);
Texture2D<float> InputConfidence : register(t1);
RWTexture2D<float2> OutputMotion : register(u0);
RWTexture2D<float> OutputConfidence : register(u1);

cbuffer ResampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
    float2 MotionScale;
    float ResidualMultiplier;
    float ResidualSaturation;
    float ResidualLightness;
    float ShadowStructureMultiplier;
    float ReflectionGlowMultiplier;
};

[numthreads(8, 8, 1)]
void DownsampleGuidance(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float2 sourceStart = float2(tid.xy) * float2(SourceExtent) /
        float2(TargetExtent);
    float2 sourceEnd = float2(tid.xy + 1) * float2(SourceExtent) /
        float2(TargetExtent);
    int2 first = int2(floor(sourceStart));
    int2 last = int2(ceil(sourceEnd));
    float2 motionTotal = 0.0;
    float2 weightedMotionTotal = 0.0;
    float confidenceTotal = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (int y = first.y; y < last.y; ++y) {
        float weightY = max(0.0, min(sourceEnd.y, float(y + 1)) -
            max(sourceStart.y, float(y)));
        [loop]
        for (int x = first.x; x < last.x; ++x) {
            float weightX = max(0.0, min(sourceEnd.x, float(x + 1)) -
                max(sourceStart.x, float(x)));
            float weight = weightX * weightY;
            int2 sourcePixel = clamp(
                int2(x, y), int2(0, 0), int2(SourceExtent) - 1);
            float2 motion = InputMotion.Load(int3(sourcePixel, 0));
            float confidence = InputConfidence.Load(int3(sourcePixel, 0));
            motionTotal += motion * weight;
            weightedMotionTotal += motion * confidence * weight;
            confidenceTotal += confidence * weight;
            totalWeight += weight;
        }
    }
    float2 motion = confidenceTotal > 1e-6 ?
        weightedMotionTotal / confidenceTotal :
        motionTotal / max(totalWeight, 1e-6);
    OutputMotion[tid.xy] = motion * MotionScale;
    OutputConfidence[tid.xy] = confidenceTotal / max(totalWeight, 1e-6);
}
)";

// 光流输入 GPU 降采样(#46/#48):YUV→RGB 转换(同 CL 先行)已产出
// inputColor(NSR),本 shader 读它的 Texture2D SRV 双线性写到 NVOF 注册
// 输入纹理。核公式与被删除的 CPU PackNvofInput 逐式一致(f=(d+0.5)*s-0.5、
// (int) 截断、clamp、+1 邻域取 min);差异仅在量化顺序 —— CPU 对 RGBS 浮点
// lerp 后一次性
// 量化,GPU 对 PackInput 已量化的 0-255 值 lerp,权重和为 1 的线性映射
// 下两端差 ≤1 LSB,对光流输入(低频信号)无影响。仅缩窄方向
// (dstW<=srcW)使用,f 恒 >= 0。
constexpr char NVOF_DOWNSAMPLE_HLSL[] = R"(
Texture2D<float4> SourceColor : register(t0);  // BGRA8:SRV 返回逻辑 RGBA(通道无关于本 shader)
RWTexture2D<float4> DestColor : register(u0);

cbuffer NvofDownsampleParams : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    uint Padding0;
};

[numthreads(8, 8, 1)]
void NvofDownsample(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    const float sx = (float)SourceExtent.x / (float)TargetExtent.x;
    const float sy = (float)SourceExtent.y / (float)TargetExtent.y;

    const float fx = (tid.x + 0.5) * sx - 0.5;
    int x0 = (int)fx;
    const float wx = fx - (float)x0;
    x0 = clamp(x0, 0, (int)SourceExtent.x - 1);
    const int x1 = min(x0 + 1, (int)SourceExtent.x - 1);

    const float fy = (tid.y + 0.5) * sy - 0.5;
    int y0 = (int)fy;
    const float wy = fy - (float)y0;
    y0 = clamp(y0, 0, (int)SourceExtent.y - 1);
    const int y1 = min(y0 + 1, (int)SourceExtent.y - 1);

    // UNORM 采样值 *255 精确还原 0-255 整数域(量化语义与 CPU 对齐)。
    const float3 v00 = SourceColor.Load(int3(x0, y0, 0)).xyz * 255.0;
    const float3 v10 = SourceColor.Load(int3(x1, y0, 0)).xyz * 255.0;
    const float3 v01 = SourceColor.Load(int3(x0, y1, 0)).xyz * 255.0;
    const float3 v11 = SourceColor.Load(int3(x1, y1, 0)).xyz * 255.0;

    const float w00 = (1.0 - wx) * (1.0 - wy), w10 = wx * (1.0 - wy);
    const float w01 = (1.0 - wx) * wy, w11 = wx * wy;

    // xyz = B,G,R(与 BGRA8 分量序一致);round-half-up 后 /255 写回,
    // 舍入与 CPU(*255+0.5 截断)一致。alpha 恒 1。
    const float3 value = v00 * w00 + v10 * w10 + v01 * w01 + v11 * w11;
    DestColor[tid.xy] = float4(floor(value + 0.5) / 255.0, 1.0);
}
)";

// ---- AMD 光流后端(FFX)----
// FFX Prepare(Magpie PREPARE_INPUT_HLSL 移植):box 平均下采样到 OF extent。
// FFX 契约输入为 R8G8B8A8(内部 luma 提取按 RGBA 序)。inputColor 是 BGRA8
// 但 typed SRV Load 返回**逻辑 RGBA**(项目惯例,见 NVOF_DOWNSAMPLE_HLSL
// 注释)—— 直接写 float4 即可,无需换序(首版误加 .zyxw 反而转反通道,
// dump_fxof_input 与 dump_input 逐字节对照实锤)。
constexpr char FFX_PREPARE_HLSL[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer Params : register(b0) { uint2 SourceExtent; uint2 TargetExtent; };
[numthreads(8, 8, 1)]
void Prepare(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    uint2 begin = tid.xy * SourceExtent / TargetExtent;
    uint2 end = max(begin + 1, (tid.xy + 1) * SourceExtent / TargetExtent);
    end = min(end, SourceExtent);
    float4 value = 0;
    uint count = 0;
    for (uint y = begin.y; y < end.y; ++y) {
        for (uint x = begin.x; x < end.x; ++x) {
            value += Source.Load(int3(uint2(x, y), 0));
            ++count;
        }
    }
    // SRV 已是逻辑 RGBA(R8G8B8A8 目标直写)。
    Target[tid.xy] = value / max(count, 1);
}
)";

// FFX densify(Magpie DENSIFY_HLSL 原样):1/8 分辨率稀疏流(R16G16_SINT,
// 单位 = OF extent 像素)双线性上采样到稠密运动 + 恒定置信度 0.65
// (FidelityFX OF 不暴露置信度面,Magpie 诚实保守基线)。
constexpr char FFX_DENSIFY_HLSL[] = R"(
Texture2D<int2> SparseFlow : register(t0);
RWTexture2D<float2> DenseMotion : register(u0);
RWTexture2D<float> DenseConfidence : register(u1);
cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 OpticalFlowExtent;
    uint2 SparseExtent;
    float2 VectorScale;
};
int2 LoadFlow(int2 p) {
    p = clamp(p, int2(0, 0), int2(SparseExtent) - 1);
    return SparseFlow.Load(int3(p, 0));
}
[numthreads(8, 8, 1)]
void Densify(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float2 opticalPixel = (float2(tid.xy) + 0.5) *
        (float2(OpticalFlowExtent) / float2(SourceExtent));
    float2 sparsePos = opticalPixel / 8.0 - 0.5;
    int2 p0 = int2(floor(sparsePos));
    float2 f = frac(sparsePos);
    float2 a = float2(LoadFlow(p0));
    float2 b = float2(LoadFlow(p0 + int2(1, 0)));
    float2 c = float2(LoadFlow(p0 + int2(0, 1)));
    float2 d = float2(LoadFlow(p0 + int2(1, 1)));
    DenseMotion[tid.xy] = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) *
        VectorScale;
    DenseConfidence[tid.xy] = 0.65;
}
)";

// 差异调试视图(面板"差异调试 ×20",OptiScaler DLSSNR fork 的 DebugView=3
// 同语义):|NR输出 − 原帧| 逐通道最大差 × 放大系数的灰度图 —— 白 = 改动
// 大,一片灰 = 模型没动画面。两输入同为 BGRA8 的 typed SRV(逻辑 RGBA,
// 通道序一致,差值与分量序无关);alpha 恒 1。
constexpr char DEBUG_DIFF_HLSL[] = R"(
Texture2D<float4> SourceInput : register(t0);   // 原帧(YUV→RGB 转换后)
Texture2D<float4> SourceOutput : register(t1);  // NR/残差/直通输出
RWTexture2D<float4> DebugDiff : register(u0);

cbuffer DebugDiffParams : register(b0) {
    uint2 Extent;        // dispatch 边界(全分辨率)
    float Amplification; // 20
    uint Pad0;
};

[numthreads(8, 8, 1)]
void DebugDiffMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= Extent)) return;
    const float3 src = SourceInput[tid.xy].xyz;
    const float3 dst = SourceOutput[tid.xy].xyz;
    const float d = max(abs(dst.r - src.r), max(abs(dst.g - src.g), abs(dst.b - src.b)));
    DebugDiff[tid.xy] = float4(saturate(d * Amplification).xxx, 1.0);
}
)";

// 光流场调试视图(面板"调试视图 = 光流场"):稠密运动场可视化 —— 方向→
// 色相(标准 HSV 环,图像坐标 y 向下:红=右/绿=下/青=左/紫=上),幅值→亮度,静止与无
// 光流 = 黑。回应"光流到底有没有在流/方向对不对":一片黑 = 无运动数据
// (OF 关/播种帧),彩色纹理 = 真运动场,颜色一致性 = 方向场正确性。
// 输入为 densify 产物(motion 或 follow 内部管线的 reducedMotion,单位 =
// 源像素 current-to-previous),R16G16_FLOAT 的 Texture2D<float2> SRV。
constexpr char FLOW_VIEW_HLSL[] = R"(
Texture2D<float2> FlowField : register(t0);
RWTexture2D<float4> FlowView : register(u0);

cbuffer FlowViewParams : register(b0) {
    uint2 Extent;    // 运动场尺寸(follow 内部管线 = 内部尺寸,否则源尺寸)
    uint2 OutExtent; // 输出尺寸(恒源尺寸;dispatch 边界)
    float Scale;     // 幅值→亮度(0.125 = 8px/帧满亮)
    uint Pad0;
};

[numthreads(8, 8, 1)]
void FlowViewMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= OutExtent)) return;
    // 最近邻取运动场 texel(方向不跨 texel 混合,单元值保真;follow 半
    // 分辨率下呈块状属预期。dispatch 恒按输出尺寸,follow 内部场放大铺满,
    // _debugDiff 无陈旧边缘)
    const int2 p = min(int2((tid.xy * Extent) / OutExtent), int2(Extent) - 1);
    const float2 v = FlowField[p];
    const float mag = length(v);
    if (mag < 0.05) {
        // 死区:亚 0.05px 的噪声不渲染,黑 = 静止,保证"有没有流"一眼可判
        FlowView[tid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const float hue = frac(atan2(v.y, v.x) * 0.15915494 + 1.0); // /2π
    const float3 rgb =
        saturate(abs(fmod(hue * 6.0 + float3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0);
    FlowView[tid.xy] = float4(rgb * saturate(mag * Scale), 1.0);
}
)";

// 抗闪烁时域稳定器主 pass(上游 DLSSNRTemporalShader.h 逐行移植,Magpie
// v0.6.8 093efe21/55d4cc38;SDR 视频管线砍掉 Hdr 域映射 —— NR 链全程 SDR
// 色域,输出随管线色格式,行末 saturate 不截信息)。
// Route 1=静态累积 2=光流累积 3=光流累积+(条件幅度+持续性迟滞)
// 4=低频时域重建(半分辨率残差历史 + 原色引导重建)。与上游的移植差异:
//   1. SampleLevel(LinearClamp) → 手写双线性 Load(插件无采样器状态基建;
//      精确 texel 中心时退化逐位一致,边缘 clamp 后质量归一)。
//   2. Motion 采样:上游恒为源尺寸 Load(p);插件 follow 模式下运动场是
//      内部尺寸 —— MotionExtent 比例采样点 + 向量单位换算(=Size 时退化
//      为逐位上游行为)。
//   3. Hdr 恒 0:Guide() 恒等映射,输出 saturate。
// 历史 alpha 语义保留:RGB = 稳定残差(Route3 = 条件幅度),alpha = 1 或
// Route3 的 1+support —— alpha 是有效性/支持度标记,不是透明度。
constexpr char TEMPORAL_MAIN_HLSL[] = R"(
Texture2D<float4> Input : register(t0);
Texture2D<float4> Base : register(t1);
Texture2D<float4> Raw : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float4> PreviousGuide : register(t4);
Texture2D<float2> Motion : register(t5);
Texture2D<float4> LowResidual : register(t6);
Texture2D<float4> LowGuide : register(t7);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> NextHistory : register(u1);
RWTexture2D<float4> NextGuide : register(u2);
cbuffer Settings : register(b0) {
    uint2 Size; uint UseMotion; uint Route;
    float HistoryWeight; uint2 MotionExtent;
    uint2 LowSize; uint Pad0;
    uint4 Region; // x, y, exclusive right, exclusive bottom
};

bool Inside(int2 p) { return all(p >= int2(Region.xy)) && all(p < int2(Region.zw)); }

// 手写双线性(精确 texel 中心时 frac=0 退化为单 tap 逐位一致;边缘
// clamp 后按质量归一,不重复计 clamped 项 —— 与 CLAMP 采样器语义对齐)。
float4 Bilinear4(Texture2D<float4> tex, float2 pos, int2 extent) {
    int2 a = int2(floor(pos));
    float2 f = frac(pos);
    float4 sum = 0; float mass = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x) {
        float w = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
        if (w <= 0) continue;
        int2 n = clamp(a + int2(x, y), 0, extent - 1);
        sum += w * tex.Load(int3(n, 0)); mass += w;
    }
    return mass > 0 ? sum / mass : 0;
}

// 运动场读取:full-res 像素 p → 面积比例映射到 MotionExtent 网格,向量
// 换算回源像素(上游恒 Load(p);MotionExtent==Size 时逐位同)。
float2 MotionAt(int2 p) {
    float2 mp = (float2(p) + 0.5) * MotionExtent / float2(Size);
    return Motion.Load(int3(int2(mp), 0)) * (float2(Size) / float2(MotionExtent));
}

bool PreviousPosition(int2 p, out float2 previous) {
    previous = p;
    if (!Inside(p)) return false;
    if (UseMotion) {
        float2 mv = MotionAt(p);
        if (!all(isfinite(mv))) return false;
        previous += mv;
    }
    // Validate the entire bilinear footprint before a history/guide read.
    return all(previous >= float2(Region.xy)) &&
        all(previous <= float2(Region.zw) - 1);
}

// G: signed half-resolution observations, upsampled with original-color guidance.
// Return support explicitly: a failed reconstruction must not publish history.
bool Reconstruct(int2 p, float3 color, out float3 residual) {
    int2 a = clamp(int2(floor(float2(p) * .5 - .25)), 0, int2(LowSize) - 1);
    int2 b = clamp(int2(floor(float2(p) * .5 - .25)) + 1, 0, int2(LowSize) - 1);
    float2 ca = min(float2(a) * 2 + .5, float2(Size) - 1);
    float2 cb = min(float2(b) * 2 + .5, float2(Size) - 1);
    float2 f = saturate((float2(p) - ca) / max(cb - ca, 1));
    residual = 0;
    float mass = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x) {
        int2 n = int2(x ? b.x : a.x, y ? b.y : a.y);
        float4 r = LowResidual.Load(int3(n, 0));
        float4 g = LowGuide.Load(int3(n, 0));
        if (!all(isfinite(r)) || !all(isfinite(g)) || r.a < .999 || g.a < .999) continue;
        float3 delta = abs(color - g.rgb);
        float accept = 1 - smoothstep(.02, .10, max(delta.r, max(delta.g, delta.b)));
        float w = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y) * accept;
        if (w <= 0) continue;
        residual += w * r.rgb; mass += w;
    }
    if (mass < .05) return false;
    residual /= mass;
    return true;
}

// G validates individual history taps before interpolation to limit edge leaks.
bool GatherHistory(float2 previous, float3 color, out float4 result) {
    int2 a = int2(floor(previous));
    float2 f = frac(previous);
    result = 0;
    float mass = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x) {
        int2 n = a + int2(x, y);
        float w = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
        if (w <= 0 || !Inside(n)) continue;
        float4 g = PreviousGuide.Load(int3(n, 0));
        float4 h = History.Load(int3(n, 0));
        if (!all(isfinite(g)) || !all(isfinite(h)) || g.a < .999 || h.a < .999) continue;
        float3 delta = abs(color - g.rgb);
        w *= 1 - smoothstep(.025, .10, max(delta.r, max(delta.g, delta.b)));
        if (w <= 0) continue;
        result += w * h; mass += w;
    }
    if (mass < .25) return false;
    result /= mass;
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 p = id.xy;
    if (any(id.xy >= Size)) return;
    float4 original = Input.Load(int3(p, 0));
    float4 base = Base.Load(int3(p, 0));
    float4 raw = Raw.Load(int3(p, 0));
    bool finiteInput = all(isfinite(original));
    bool finiteCurrent = finiteInput && all(isfinite(base)) && all(isfinite(raw));
    NextGuide[p] = finiteInput ? float4(original.rgb, 1) : 0;
    if (!finiteCurrent) {
        NextHistory[p] = 0;
        Output[p] = all(isfinite(base)) ? base : (finiteInput ? original : float4(0, 0, 0, 1));
        return;
    }
    float3 current = raw.rgb - base.rgb;
    float3 detail = 0;
    if (Route == 4) {
        float3 low;
        if (!Reconstruct(p, original.rgb, low)) {
            NextHistory[p] = 0;
            Output[p] = raw;
            return;
        }
        detail = current - low; // Exact current high-frequency complement, not temporally filtered.
        current = low;
    }
    // F stores conditional amplitude in RGB and 1+support in alpha. Missing
    // observations update support only; they never average zeros into amplitude.
    float3 delta = abs(raw.rgb - base.rgb);
    float observed = smoothstep(.005, .02, max(delta.r, max(delta.g, delta.b)));
    float support = observed;
    float3 result = current;
    float2 previous;
    if (HistoryWeight > 0 && PreviousPosition(p, previous)) {
        float error = 0;
        float maximumError = 0;
        bool valid = true;
        float3 lo = current, hi = current;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 n = p + int2(x, y);
            float2 previousN;
            if (!PreviousPosition(n, previousN)) { valid = false; continue; }
            // Reject patch support crossing a motion discontinuity.
            if (UseMotion && any(abs((previousN - n) - (previous - p)) > 2)) { valid = false; continue; }
            float4 inputN = Input.Load(int3(n, 0));
            float4 old = Bilinear4(PreviousGuide, previousN, int2(Size));
            float3 residualN = Raw.Load(int3(n, 0)).rgb - Base.Load(int3(n, 0)).rgb;
            if (!all(isfinite(inputN)) || !all(isfinite(old)) || old.a < .999 ||
                !all(isfinite(residualN))) { valid = false; continue; }
            float3 delta = abs(inputN.rgb - old.rgb);
            float e = max(delta.r, max(delta.g, delta.b));
            error += e / 9;
            maximumError = max(maximumError, e);
            lo = min(lo, residualN); hi = max(hi, residualN);
        }
        // Raw-color patch matching is the initial B.1 baseline: no residual blur.
        float q = (1 - smoothstep(.008, .04, error)) *
                  (1 - smoothstep(.025, .10, maximumError));
        if (valid && q > 0) {
            float4 old = 0;
            bool historyValid = true;
            if (Route == 4) historyValid = GatherHistory(previous, original.rgb, old);
            else old = Bilinear4(History, previous, int2(Size));
            if (historyValid && all(isfinite(old)) && old.a >= .999) {
                // Include the center and permit a stable correction to decay even
                // when the current residual vanishes. A tight variance box would
                // erase the very on/off history this filter needs to stabilize.
                float3 margin = .02 + q * abs(old.rgb);
                float3 safe = clamp(old.rgb, lo - margin, hi + margin);
                if (Route == 3) {
                    float oldSupport = saturate(old.a - 1);
                    // 60 ms attack / 180 ms release, derived from the same capture
                    // interval as the 80 ms amplitude EMA. q is applied once.
                    float memory = pow(abs(HistoryWeight), .08 / (observed > oldSupport ? .06 : .18)) * q;
                    support = lerp(observed, oldSupport, memory);
                    if (oldSupport <= .001) result = current;
                    else if (observed > 0 && dot(current, old.rgb) < 0) {
                        // An opposite direction starts a new support episode.
                        support = observed * (1 - pow(abs(HistoryWeight), .08 / .06) * q);
                        result = current;
                    } else {
                        float update = (1 - HistoryWeight * q) * observed;
                        result = lerp(safe, current, update);
                    }
                } else result = lerp(current, safe, HistoryWeight * q);
            }
        }
    }
    // FP16 history is signed; its alpha marks a valid observation, not opacity.
    NextHistory[p] = float4(clamp(result, -65504, 65504), Route == 3 ? 1 + support : 1);
    float3 color = base.rgb + detail + (Route == 3 ? support * result : result);
    Output[p] = float4(saturate(color), raw.a);
}
)";

// mode4 半分辨率 reduce(上游 DLSSNR_TEMPORAL_REDUCE_SHADER 移植):精确
// 后链差分(含各层控制/裁切/量化)降采样为有符号残差 + 原色引导,主 pass
// 以原色一致性引导重建 —— 不改变 NGX 输入。cbuffer 布局与主 pass 同步。
constexpr char TEMPORAL_REDUCE_HLSL[] = R"(
Texture2D<float4> Input : register(t0);
Texture2D<float4> Base : register(t1);
Texture2D<float4> Raw : register(t2);
RWTexture2D<float4> Residual : register(u0);
RWTexture2D<float4> Guide : register(u1);
cbuffer Settings : register(b0) {
    uint2 Size; uint UseMotion; uint Route;
    float HistoryWeight; uint2 MotionExtent;
    uint2 LowSize; uint Pad0;
    uint4 Region;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= LowSize)) return;
    float3 r = 0, g = 0;
    float count = 0;
    bool valid = true;
    [unroll] for (uint y = 0; y < 2; ++y)
    [unroll] for (uint x = 0; x < 2; ++x) {
        uint2 p = id.xy * 2 + uint2(x, y);
        if (any(p >= Size)) continue; // Odd dimensions retain their last sample.
        if (any(p < Region.xy) || any(p >= Region.zw)) { valid = false; continue; }
        float4 i = Input.Load(int3(p, 0));
        float4 b = Base.Load(int3(p, 0));
        float4 o = Raw.Load(int3(p, 0));
        if (!all(isfinite(i)) || !all(isfinite(b)) || !all(isfinite(o))) { valid = false; continue; }
        r += o.rgb - b.rgb;
        g += i.rgb;
        count += 1;
    }
    if (!valid || count == 0) { Residual[id.xy] = 0; Guide[id.xy] = 0; return; }
    Residual[id.xy] = float4(clamp(r / count, -65504, 65504), 1);
    Guide[id.xy] = float4(g / count, 1);
}
)";

// YUV→RGB(YUV 原生化):typed buffer SRV 直读 upload 平面(2026-10-02,
// tsrv_probe 复核的驱动形态;此前 Texture2D 间接 + 3×拷贝已撤),按位深
// 恢复整数采样字、按范围展开,矩阵求逆得 RGB,UAV 直写 inputColor。深度/
// 矩阵/范围全在 root constants(C0/C1,由 YuvCoeffsFor 按 _bitDepth 推导)
// —— R8/R16_UNORM 的 Buffer<float> SRV 与 Texture2D 同构,单 PSO 通吃两
// 深度;平铺索引 x + y*Pitch(@14/15,采样字/行),取值公式与 Texture2D
// Load 逐位一致。
// BGRA typed SRV 返回逻辑 RGBA(.xyz=R,G,B,Magpie r5 TODO §2.1 同款约定;
// 数值验收的 python 参考会当场抓 R/B 换位)。
constexpr char YUV_TO_BGRA_HLSL[] = R"(
Buffer<float> PlaneY : register(t0);
Buffer<float> PlaneU : register(t1);
Buffer<float> PlaneV : register(t2);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer ConvertInParams : register(b0) {
    uint2 DstExtent;      // inputColor 尺寸(dispatch 边界)
    float ContainerMax;   // 255 / 65535
    float YLo;            // limited 16/64/256…;full 0
    float YScale;         // 1/ySpan
    float CMid;           // limited 128/512/2048…;full sampleMax/2
    float CScale;         // 1/cSpan
    float Kr;             // 0.2126(709) / 0.299(601)
    float Kb;             // 0.0722(709) / 0.114(601)
    float Pad0;
    uint2 ChromaExtent;   // @10 色度平面尺寸(420=半、422=半宽、444=全)
    float2 ChromaScale;   // @12 双线性映射比例(0.5 / 1)
    uint PlaneYPitch;     // @14 Y 平面行宽(采样字)
    uint PlaneCPitch;     // @15 U/V 平面行宽(采样字)
};

[numthreads(8, 8, 1)]
void ConvertYuvToBgra(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const uint yi = tid.x + tid.y * PlaneYPitch;
    // round(UNORM × containerMax) 精确还原整数采样字(8/10/12+ 位同构)。
    const float codeY = round(PlaneY[yi].x * ContainerMax);
    const float nY = (codeY - YLo) * YScale;

    // 色度子采样 → 双线性上采(与原 zimg Bilinear 对齐;siting (0,0.5))。
    // ChromaScale=(0.5,0.5) 与旧硬编码逐式恒等;444 =(1,1) 时 fc=tid、
    // wf=0 → 仅 s0 命中(零插值)。
    const float2 fc = (tid.xy + 0.5) * ChromaScale - 0.5;
    const int2 c0 = int2(floor(fc));
    const float2 wf = fc - c0;
    const int2 s0 = clamp(c0, int2(0, 0), ChromaExtent - 1);
    const int2 s1 = clamp(c0 + 1, int2(0, 0), ChromaExtent - 1);
    const float w00 = (1.0 - wf.x) * (1.0 - wf.y), w10 = wf.x * (1.0 - wf.y);
    const float w01 = (1.0 - wf.x) * wf.y, w11 = wf.x * wf.y;
    const uint ci0 = s0.x + s0.y * PlaneCPitch;
    const uint ciX = s1.x + s0.y * PlaneCPitch;
    const uint ciY = s0.x + s1.y * PlaneCPitch;
    const uint ci1 = s1.x + s1.y * PlaneCPitch;
    const float codeU = round(PlaneU[ci0].x * ContainerMax) * w00
                      + round(PlaneU[ciX].x * ContainerMax) * w10
                      + round(PlaneU[ciY].x * ContainerMax) * w01
                      + round(PlaneU[ci1].x * ContainerMax) * w11;
    const float codeV = round(PlaneV[ci0].x * ContainerMax) * w00
                      + round(PlaneV[ciX].x * ContainerMax) * w10
                      + round(PlaneV[ciY].x * ContainerMax) * w01
                      + round(PlaneV[ci1].x * ContainerMax) * w11;
    const float nU = (codeU - CMid) * CScale;
    const float nV = (codeV - CMid) * CScale;

    // 矩阵求逆(Kg = 1-Kr-Kb):Cr 驱动 R、Cb 驱动 B,增益各配自己的
    // (1-Kx) —— R = Y + Cr·2(1-Kr) = Y + 1.5748·Cr(709)。
    const float Kg = 1.0 - Kr - Kb;
    const float r = nY + nV * 2.0 * (1.0 - Kr);
    const float b = nY + nU * 2.0 * (1.0 - Kb);
    const float g = (nY - Kr * r - Kb * b) / Kg;
    OutputColor[tid.xy] = float4(saturate(r), saturate(g), saturate(b), 1.0);
}

// VS RGBP(计划 RGB,平面序 G/B/R)直读:零矩阵零插值打包。8bit 走
// BGRA8 逐位精确;>8bit 容器(R16 采样)按输入Color 格式(见 Phase C)。
[numthreads(8, 8, 1)]
void ConvertRgbToRgba(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const uint yi = tid.x + tid.y * PlaneYPitch;
    const uint ci = tid.x + tid.y * PlaneCPitch;
    const float g = PlaneY[yi].x;
    const float b = PlaneU[ci].x;
    const float r = PlaneV[ci].x;
    OutputColor[tid.xy] = float4(r, g, b, 1.0);
}
)";

// RGB→YUV(YUV 原生化):outputColor(SRV,逻辑 RGBA)→ luma 全分辨率 +
// chroma 半分辨率 2×2 box 两个入口。归一域 → 整数采样字 → 写 w =
// code/containerMax;UNORM 存储回读 = round(w×containerMax) = code,与
// VS 平面字(P8 byte / P10 word)逐位一致 → CPU 解包纯行拷贝。深度由
// LoOverCM/SpanOverCM 折叠(= yLo/CM 等,YuvCoeffsFor 推导)。
constexpr char BGRA_TO_YUV_HLSL[] = R"(
Texture2D<float4> CompositeColor : register(t0);  // BGRA8:SRV 返回逻辑 RGBA
RWTexture2D<float> OutputA : register(u0);  // luma dispatch 绑 yuvOut[0](Y)
RWTexture2D<float> OutputB : register(u1);  // chroma dispatch 绑 yuvOut[1](U)/[2](V)

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;      // luma: W,H;chroma: cw,ch(dispatch 边界)
    uint2 SourceExtent;   // 全分辨率尺寸(chroma 块 clamp)
    float Kr;
    float Kb;
    float LoOverCM;       // luma: yLo/CM;chroma: cMid/CM
    float SpanOverCM;     // luma: ySpan/CM;chroma: cSpan/CM
    float Pad0;
    float Pad1;
    float Pad2;
    uint2 ChromaStep;     // @12 色度抽因子(420=(2,2)、422=(2,1)、444=(1,1))
};

float LumaOf(float3 rgb) {
    return dot(rgb, float3(Kr, 1.0 - Kr - Kb, Kb));
}

// 色度写回:cb/cr ∈ [-0.5,0.5],必须先加半幅度偏移再 saturate —— saturate
// 在前会把全部负色度钳成中性 128(绿/青/蓝内容去饱和,2026-09-08 实测;
// 与 #30 负残差 UNORM clamp 同族的"负值先钳"坑)。
float ChromaToCode(float c) {
    return saturate(c * SpanOverCM + LoOverCM);
}

[numthreads(8, 8, 1)]
void BgraToYuvLuma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 rgb = saturate(CompositeColor[tid.xy].xyz);
    const float nY = LumaOf(rgb);
    OutputA[tid.xy] = saturate(nY * SpanOverCM + LoOverCM);
}

[numthreads(8, 8, 1)]
void BgraToYuvChroma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    // box 平均(420=2×2、422=2×1、444=1×1):全程仿射,先平均后转换与
    // 逐点转换等价。step=(2,2) 与旧硬编码逐式恒等;444 四样本同址 = 直写。
    const int2 base = tid.xy * ChromaStep;
    const int2 x1 = min(base + int2(ChromaStep.x - 1, 0), SourceExtent - 1);
    const int2 y1 = min(base + int2(0, ChromaStep.y - 1), SourceExtent - 1);
    const int2 x1y1 = min(base + int2(ChromaStep.x - 1, ChromaStep.y - 1), SourceExtent - 1);
    const float invW = 1.0 / float(ChromaStep.x * ChromaStep.y);
    const float3 rgb = saturate(invW * (CompositeColor[base].xyz
                                      + CompositeColor[x1].xyz
                                      + CompositeColor[y1].xyz
                                      + CompositeColor[x1y1].xyz));
    const float nY = LumaOf(rgb);
    // cb = (B-Y)·0.5/(1-Kb),cr = (R-Y)·0.5/(1-Kr)
    const float cb = (rgb.z - nY) * (0.5 / (1.0 - Kb));
    const float cr = (rgb.x - nY) * (0.5 / (1.0 - Kr));
    OutputA[tid.xy] = ChromaToCode(cb);  // u0 → U 平面
    OutputB[tid.xy] = ChromaToCode(cr);  // u1 → V 平面
}
)";

// ---- RGB 出侧(Phase B):RGBP(平面序 G/B/R)直写,零矩阵 ----
// SDR 同格式输出。8bit 全域 0-255(RGB 无 limited 概念,直写归一值);
// >8bit 容器同理。RTX 缩放版(PIPE→OUT)用双线性,cbuffer 同 ConvertOut。
constexpr char RGB_TO_PLANAR_HLSL[] = R"(
Texture2D<float4> CompositeColor : register(t0);
RWTexture2D<float> OutputA : register(u0);  // G(yuvOut[0])
RWTexture2D<float> OutputB : register(u1);  // B(yuvOut[1])
RWTexture2D<float> OutputC : register(u2);  // R(yuvOut[2])

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;
    uint2 SourceExtent;
    float Kr;
    float Kb;
    float LoOverCM;
    float SpanOverCM;
    float Pad0;
    float Pad1;
    float Pad2;
    uint2 ChromaStep;
};

float3 SampleBilinear(float2 srcPos) {
    const float2 f = floor(srcPos);
    const int2 i0 = int2(f);
    const float2 t = srcPos - f;
    const int2 e = SourceExtent - 1;
    const int2 a = clamp(i0, int2(0, 0), e);
    const int2 b = clamp(i0 + int2(1, 0), int2(0, 0), e);
    const int2 c = clamp(i0 + int2(0, 1), int2(0, 0), e);
    const int2 d = clamp(i0 + int2(1, 1), int2(0, 0), e);
    return CompositeColor[a].xyz * ((1 - t.x) * (1 - t.y))
         + CompositeColor[b].xyz * (t.x * (1 - t.y))
         + CompositeColor[c].xyz * ((1 - t.x) * t.y)
         + CompositeColor[d].xyz * (t.x * t.y);
}
float2 ToSrcPos(float2 dstPos) {
    return (dstPos + 0.5) * float2(SourceExtent) / float2(DstExtent) - 0.5;
}

[numthreads(8, 8, 1)]
void RgbaToRgbPlanes(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 rgb = saturate(CompositeColor[tid.xy].xyz);
    OutputA[tid.xy] = rgb.g;  // RGBP 平面序 G,B,R
    OutputB[tid.xy] = rgb.b;
    OutputC[tid.xy] = rgb.r;
}

[numthreads(8, 8, 1)]
void ScaledToRgbPlanes(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 rgb = saturate(SampleBilinear(ToSrcPos(float2(tid.xy))));
    OutputA[tid.xy] = rgb.g;
    OutputB[tid.xy] = rgb.b;
    OutputC[tid.xy] = rgb.r;
}
)";

// ---- RTX Video:PIPE → OUT 缩放转换(SDR;BGRA8 源)----
// VSR 输出(PIPE 尺寸)→ OUT 尺寸 YUV 平面。双 bilinear 采样(归一化坐标
// 映射,手工 4 tap——全仓惯例无 sampler,SRV [] 索引)。PIPE==OUT 时坐标
// 恒等映射 = 逐位等价旧路径。cbuffer 布局与 BGRA_TO_YUV_HLSL 完全一致
// (dstExtent / srcExtent / Kr Kb LoOverCM SpanOverCM + pad),共用
// _rsConvertOut 根签名,仅换 PSO。4 tap 权重全正,色度平均后一次转换
// (与 2×2 box 同款仿射论证)。
constexpr char COLOR_TO_YUV_SCALED_HLSL[] = R"(
Texture2D<float4> CompositeColor : register(t0);
RWTexture2D<float> OutputA : register(u0);
RWTexture2D<float> OutputB : register(u1);

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;      // luma: OUT W,H;chroma: OUT cw,ch(dispatch 边界)
    uint2 SourceExtent;   // PIPE 全分辨率尺寸
    float Kr;
    float Kb;
    float LoOverCM;
    float SpanOverCM;
    float Pad0;
    float Pad1;
    float Pad2;
    uint2 ChromaStep;     // @12 色度抽因子(420=(2,2)、422=(2,1)、444=(1,1))
};

float LumaOf(float3 rgb) {
    return dot(rgb, float3(Kr, 1.0 - Kr - Kb, Kb));
}
float ChromaToCode(float c) {
    return saturate(c * SpanOverCM + LoOverCM);
}
// 手工双线性(4 tap,clamp 边界;idx 域 = 源纹理尺寸)。
float3 SampleBilinear(float2 srcPos) {
    const float2 f = floor(srcPos);
    const int2 i0 = int2(f);
    const float2 t = srcPos - f;
    const int2 e = SourceExtent - 1;
    const int2 a = clamp(i0, int2(0, 0), e);
    const int2 b = clamp(i0 + int2(1, 0), int2(0, 0), e);
    const int2 c = clamp(i0 + int2(0, 1), int2(0, 0), e);
    const int2 d = clamp(i0 + int2(1, 1), int2(0, 0), e);
    return CompositeColor[a].xyz * ((1 - t.x) * (1 - t.y))
         + CompositeColor[b].xyz * (t.x * (1 - t.y))
         + CompositeColor[c].xyz * ((1 - t.x) * t.y)
         + CompositeColor[d].xyz * (t.x * t.y);
}
float2 ToSrcPos(float2 dstPos) {
    return (dstPos + 0.5) * float2(SourceExtent) / float2(DstExtent) - 0.5;
}
// 色度域采样映射:入参为 out-luma 域坐标(chroma 侧 base = tid*ChromaStep),
// 映射分母必须是 out-luma 域 = ChromaStep*DstExtent(色度 dispatch 的
// DstExtent=色度域;420 时 step=(2,2) 与旧 2*DstExtent 逐式恒等)。误用
// DstExtent = 2× 过采样 —— 色度右半平面 clamp 到源右缘,表现为大色块/
// 失饱和(2026-09-22 真机实锤,"只有一层分辨率放大了")。
float2 ToSrcPosChroma(float2 lumaPos) {
    return (lumaPos + 0.5) * float2(SourceExtent)
         / (float2(ChromaStep) * float2(DstExtent)) - 0.5;
}

[numthreads(8, 8, 1)]
void ScaledToYuvLuma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 rgb = saturate(SampleBilinear(ToSrcPos(float2(tid.xy))));
    const float nY = LumaOf(rgb);
    OutputA[tid.xy] = saturate(nY * SpanOverCM + LoOverCM);
}

[numthreads(8, 8, 1)]
void ScaledToYuvChroma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    // luma 域 ChromaStep 覆盖区四点采样(各点双线性)平均 → 转换。
    // step=(2,2) 时与旧硬编码 0.25/2×2 逐式恒等;444=(1,1) 四点同址 = 直写。
    const float2 base = tid.xy * ChromaStep;
    const float invW = 1.0 / float(ChromaStep.x * ChromaStep.y);
    const float2 h = float2(ChromaStep) * 0.5;
    const float3 rgb = saturate(invW * (SampleBilinear(ToSrcPosChroma(base + float2(h.x, h.y)))
                                      + SampleBilinear(ToSrcPosChroma(base + float2(ChromaStep.x - h.x, h.y)))
                                      + SampleBilinear(ToSrcPosChroma(base + float2(h.x, ChromaStep.y - h.y)))
                                      + SampleBilinear(ToSrcPosChroma(base + float2(ChromaStep.x - h.x, ChromaStep.y - h.y)))));
    const float nY = LumaOf(rgb);
    const float cb = (rgb.z - nY) * (0.5 / (1.0 - Kb));
    const float cr = (rgb.x - nY) * (0.5 / (1.0 - Kr));
    OutputA[tid.xy] = ChromaToCode(cb);
    OutputB[tid.xy] = ChromaToCode(cr);
}
)";

// ---- RTX Video HDR:FP16 scRGB → BT.2020 PQ P10(TrueHDR 输出转换)----
// TrueHDR 输出语义 = linear scRGB(1.0 = 80 nits SDR 参考白,Magpie
// RTXVideoHdr 同解读)。链路:线性 709 →(线性域)2020 色域 → 各分量 PQ
// (ST 2084,归一 10000 nits)→ 2020 YCbCr limited 10bit → P10 字(右对齐
// 惯例:w = code/65535)。双线性在 LINEAR 光域 = 物理正确插值。cbuffer 同
// 上(Kr/Kb 位置复用为 2020 的 0.2627/0.0593,Lo/Span 传 limited 常量;
// 2026-09-23 实锤:Lo/Span 曾按跨度 876/896 折 —— 整个 HDR 动态范围被
// 压进 1.4 个 P10 码的"恒定纯色"。正确分母 = 容器 1023)。
constexpr char FP16_TO_YUV_PQ_HLSL[] = R"(
Texture2D<float4> HdrColor : register(t0);
RWTexture2D<float> OutputA : register(u0);
RWTexture2D<float> OutputB : register(u1);

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;
    uint2 SourceExtent;
    float Kr;             // BT.2020: 0.2627
    float Kb;             // BT.2020: 0.0593
    float LoOverCM;       // luma: 64/1023;chroma: 512/1023
    float SpanOverCM;     // 876/1023 或 896/1023
    float Pad0;
    float Pad1;
    float Pad2;
    uint2 ChromaStep;     // @12 恒 (2,2):HDR 输出契约恒 P10 420
};

static const float3x3 M709To2020 = {
    0.6274, 0.3293, 0.0433,
    0.0690, 0.9195, 0.0112,
    0.0164, 0.0880, 0.8955,
};

// ST 2084 PQ EOTF 逆(输入 nits → [0,1] PQ 码值;归一 10000 nits)。
float PqEncode(float nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float p = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float4 SampleHdrBilinear(float2 srcPos) {
    const float2 f = floor(srcPos);
    const int2 i0 = int2(f);
    const float2 t = srcPos - f;
    const int2 e = SourceExtent - 1;
    const int2 a = clamp(i0, int2(0, 0), e);
    const int2 b = clamp(i0 + int2(1, 0), int2(0, 0), e);
    const int2 c = clamp(i0 + int2(0, 1), int2(0, 0), e);
    const int2 d = clamp(i0 + int2(1, 1), int2(0, 0), e);
    return HdrColor[a] * ((1 - t.x) * (1 - t.y))
         + HdrColor[b] * (t.x * (1 - t.y))
         + HdrColor[c] * ((1 - t.x) * t.y)
         + HdrColor[d] * (t.x * t.y);
}
float2 ToSrcPos(float2 dstPos) {
    return (dstPos + 0.5) * float2(SourceExtent) / float2(DstExtent) - 0.5;
}
// 同 ScaledToYuvChroma 的 ToSrcPosChroma:色度侧入参为 out-luma 域坐标,
// 分母 = 2*DstExtent(误用 DstExtent = 2× 过采样,色块/失饱和)。
float2 ToSrcPosChroma(float2 lumaPos) {
    return (lumaPos + 0.5) * float2(SourceExtent) / (2.0 * float2(DstExtent)) - 0.5;
}
// 线性 scRGB(709)→ PQ 编码的 2020 RGB 三元组。
// mul(矩阵, 向量) = 行和语义(灰保持);曾写 mul(向量, 矩阵) = 列和
// (0.713/1.337/0.950)→ 灰色扭曲成 R 压 G 涨 → 全画面青色罩。
float3 ToPq2020(float3 lin709) {
    const float3 lin2020 = mul(M709To2020, max(lin709, 0.0)) * 80.0; // nits
    return float3(PqEncode(lin2020.r), PqEncode(lin2020.g), PqEncode(lin2020.b));
}

[numthreads(8, 8, 1)]
void PqToYuvLuma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 pq = ToPq2020(SampleHdrBilinear(ToSrcPos(float2(tid.xy))).rgb);
    const float y = dot(pq, float3(Kr, 1.0 - Kr - Kb, Kb));
    // P10 存储约定(与 BGRA_TO_YUV 同款):R16_UNORM 字 = 10-bit 采样值,
    // shader 侧 ×1023/65535 精确缩放 —— 缺它 = 字 = 码×64,mpv 读出越界垃圾。
    OutputA[tid.xy] = saturate(y * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
}

[numthreads(8, 8, 1)]
void PqToYuvChroma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float2 base = tid.xy * 2;
    const float3 pq = 0.25 * (ToPq2020(SampleHdrBilinear(ToSrcPosChroma(base + float2(0.5, 0.5))).rgb)
                            + ToPq2020(SampleHdrBilinear(ToSrcPosChroma(base + float2(1.5, 0.5))).rgb)
                            + ToPq2020(SampleHdrBilinear(ToSrcPosChroma(base + float2(0.5, 1.5))).rgb)
                            + ToPq2020(SampleHdrBilinear(ToSrcPosChroma(base + float2(1.5, 1.5))).rgb));
    const float y = dot(pq, float3(Kr, 1.0 - Kr - Kb, Kb));
    const float cb = (pq.b - y) * (0.5 / (1.0 - Kb));
    const float cr = (pq.r - y) * (0.5 / (1.0 - Kr));
    OutputA[tid.xy] = saturate(cb * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
    OutputB[tid.xy] = saturate(cr * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
}
)";

// ---- PQ 域插帧(实验 fgHdrInterp,2026-09-24 定案)----
// DLSSG 的 ColorBuffersHDR=1 路径对 >1.0 的 scRGB 线性值不保真(插值帧高光
// 钳 ~0.875 = ~70 nits;≤1.0 内容逐位保真,HDR10 游戏的 PQ 码域 ≤1.0 天然
// 免疫)。改为:TrueHDR 产物编码成 PQ 码(BT.2020,≤1.0)作 DLSSG backbuffer
// (ColorBuffersHDR=0,原生 HDR 直通已验证的 LDR 路径),DLSSG 在感知域插帧
// (HDR10 游戏标准形态),输出侧码直读免逐像素 pow。
constexpr char HDR_TO_PQ_HLSL[] = R"(
Texture2D<float4> HdrColor : register(t0);
RWTexture2D<float4> OutputPq : register(u0);

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;
    uint2 SourceExtent;
    float Kr;
    float Kb;
    float LoOverCM;
    float SpanOverCM;
    float Pad0;
    float Pad1;
    float Pad2;
    uint2 ChromaStep;
};

static const float3x3 M709To2020 = {
    0.6274, 0.3293, 0.0433,
    0.0690, 0.9195, 0.0112,
    0.0164, 0.0880, 0.8955,
};

// ST 2084 PQ EOTF 逆(输入 nits → [0,1] PQ 码值;归一 10000 nits)。
float PqEncode(float nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float p = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// 同 FP16_TO_YUV_PQ 的 ToPq2020:mul(矩阵, 向量) = 行和语义(灰保持);
// 曾写 mul(向量, 矩阵) → 全画面青色罩(见该处注释)。
float3 ToPq2020(float3 lin709) {
    const float3 lin2020 = mul(M709To2020, max(lin709, 0.0)) * 80.0; // nits
    return float3(PqEncode(lin2020.r), PqEncode(lin2020.g), PqEncode(lin2020.b));
}

// 1:1 编码(hdrColor 与 fgBack 同为 PIPE 尺寸):Load 直读,双线性无意义。
// 越界负色度(out-of-2020-gamut)经 max(,0) 钳 0 —— 与 PqToYuv 的
// PqEncode(saturate) 同语义,无行为回退。
[numthreads(8, 8, 1)]
void HdrToPq(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    OutputPq[tid.xy] = float4(ToPq2020(HdrColor[tid.xy].rgb), 1.0);
}
)";

// PQ 码域(FG 插值产物)→ P10 420:布局与 FP16_TO_YUV_PQ 同款,仅输入已是
// 感知码 —— 双线性/均值在码域做(感知域插值语义),BT.2020 矩阵 + limited
// ladder 落 P10,零 PqEncode(逐像素 pow 省略 = 该模式转换成本的主项)。
constexpr char PQ_CODES_TO_YUV_HLSL[] = R"(
Texture2D<float4> HdrColor : register(t0);
RWTexture2D<float> OutputA : register(u0);
RWTexture2D<float> OutputB : register(u1);

cbuffer ConvertOutParams : register(b0) {
    uint2 DstExtent;
    uint2 SourceExtent;
    float Kr;
    float Kb;
    float LoOverCM;
    float SpanOverCM;
    float Pad0;
    float Pad1;
    float Pad2;
    uint2 ChromaStep;
};

float4 SampleCodesBilinear(float2 srcPos) {
    const float2 f = floor(srcPos);
    const int2 i0 = int2(f);
    const float2 t = srcPos - f;
    const int2 e = SourceExtent - 1;
    const int2 a = clamp(i0, int2(0, 0), e);
    const int2 b = clamp(i0 + int2(1, 0), int2(0, 0), e);
    const int2 c = clamp(i0 + int2(0, 1), int2(0, 0), e);
    const int2 d = clamp(i0 + int2(1, 1), int2(0, 0), e);
    return HdrColor[a] * ((1 - t.x) * (1 - t.y))
         + HdrColor[b] * (t.x * (1 - t.y))
         + HdrColor[c] * ((1 - t.x) * t.y)
         + HdrColor[d] * (t.x * t.y);
}
float2 ToSrcPos(float2 dstPos) {
    return (dstPos + 0.5) * float2(SourceExtent) / float2(DstExtent) - 0.5;
}
// 同 ScaledToYuvChroma 的 ToSrcPosChroma:色度侧入参为 out-luma 域坐标,
// 分母 = 2*DstExtent(误用 DstExtent = 2× 过采样,色块/失饱和)。
float2 ToSrcPosChroma(float2 lumaPos) {
    return (lumaPos + 0.5) * float2(SourceExtent) / (2.0 * float2(DstExtent)) - 0.5;
}

[numthreads(8, 8, 1)]
void PqCodesToYuvLuma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float3 pq = SampleCodesBilinear(ToSrcPos(float2(tid.xy))).rgb;
    const float y = dot(pq, float3(Kr, 1.0 - Kr - Kb, Kb));
    // P10 存储约定(与 BGRA_TO_YUV 同款):R16_UNORM 字 = 10-bit 采样值,
    // shader 侧 ×1023/65535 精确缩放 —— 缺它 = 字 = 码×64,mpv 读出越界垃圾。
    OutputA[tid.xy] = saturate(y * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
}

[numthreads(8, 8, 1)]
void PqCodesToYuvChroma(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float2 base = tid.xy * 2;
    const float3 pq = 0.25 * (SampleCodesBilinear(ToSrcPosChroma(base + float2(0.5, 0.5))).rgb
                            + SampleCodesBilinear(ToSrcPosChroma(base + float2(1.5, 0.5))).rgb
                            + SampleCodesBilinear(ToSrcPosChroma(base + float2(0.5, 1.5))).rgb
                            + SampleCodesBilinear(ToSrcPosChroma(base + float2(1.5, 1.5))).rgb);
    const float y = dot(pq, float3(Kr, 1.0 - Kr - Kb, Kb));
    const float cb = (pq.b - y) * (0.5 / (1.0 - Kb));
    const float cr = (pq.r - y) * (0.5 / (1.0 - Kr));
    OutputA[tid.xy] = saturate(cb * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
    OutputB[tid.xy] = saturate(cr * SpanOverCM + LoOverCM) * (1023.0 / 65535.0);
}
)";

// ---- RTX Video + FG:源尺寸运动场 → PIPE 尺寸(mvec 放大)----
// DLSSG MVecs 契约 = backbuffer 同尺寸、像素单位 current-to-previous。
// R16G16F 向量场双线性 = 线性插值,语义保真。4 常量:dstExtent(2)+srcExtent(2)
// (CreateOfPso 通用形态)。
constexpr char MVEC_SCALE_HLSL[] = R"(
Texture2D<float2> SrcMotion : register(t0);
RWTexture2D<float2> DstMotion : register(u0);

cbuffer MotionScaleParams : register(b0) {
    uint2 DstExtent;
    uint2 SourceExtent;
};

[numthreads(8, 8, 1)]
void ScaleMotion(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= DstExtent)) return;
    const float2 srcPos = (tid.xy + 0.5) * float2(SourceExtent) / float2(DstExtent) - 0.5;
    const float2 f = floor(srcPos);
    const int2 i0 = int2(f);
    const float2 t = srcPos - f;
    const int2 e = SourceExtent - 1;
    const int2 a = clamp(i0, int2(0, 0), e);
    const int2 b = clamp(i0 + int2(1, 0), int2(0, 0), e);
    const int2 c = clamp(i0 + int2(0, 1), int2(0, 0), e);
    const int2 d = clamp(i0 + int2(1, 1), int2(0, 0), e);
    DstMotion[tid.xy] = SrcMotion[a] * ((1 - t.x) * (1 - t.y))
                      + SrcMotion[b] * (t.x * (1 - t.y))
                      + SrcMotion[c] * ((1 - t.x) * t.y)
                      + SrcMotion[d] * (t.x * t.y);
}
)";


} // namespace
} // namespace vsdlssnr
