#include "d3d12_context.h"
#include "d3d12_internal.h"
#include "d3d12_shaders.h"

// 色域转换子系统(RecordConvertInput/RecordColorOutput/HdrToPq/readback 拷贝;
// CreateConvertObjects)—— 2026-10-04 Pass 组拆分的派生件,方法体逐字迁移。

namespace vsdlssnr {

namespace {

// YUV↔RGB 转换系数(按位深/矩阵/范围推导,root constants 下发)。全程
// 归一域 [0,1](Y)/[-0.5,0.5](C);shader 端与 CPU 参考实现共用同一组
// 公式。10-bit 的有限范围常量 = 8-bit ×4(16→64 等)。
// containerMax = UNORM 容器满值(255/65535):round(UNORM读出×containerMax)
// 精确还原整数采样字;sampleMax = 采样值域上限(255/1023)。
// (2026-10-05 自 d3d12_internal.h 下沉 —— 本 TU 唯一消费者。)
struct YuvCoeffs {
    float containerMax;
    float sampleMax;
    float yLo, ySpan;   // limited: 16/219(8bit) 64/876(10bit);full: 0/sampleMax
    float cMid, cSpan;  // limited: 128/224 512/896;full: sampleMax/2 与 sampleMax
    float kr, kb;       // 709: 0.2126/0.0722;601: 0.299/0.114
};

YuvCoeffs YuvCoeffsFor(ColorMatrix matrix, ColorRange range, int depth) noexcept {
    YuvCoeffs c{};
    // 容器字宽:8bit 单字节;>8bit(VS 10/12/14/16)统一 16bit 容器右对齐
    // 采样字(VS 约定),R16 footprint 直传,无位移。
    c.containerMax = depth > 8 ? 65535.0f : 255.0f;
    c.sampleMax = static_cast<float>((1 << depth) - 1);
    // limited 阶梯 = 8bit 基准按位左移(10bit: 64/876/512/896;12bit:
    // 256/3504/2048/3584 —— ITU 量化表同构)。注意是 ×(1<<(depth-8)) 而非
    // ×(sampleMax/255):1023/255=4.0118 ≠ 4,等比会偏出 0.2 个码。
    const float shift = depth > 8 ? static_cast<float>(1 << (depth - 8)) : 1.0f;
    if (range == ColorRange::Limited) {
        c.yLo = 16.0f * shift;
        c.ySpan = 219.0f * shift;
        c.cMid = 128.0f * shift;
        c.cSpan = 224.0f * shift;
    } else {
        c.yLo = 0.0f;
        c.ySpan = c.sampleMax;
        c.cMid = c.sampleMax * 0.5f;
        // 全范围色度零点在 sampleMax/2,最大偏移到两端(0/sampleMax)。
        // 归一域与 limited 档同为 [-0.5,0.5](shader 公式 r=nY+nV*2(1-Kr)
        // 恒定),跨度 = sampleMax —— 此前 sampleMax*0.5 使 nC 落 [-1,1]:
        // 解码色度 ×2(强色被 saturate 钳断不可逆)、编码压进半码域
        // (2026-10-05 评审修;check_yuv_convert 此前只验 limited 档,
        // 该分支零数值验收)。
        c.cSpan = c.sampleMax;
    }
    c.kr = matrix == ColorMatrix::BT709 ? 0.2126f : 0.299f;
    c.kb = matrix == ColorMatrix::BT709 ? 0.0722f : 0.114f;
    return c;
}

} // namespace

bool D3D12Context::CreateConvertObjects(char *err, size_t errLen) noexcept {
    // YUV↔RGB 与 RTX 输出色域转换(_rsConvertIn/_rsConvertOut 两 RS 的 PSO 族)。
    // YUV↔RGB 转换(YUV 原生化):convertIn = t0/t1/t2 三张 SRV 表(Y/U/V)
    // + u0 一张 UAV 表(inputColor)+ b0 16 常量(extent 2+C0 4+C1 4+
    // chromaExtent 2+chromaScale 2+pitch 2);convertOut = t0 一张 SRV 表
    // + u0/u1/u2 三张 UAV 表(Y;U,V)+ b0 14 常量。深度/矩阵/范围全在
    // 常量里(R8/R16_UNORM 的 float 视图同构)。
    if (!CreateComputeRs(_device.Get(), 16, 3, 1, _rsConvertIn.GetAddressOf(),
                         "convert in", nullptr, err, errLen)) {
        return false;
    }
    if (!CreateComputePsoFor(_device.Get(), YUV_TO_BGRA_HLSL, "ConvertYuvToBgra",
                             _rsConvertIn.Get(), _psoConvertIn.GetAddressOf(),
                             "convert in", err, errLen) ||
        // Phase B:VS RGBP 直读核(同 RS/cbuffer,平面序 G/B/R 在核内换位)。
        !CreateComputePsoFor(_device.Get(), YUV_TO_BGRA_HLSL, "ConvertRgbToRgba",
                             _rsConvertIn.Get(), _psoConvertInRgb.GetAddressOf(),
                             "convert in rgb", err, errLen)) {
        return false;
    }
    if (!CreateComputeRs(_device.Get(), 14, 1, 3, _rsConvertOut.GetAddressOf(),
                         "convert out", nullptr, err, errLen)) {
        return false;
    }
    {
        struct OutCso { const char *entry; ID3D12PipelineState **pso; };
        const OutCso outCsos[]{
            { "BgraToYuvLuma", _psoConvertOutLuma.GetAddressOf() },
            { "BgraToYuvChroma", _psoConvertOutChroma.GetAddressOf() },
            // RTX Video 输出转换:与上面共用 _rsConvertOut,仅换 HLSL 入口。
            { "ScaledToYuvLuma", _psoConvertScaledLuma.GetAddressOf() },
            { "ScaledToYuvChroma", _psoConvertScaledChroma.GetAddressOf() },
            { "PqToYuvLuma", _psoPqLuma.GetAddressOf() },
            { "PqToYuvChroma", _psoPqChroma.GetAddressOf() },
            // PQ 域插帧(fgHdrInterp):编码 + 码域→P10。共用 _rsConvertOut
            // 布局;HdrToPq 只消费 u0,u1/u2 表绑占位描述符(根参数必须全绑)。
            { "HdrToPq", _psoHdrToPq.GetAddressOf() },
            { "PqCodesToYuvLuma", _psoPqCodesLuma.GetAddressOf() },
            { "PqCodesToYuvChroma", _psoPqCodesChroma.GetAddressOf() },
            // Phase B:RGBP 出侧(直写 + RTX 缩放版;u0/u1/u2 = G/B/R)。
            { "RgbaToRgbPlanes", _psoRgbOut.GetAddressOf() },
            { "ScaledToRgbPlanes", _psoRgbScaled.GetAddressOf() },
        };
        constexpr const char *kOutHlslByEntry[] = {
            BGRA_TO_YUV_HLSL, BGRA_TO_YUV_HLSL,
            COLOR_TO_YUV_SCALED_HLSL, COLOR_TO_YUV_SCALED_HLSL,
            FP16_TO_YUV_PQ_HLSL, FP16_TO_YUV_PQ_HLSL,
            HDR_TO_PQ_HLSL, PQ_CODES_TO_YUV_HLSL, PQ_CODES_TO_YUV_HLSL,
            RGB_TO_PLANAR_HLSL, RGB_TO_PLANAR_HLSL,
        };
        for (size_t ci = 0; ci < std::size(outCsos); ++ci) {
            if (!CreateComputePsoFor(_device.Get(), kOutHlslByEntry[ci], outCsos[ci].entry,
                                     _rsConvertOut.Get(), outCsos[ci].pso,
                                     "convert out", err, errLen)) {
                return false;
            }
        }
    }
    return true;
}


// cbuffer ConvertInParams(root constants,三处同步铁律:HLSL cbuffer /
// Num32BitValues=16 / SetComputeRoot32BitConstants):
// @0-1 uint2 DstExtent(inputColor 尺寸,dispatch 边界)
// @2 containerMax @3 yLo @4 yScale(1/ySpan) @5 cMid @6 cScale(1/cSpan)
// @7 kr @8 kb @9 pad
void D3D12Context::RecordConvertInput(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                      ColorMatrix matrix, ColorRange range,
                                      D3D12_RESOURCE_STATES stateAfter) noexcept {
    // cl 由调用方给定(nvof CL 或槽 CL)—— 见头文件注释,录错列表 = 被销毁。
    ID3D12GraphicsCommandList *cl = &clRef;

    // 1) 转换 dispatch:typed buffer SRV 直读 uploadYuv(2026-10-02 起;
    // upload→yuvIn 的 3×拷贝与 yuvIn 纹理已撤 —— tsrv_probe 复核的驱动
    // 形态)。UPLOAD 堆 buffer 恒 GENERIC_READ(全读态),SRV 绑定免屏障;
    // CPU 写(persist-mapped 行拷贝)→ GPU 读的同步不变(提交序)。
    // 采样字平铺索引:x + y*Pitch(Pitch 以采样字计,@14/15)。
    D3D12_RESOURCE_BARRIER toUav[1]{
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(1, toUav);
    const YuvCoeffs cf = YuvCoeffsFor(matrix, range, _bitDepth);
    cl->SetComputeRootSignature(_rsConvertIn.Get());
    cl->SetPipelineState(_isRgb ? _psoConvertInRgb.Get() : _psoConvertIn.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };
    const UINT extent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const float consts[8]{ cf.containerMax, cf.yLo, 1.0f / cf.ySpan, cf.cMid,
                           1.0f / cf.cSpan, cf.kr, cf.kb, 0.0f };
    cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl->SetComputeRoot32BitConstants(0, 8, consts, 2);
    // ChromaExtent/ChromaScale(@10-13):420=(半,0.5) 与旧硬编码恒等;
    // 422=(半宽,1)、444=(全,(1,1) 零插值)。PSO 由 _isRgb 选(RGB 核不
    // 消费色度常量)。
    const UINT chromaExtent[2]{ static_cast<UINT>(_chromaW), static_cast<UINT>(_chromaH) };
    const float chromaScale[2]{ _subW ? 0.5f : 1.0f, _subH ? 0.5f : 1.0f };
    cl->SetComputeRoot32BitConstants(0, 2, chromaExtent, 10);
    cl->SetComputeRoot32BitConstants(0, 2, chromaScale, 12);
    const UINT texelBytes = _bitDepth > 8 ? 2u : 1u;
    const UINT planePitches[2]{
        static_cast<UINT>(slot.uploadPitchYuv[0]) / texelBytes,
        static_cast<UINT>(slot.uploadPitchYuv[1]) / texelBytes,
    };
    cl->SetComputeRoot32BitConstants(0, 2, planePitches, 14);
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvYuvIn0)); // t0 PlaneY
    cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::SrvYuvIn1)); // t1 PlaneU
    cl->SetComputeRootDescriptorTable(3, gpu(HeapSlot::SrvYuvIn2)); // t2 PlaneV
    cl->SetComputeRootDescriptorTable(4, gpu(HeapSlot::UavInput)); // u0 inputColor
    cl->Dispatch((static_cast<UINT>(_width) + 7) / 8, (static_cast<UINT>(_height) + 7) / 8, 1);

    // 2) inputColor → stateAfter(NSR=NGX 待读;COMMON=skipEval)。单屏障
    //    (yuvIn 归位屏障已随纹理删除)。
    D3D12_RESOURCE_BARRIER tail[1];
    tail[0] = Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, stateAfter);
    cl->ResourceBarrier(1, tail);
}

bool D3D12Context::RecordReadbackCopy(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                      char *err, size_t errLen,
                                      int fgGen) noexcept {
    // RecordColorOutput 之后调用:yuvOut 处于 UAV 态 → COPY_SOURCE → 拷贝
    // → COMMON。三平面各自 footprint(格式同资源,跨格式 = 静默 E_INVALIDARG)。
    // fgGen >= 0 = 拷入 FG 插值帧第 fgGen 组回读缓冲(真实帧与各插值槽先后
    // 转换+回读,共用 yuvOut,目标缓冲两两不同)。cl 由调用方显式给定
    // (真实帧 = base CL,插值 = fg CL)。
    ID3D12GraphicsCommandList *cl = &clRef;
    const DXGI_FORMAT yuvFmt = _outFmt;
    const int planeW[3]{ _outW, _outChromaW, _outChromaW };
    const int planeH[3]{ _outH, _outChromaH, _outChromaH };
    // 屏障合批(2026-09-25):三平面 UAV→COPY_SOURCE 一批、拷贝、
    // COPY_SOURCE→COMMON 一批(此前逐平面 1+1,FG 6x 每帧 36 次驱动调用;
    // 平面间无依赖,纯重排)。RecordConvertInput 同款写法。
    D3D12_RESOURCE_BARRIER toCopySrc[3];
    for (int i = 0; i < 3; ++i) {
        toCopySrc[i] = Transition(slot.yuvOut[i].Get(),
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    cl->ResourceBarrier(3, toCopySrc);
    for (int i = 0; i < 3; ++i) {
        ID3D12Resource *dstBuf = fgGen >= 0 ? slot.readbackFg[fgGen][i].res.Get()
                                            : slot.readbackYuv[i].res.Get();
        const size_t dstPitch = fgGen >= 0 ? slot.readbackPitchFg[fgGen][i]
                                           : slot.readbackPitchYuv[i];
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = slot.yuvOut[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dstBuf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = yuvFmt;
        dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(planeW[i]);
        dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(planeH[i]);
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(dstPitch);
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER backToCommon[3];
    for (int i = 0; i < 3; ++i) {
        backToCommon[i] = Transition(slot.yuvOut[i].Get(),
                                     D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    cl->ResourceBarrier(3, backToCommon);
    return true;
}

void D3D12Context::RecordColorOutput(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                     ID3D12Resource *srcColor, UINT srcSrvIndex,
                                     ColorOutKind kind, int srcW, int srcH,
                                     ColorMatrix matrix, ColorRange range,
                                     D3D12_RESOURCE_STATES stateBefore) noexcept {
    // RTX Video 管线的颜色输出(PIPE 尺寸源 → OUT 尺寸平面;契约同
    // 见头文件注释)。PIPE==OUT 且 SDR 时坐标恒等映射,
    // 与旧路径逐位同价。
    ID3D12GraphicsCommandList *cl = &clRef;
    if (stateBefore != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER toNsr[1]{
            Transition(srcColor, stateBefore,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cl->ResourceBarrier(1, toNsr);
    }
    D3D12_RESOURCE_BARRIER toUav[3];
    for (int i = 0; i < 3; ++i) {
        toUav[i] = Transition(slot.yuvOut[i].Get(),
                              D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    cl->ResourceBarrier(3, toUav);

    cl->SetComputeRootSignature(_rsConvertOut.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    // 深度 = 源位深(SDR 输出 = 源同格式,见 _outPlaneBytes 定义处):
    // 此前 >8bit 一律按 10bit 量化,12/16bit 源的输出码域错位(整帧暗
    // 4×/256×,2026-10-04 评审修)。HDR 分支(PQ)不消费阶梯常量,无影响。
    const YuvCoeffs cf = YuvCoeffsFor(matrix, range, _bitDepth);
    const bool fp16 = kind != ColorOutKind::Sdr; // P10 输出契约 + ChromaStep (2,2)
    ID3D12PipelineState *lumaPso = nullptr;
    ID3D12PipelineState *chromaPso = nullptr;
    switch (kind) {
    case ColorOutKind::HdrScRgb:
        lumaPso = _psoPqLuma.Get();
        chromaPso = _psoPqChroma.Get();
        break;
    case ColorOutKind::HdrPqCodes:
        lumaPso = _psoPqCodesLuma.Get();
        chromaPso = _psoPqCodesChroma.Get();
        break;
    default:
        // 1:1(PIPE==OUT)改派直写 PSO:双线性 4-tap 的权重在恒等映射下
        // 退化为单位冲激,但 4 次 load 一次不少 —— 直写单 load 省 4 倍读
        // 带宽。逐位同价由函数头注释背书(2026-09-25)。
        lumaPso = (srcW == _outW && srcH == _outH)
                      ? _psoConvertOutLuma.Get() : _psoConvertScaledLuma.Get();
        chromaPso = (srcW == _outW && srcH == _outH)
                        ? _psoConvertOutChroma.Get() : _psoConvertScaledChroma.Get();
        break;
    }
    // ChromaStep:SDR 输出 = 输入布局(同格式出);HDR P10 输出恒 (2,2)。
    const UINT step[2]{
        static_cast<UINT>(fp16 ? 2 : (_subW ? 2 : 1)),
        static_cast<UINT>(fp16 ? 2 : (_subH ? 2 : 1)),
    };
    // 单个转换 dispatch(_rsConvertOut 契约同 RecordColorOutput;恒绑满 u0/u1/u2
    // 三张表 —— 未消费的表绑合法描述符,shader 不读无害)。
    auto recordConvertPass = [&](ID3D12PipelineState *pso, const UINT (&extent)[2],
                                 const UINT (&srcExtent)[2], const float (&consts)[8],
                                 UINT u0, UINT u1, UINT u2) {
        cl->SetPipelineState(pso);
        cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
        cl->SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
        cl->SetComputeRoot32BitConstants(0, 8, consts, 4);
        cl->SetComputeRoot32BitConstants(0, 2, step, 12);
        cl->SetComputeRootDescriptorTable(1, gpu(srcSrvIndex));
        cl->SetComputeRootDescriptorTable(2, gpu(static_cast<HeapSlot>(u0)));
        cl->SetComputeRootDescriptorTable(3, gpu(static_cast<HeapSlot>(u1)));
        cl->SetComputeRootDescriptorTable(4, gpu(static_cast<HeapSlot>(u2)));
        cl->Dispatch((extent[0] + 7) / 8, (extent[1] + 7) / 8, 1);
    };
    constexpr UINT kUnusedUav = static_cast<UINT>(HeapSlot::UavYuvOut1);

    if (kind == ColorOutKind::Sdr && _isRgb) {
        // Phase B:RGBP SDR 输出(PIPE→OUT 双线性;1:1 时坐标恒等映射,
        // 与直写逐位同价)。RGB 全域直码,零矩阵。
        const UINT extent[2]{ static_cast<UINT>(_outW), static_cast<UINT>(_outH) };
        const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
        const float consts[8]{};
        recordConvertPass(_psoRgbScaled.Get(), extent, srcExtent, consts,
                          HeapSlot::UavYuvOut0, HeapSlot::UavYuvOut1, HeapSlot::UavYuvOut2);
    } else if (kind == ColorOutKind::Sdr) {
        const UINT extent[2]{ static_cast<UINT>(_outW), static_cast<UINT>(_outH) };
        const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
        const float consts[8]{ cf.kr, cf.kb,
                               cf.yLo / cf.containerMax, cf.ySpan / cf.containerMax,
                               0.0f, 0.0f, 0.0f, 0.0f };
        recordConvertPass(lumaPso, extent, srcExtent, consts,
                          HeapSlot::UavYuvOut0, HeapSlot::UavYuvOut1, kUnusedUav);
        const UINT cExtent[2]{ static_cast<UINT>(_outChromaW), static_cast<UINT>(_outChromaH) };
        const float cConsts[8]{ cf.kr, cf.kb,
                                cf.cMid / cf.containerMax, cf.cSpan / cf.containerMax,
                                0.0f, 0.0f, 0.0f, 0.0f };
        recordConvertPass(chromaPso, cExtent, srcExtent, cConsts,
                          HeapSlot::UavYuvOut1, HeapSlot::UavYuvOut2, kUnusedUav);
    } else {
        // HDR(HdrScRgb / HdrPqCodes):Kr/Kb = BT.2020(0.2627/0.0593);
        // limited 10bit luma 64+876y、chroma 512+896c(÷CM 折进常量,与 SDR
        // 路径同布局)。两者常量组相同,仅 PSO 对不同(码域源免 PqEncode)。
        constexpr float kKr2020 = 0.2627f;
        constexpr float kKb2020 = 0.0593f;
        const UINT extent[2]{ static_cast<UINT>(_outW), static_cast<UINT>(_outH) };
        const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
        const float consts[8]{ kKr2020, kKb2020,
                               64.0f / 1023.0f, 876.0f / 1023.0f,
                               0.0f, 0.0f, 0.0f, 0.0f };
        recordConvertPass(lumaPso, extent, srcExtent, consts,
                          HeapSlot::UavYuvOut0, HeapSlot::UavYuvOut1, kUnusedUav);
        const UINT cExtent[2]{ static_cast<UINT>(_outChromaW), static_cast<UINT>(_outChromaH) };
        const float cConsts[8]{ kKr2020, kKb2020,
                                512.0f / 1023.0f, 896.0f / 1023.0f,
                                0.0f, 0.0f, 0.0f, 0.0f };
        recordConvertPass(chromaPso, cExtent, srcExtent, cConsts,
                          HeapSlot::UavYuvOut1, HeapSlot::UavYuvOut2, kUnusedUav);
    }

    D3D12_RESOURCE_BARRIER back[1]{
        Transition(srcColor,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
    };
    cl->ResourceBarrier(1, back);
}

void D3D12Context::RecordHdrToPq(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                 D3D12_RESOURCE_STATES stateBefore) noexcept {
    // PQ 域插帧编码 pass(仅 fgHdrInterp 且 fg 段开启被记录):hdrColor(FP16
    // scRGB,1:1 @PIPE)→ fgBack(FP16 PQ 码 ≤1.0,UAV)→ NSR 作 DLSSG
    // backbuffer。DLSSG 的 ColorBuffersHDR=1 路径对 >1.0 线性值不保真,码域
    // 走 LDR 路径无损(原生 HDR 直通同域实证);插值输出码域直读(post 侧
    // PqCodesToYuv 免逐像素 PqEncode)。
    ID3D12GraphicsCommandList *cl = &clRef;
    if (stateBefore != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER toNsr[1]{
            Transition(slot.hdrColor.Get(), stateBefore,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cl->ResourceBarrier(1, toNsr);
    }
    D3D12_RESOURCE_BARRIER toUav[1]{
        Transition(slot.fgBack.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(1, toUav);

    cl->SetComputeRootSignature(_rsConvertOut.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    cl->SetPipelineState(_psoHdrToPq.Get());
    {
        // 编码只消费 t0/u0;u1/u2 表绑 yuvOut 占位(根签名必须全绑,本机
        // 驱动对未绑定根参数的死法见 #41-①)。
        const UINT extent[2]{ static_cast<UINT>(_pipeW), static_cast<UINT>(_pipeH) };
        const float consts[14]{};
        cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
        cl->SetComputeRoot32BitConstants(0, 2, extent, 2); // SourceExtent:1:1
        cl->SetComputeRoot32BitConstants(0, 10, consts, 4);
        cl->SetComputeRootDescriptorTable(1, gpu(kSrvHdrColor));
        cl->SetComputeRootDescriptorTable(2, gpu(kUavFgBack));
        cl->SetComputeRootDescriptorTable(3, gpu(HeapSlot::UavYuvOut0)); // 占位(未消费)
        cl->SetComputeRootDescriptorTable(4, gpu(HeapSlot::UavYuvOut1)); // 占位(未消费)
        cl->Dispatch((static_cast<UINT>(_pipeW) + 7) / 8, (static_cast<UINT>(_pipeH) + 7) / 8, 1);
    }
    // UAV→NSR:fgBack 即 DLSSG backbuffer(eval 前 NSR 化,替代旧 fgBar)。
    D3D12_RESOURCE_BARRIER toNsr[1]{
        Transition(slot.fgBack.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cl->ResourceBarrier(1, toNsr);
}

} // namespace vsdlssnr
