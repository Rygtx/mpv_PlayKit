#include "d3d12_context.h"
#include "d3d12_internal.h"
#include "d3d12_shaders.h"

#include <algorithm>
#include <cstdio>

// 光流 guidance 子系统(NVOF densify/guidance、FFX、mvec 放大; CreateOfObjects)—— 方法体逐字迁移。

namespace vsdlssnr {

void TimingStatusLine(const char *line) noexcept; // 本体在 dlssnr_context(免重 include)
bool ProbeEnabled() noexcept;                    // 同上(VSDLSSNR_PROBE)

bool D3D12Context::CreateOfObjects(char *err, size_t errLen) noexcept {
    // OF guidance/densify + NVOF 输入降采样 + FFX prepare/densify + mvec 放大。
    // NVOF guidance(PORTING #6):densify = t0-t3 SRV 表 + u0/u1 独立 UAV 表
    // + b0 10 常量(srcWH 2+flowWH 2+flags 4+MotionScale 2);guidance 降采样
    // = t0/t1 SRV 表 + u0/u1 独立 UAV 表 + b0 12 常量(独立表参数,同表重叠
    // range 非法 —— 禁忌由 CreateComputeRs 构造性规避)。
    if (!CreateComputeRs(_device.Get(), 10, 4, 2, _rsDensify.GetAddressOf(),
                         "densify", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), DENSIFY_HLSL, "Densify", _rsDensify.Get(),
                             _psoDensify.GetAddressOf(), "densify", err, errLen)) {
        return false;
    }
    if (!CreateComputeRs(_device.Get(), 12, 2, 2, _rsGuidance.GetAddressOf(),
                         "guidance", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), GUIDANCE_DOWNSAMPLE_HLSL, "DownsampleGuidance",
                             _rsGuidance.Get(), _psoGuidanceDownsample.GetAddressOf(),
                             "guidance", err, errLen)) {
        return false;
    }
    // 光流输入 GPU 降采样(#46/#48):b0 5 常量 + t0 单表 + u0 单表。
    if (ProbeEnabled()) TimingStatusLine("PROBE: pso base ok");
    if (!CreateComputeRs(_device.Get(), 5, 1, 1, _rsNvofDownsample.GetAddressOf(),
                         "nvof downsample", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), NVOF_DOWNSAMPLE_HLSL, "NvofDownsample",
                             _rsNvofDownsample.Get(), _psoNvofDownsample.GetAddressOf(),
                             "nvof downsample", err, errLen)) {
        return false;
    }
    if (ProbeEnabled()) TimingStatusLine("PROBE: pso nvofds done");
    // AMD 光流后端 PSO(FFX Prepare/Densify)。仅 of_backend 选择 ffx 时才
    // 被消费;构造失败 = 初始化失败(与其它 PSO 同语义,不静默降级)。
    {
        if (!CreateOfPso(_device.Get(), FFX_PREPARE_HLSL, "Prepare", 4, 1, 1,
                         _rsFfxPrepare.GetAddressOf(), _psoFfxPrepare.GetAddressOf(),
                         "ffx prepare", err, errLen) ||
            !CreateOfPso(_device.Get(), FFX_DENSIFY_HLSL, "Densify", 8, 1, 2,
                         _rsFfxDensify.GetAddressOf(), _psoFfxDensify.GetAddressOf(),
                         "ffx densify", err, errLen)) {
            return false;
        }
    }
        // mvec 放大(源→PIPE):CreateOfPso 通用形态(1 SRV + 1 UAV + 4 常量)。
        if (!CreateOfPso(_device.Get(), MVEC_SCALE_HLSL, "ScaleMotion", 4, 1, 1,
                         _rsMotionScale.GetAddressOf(), _psoMotionScale.GetAddressOf(),
                         "motion scale", err, errLen)) {
            return false;
        }
    return true;
}


bool D3D12Context::BindNvofResources(ID3D12Resource *flowFwd, ID3D12Resource *flowBwd,
                                     ID3D12Resource *costFwd, ID3D12Resource *costBwd,
                                     ID3D12Resource *inputFwd, ID3D12Resource *inputBwd) noexcept {
    // 每个槽的描述符堆各写一份(densify 在槽列表上执行)。未启用的 cost/
    // backward 槽位用 flowFwd 的视图占位 —— densify 着色器按 cbuffer 旗标
    // 跳过读取,占位永不被有效采样。inputFwd/inputBwd(23/24)为注册输入
    // 纹理的 UAV(GPU 光流输入降采样直写目标);null 时用本槽 outputColor
    // 占位(带 UAV flag)。绝不创建 NULL 描述符(本机驱动上会异步 TDR,
    // 见 CreateSlotResources 注释)。
    if (!_device || !flowFwd) return false;
    ID3D12Resource *views[4]{ flowFwd,
                              flowBwd ? flowBwd : flowFwd,
                              costFwd ? costFwd : flowFwd,
                              costBwd ? costBwd : flowFwd };
    for (int i = 0; i < kSlotCount; ++i) {
        if (!_slots[i].srvUavHeap) return false;
        HeapBinder h{ _device.Get(), _slots[i] };
        _device->CreateShaderResourceView(views[0], nullptr, h.cpu(HeapSlot::SrvFlowF));
        _device->CreateShaderResourceView(views[1], nullptr, h.cpu(HeapSlot::SrvFlowB));
        _device->CreateShaderResourceView(views[2], nullptr, h.cpu(HeapSlot::SrvCostF));
        _device->CreateShaderResourceView(views[3], nullptr, h.cpu(HeapSlot::SrvCostB));
        _device->CreateUnorderedAccessView(inputFwd ? inputFwd : _slots[i].outputColor.Get(),
                                           nullptr, nullptr, h.cpu(HeapSlot::UavNvofInput0));
        _device->CreateUnorderedAccessView(inputBwd ? inputBwd : _slots[i].outputColor.Get(),
                                           nullptr, nullptr, h.cpu(HeapSlot::UavNvofInput1));
    }
    return true;
}

void D3D12Context::RecordDensify(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                 uint32_t denseW, uint32_t denseH,
                                 uint32_t flowW, uint32_t flowH,
                                 bool hasForwardCost, bool hasBackward,
                                 bool hasBackwardCost,
                                 float motionScaleX, float motionScaleY,
                                 UINT uavMotion, UINT uavConfidence) noexcept {
    // 在 NVOF 会话的 nvof CL 上执行(门内、execute 完成后)。NGX evaluate
    // 可能重绑堆/根签名;槽列表上的其它 pass 仍各自先重绑(同款)。
    // shader 的 SourceExtent 语义 = 稠密目标尺寸:流场网格均匀覆盖会话输入
    // 范围,SourceExtent 即会话输入被线性映射到的目标域 —— 源尺寸管线(源,
    // MotionScale=会话→源)与 follow 内部管线(内部,= 会话尺寸,
    // MotionScale=(1,1))共用同一几何公式。
    ID3D12GraphicsCommandList *cl = &clRef;

    cl->SetComputeRootSignature(_rsDensify.Get());
    cl->SetPipelineState(_psoDensify.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const UINT srcWH[2]{ static_cast<UINT>(denseW), static_cast<UINT>(denseH) };
    const UINT flowWH[2]{ flowW, flowH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, flowWH, 2);
    const UINT flags[3]{ hasForwardCost ? 1u : 0u,
                         hasBackward ? 1u : 0u, hasBackwardCost ? 1u : 0u };
    cl->SetComputeRoot32BitConstants(0, 3, flags, 4);
    const float motionScale[2]{ motionScaleX, motionScaleY };
    cl->SetComputeRoot32BitConstants(0, 2, motionScale, 7);

    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvFlowF)); // t0 ForwardFlow
    cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::SrvFlowB)); // t1 BackwardFlow
    cl->SetComputeRootDescriptorTable(3, gpu(HeapSlot::SrvCostF)); // t2 ForwardCost
    cl->SetComputeRootDescriptorTable(4, gpu(HeapSlot::SrvCostB)); // t3 BackwardCost
    cl->SetComputeRootDescriptorTable(5, gpu(uavMotion));      // u0 DenseMotion
    cl->SetComputeRootDescriptorTable(6, gpu(uavConfidence));  // u1 DenseConfidence
    cl->Dispatch((static_cast<UINT>(denseW) + 7) / 8,
                 (static_cast<UINT>(denseH) + 7) / 8, 1);
}

void D3D12Context::RecordGuidancePass(FrameSlot &slot, ID3D12PipelineState *pso,
                                      UINT srv0, UINT srv1, UINT uav0, UINT uav1,
                                      UINT dispatchX, UINT dispatchY) noexcept {
    // guidance 降采样:12 常量 cbuffer(与残差管线同布局),MotionScale =
    // 内部/源(逐轴),运动向量换算到内部像素单位(Magpie 同款,
    // PARAM_MVEC_SCALE 保持 1)。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    cl->SetComputeRootSignature(_rsGuidance.Get());
    cl->SetPipelineState(pso);
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(_internalWidth), static_cast<UINT>(_internalHeight) };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    const float zero = 0.0f;
    cl->SetComputeRoot32BitConstants(0, 1, &zero, 4); // Padding0
    const float motionScale[2]{
        static_cast<float>(_internalWidth) / static_cast<float>(_width),
        static_cast<float>(_internalHeight) / static_cast<float>(_height)
    };
    cl->SetComputeRoot32BitConstants(0, 2, motionScale, 5);

    cl->SetComputeRootDescriptorTable(1, gpu(srv0));
    cl->SetComputeRootDescriptorTable(2, gpu(srv1));
    cl->SetComputeRootDescriptorTable(3, gpu(uav0));
    cl->SetComputeRootDescriptorTable(4, gpu(uav1));
    cl->Dispatch(dispatchX, dispatchY, 1);
}

void D3D12Context::RecordGuidanceDownsample(FrameSlot &slot) noexcept {
    RecordGuidancePass(slot, _psoGuidanceDownsample.Get(),
                       10, 11,          // t0 motion, t1 confidence
                       20, 21,          // u0 reducedMotion, u1 reducedConfidence
                       (static_cast<UINT>(_internalWidth) + 7) / 8,
                       (static_cast<UINT>(_internalHeight) + 7) / 8);
}

void D3D12Context::RecordNvofDownsample(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                        int dstW, int dstH, int inputIndex) noexcept {
    // 光流输入 GPU 降采样(#46/#48):在 NVOF 会话 nvof CL 第一次提交上
    // 执行,替代 follow 分支的 CopyTextureRegion。inputColor 由同 CL 上
    // 先行的 RecordConvertInput 产出(已是 NSR,直读,无屏障);注册纹理
    // 的 COMMON→UAV→COMMON 屏障由调用方(NvofContext)负责 —— 纹理归会话
    // 所有;copyFence 在 CL 完成后才 Signal,execute 的 inFence[0] 天然
    // 覆盖本 dispatch。NGX evaluate 可能重绑堆/根签名,与其它 pass 同款
    // 先重绑。
    ID3D12GraphicsCommandList *cl = &clRef;
    cl->SetComputeRootSignature(_rsNvofDownsample.Get());
    cl->SetPipelineState(_psoNvofDownsample.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    // cbuffer 布局(与 NVOF_DOWNSAMPLE_HLSL 同步,三处同步铁律):
    // srcWH@0 dstWH@2 pad@4。
    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(dstW), static_cast<UINT>(dstH) };
    const UINT pad[1]{ 0u };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    cl->SetComputeRoot32BitConstants(0, 1, pad, 4);

    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));               // t0 inputColor(NSR)
    cl->SetComputeRootDescriptorTable(2, gpu(static_cast<HeapSlot>(HeapSlot::UavNvofInput0 + inputIndex))); // u0 NvofInput[cur]
    cl->Dispatch((static_cast<UINT>(dstW) + 7) / 8,
                 (static_cast<UINT>(dstH) + 7) / 8, 1);
}

bool D3D12Context::BindOfResources(ID3D12Resource *ffxInput, ID3D12Resource *ffxSparse) noexcept {
    // 每槽堆写一份;会话纹理由 context 持有、销毁走退役名单,视图内容在
    // 纹理存活期内有效。调用方保证只在会话建立时调用(PoolHold 内)。
    if (!_device) return false;
    for (int i = 0; i < kSlotCount; ++i) {
        if (!_slots[i].srvUavHeap) return false;
        HeapBinder h{ _device.Get(), _slots[i] };
        if (ffxInput) _device->CreateUnorderedAccessView(ffxInput, nullptr, nullptr, h.cpu(HeapSlot::UavFfxInput));
        if (ffxSparse) _device->CreateShaderResourceView(ffxSparse, nullptr, h.cpu(HeapSlot::SrvFfxSparse));
    }
    return true;
}

void D3D12Context::RecordFfxPrepare(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                    uint32_t srcW, uint32_t srcH,
                                    uint32_t dstW, uint32_t dstH) noexcept {
    // inputColor(槽 0 SRV,BGRA8)→ ffxInput(49,R8G8B8A8):box 平均 + 换序。
    // inputColor 处于 NSR(convert 产出),ffxInput 的 UAV 态由调用方管理。
    ID3D12GraphicsCommandList *cl = &clRef;
    cl->SetComputeRootSignature(_rsFfxPrepare.Get());
    cl->SetPipelineState(_psoFfxPrepare.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const UINT srcWH[2]{ srcW, srcH };
    const UINT dstWH[2]{ dstW, dstH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));  // t0 inputColor(NSR)
    cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::UavFfxInput)); // u0 ffxInput
    cl->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
}

void D3D12Context::RecordFfxDensify(ID3D12GraphicsCommandList &clRef, FrameSlot &slot,
                                    uint32_t denseW, uint32_t denseH,
                                    uint32_t ofW, uint32_t ofH,
                                    uint32_t sparseW, uint32_t sparseH,
                                    float scaleX, float scaleY,
                                    UINT uavMotion, UINT uavConfidence) noexcept {
    // 稀疏流(50)→ 稠密运动 + 置信度。SourceExtent = 稠密目标尺寸
    // (dispatch 范围);VectorScale = dense/OF(源尺寸管线 = 源/OF,
    // follow 内部管线 = 会话/OF)。motion/confidence 的 UAV 态转移由
    // 调用方(postExecute lambda)负责,与 NVOF densify 同契约。
    ID3D12GraphicsCommandList *cl = &clRef;
    cl->SetComputeRootSignature(_rsFfxDensify.Get());
    cl->SetPipelineState(_psoFfxDensify.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const UINT srcWH[2]{ denseW, denseH };
    const UINT ofWH[2]{ ofW, ofH };
    const UINT spWH[2]{ sparseW, sparseH };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, ofWH, 2);
    cl->SetComputeRoot32BitConstants(0, 2, spWH, 4);
    const float scale[2]{ scaleX, scaleY };
    cl->SetComputeRoot32BitConstants(0, 2, scale, 6);
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvFfxSparse));            // t0 sparseFlow
    cl->SetComputeRootDescriptorTable(2, gpu(uavMotion));     // u0 DenseMotion
    cl->SetComputeRootDescriptorTable(3, gpu(uavConfidence)); // u1 DenseConfidence
    cl->Dispatch((denseW + 7) / 8, (denseH + 7) / 8, 1);
}

void D3D12Context::RecordMotionScale(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                                     int srcW, int srcH) noexcept {
    // 源尺寸运动场(NSR)→ motionDense(PIPE 尺寸,UAV)。motionDense 由
    // 调用方(帧路径)按 COMMON→UAV 屏障后调用,UAV→NSR 收尾归调用方。
    const UINT extent[2]{ static_cast<UINT>(_pipeW), static_cast<UINT>(_pipeH) };
    const UINT srcExtent[2]{ static_cast<UINT>(srcW), static_cast<UINT>(srcH) };
    HeapBinder gpu{ _device.Get(), cl, slot };
    cl.SetComputeRootSignature(_rsMotionScale.Get());
    cl.SetPipelineState(_psoMotionScale.Get());
    cl.SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl.SetComputeRoot32BitConstants(0, 2, srcExtent, 2);
    cl.SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvMotion));             // t0 slot.motion SRV
    cl.SetComputeRootDescriptorTable(2, gpu(kUavMotionDense)); // u0 motionDense
    cl.Dispatch((extent[0] + 7) / 8, (extent[1] + 7) / 8, 1);
}

} // namespace vsdlssnr
