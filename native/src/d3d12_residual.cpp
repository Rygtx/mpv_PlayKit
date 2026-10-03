#include "d3d12_context.h"
#include "d3d12_internal.h"
#include "d3d12_shaders.h"

// 内部分辨率降采样残差子系统(RebuildScaling/RecordPass 族; CreateResidualObjects)
// —— 2026-10-04 Pass 组拆分的派生件,方法体逐字迁移。
//
// Residual pipeline (ported from Magpie DLSSNRFilter.cpp, 2d37f8c0 / v0.6.6):
// two-pass Lanczos2 color downsample (vertical -> horizontal, 1cde1bae
// "lanczos2-aa"; at equal extents Lanczos2 degenerates to an exact copy) ->
// NGX evaluate at internal resolution ->
// PrepareResidual (per-pixel fine controls in the low-resolution domain) ->
// Catmull-Rom horizontal residual upsample (skipped at equal width) ->
// Catmull-Rom vertical + composite (saturate(original + residual)).

namespace vsdlssnr {

bool D3D12Context::CreateResidualObjects(char *err, size_t errLen) noexcept {
    // 残差五段:b0 = 12 root constants (Magpie ResampleConstants, 48B),
    // t0/t1 独立 SRV 表(the passes need non-adjacent descriptor pairs)+ u0。
    if (!CreateComputeRs(_device.Get(), 12, 2, 1, _rsCompute.GetAddressOf(),
                         "compute", nullptr, err, errLen)) {
        return false;
    }
    {
        struct Cso { const char *hlsl; const char *entry; ID3D12PipelineState **pso; };
        const Cso csos[] = {
            { DOWNSAMPLE_HLSL, "DownsampleColorVertical", _psoDownsampleVertical.GetAddressOf() },
            { DOWNSAMPLE_HLSL, "DownsampleColorHorizontal", _psoDownsampleHorizontal.GetAddressOf() },
            { RESIDUAL_PREPARE_HLSL, "PrepareResidual", _psoPrepare.GetAddressOf() },
            { RESIDUAL_HORIZONTAL_HLSL, "UpsampleResidualHorizontal", _psoHorizontal.GetAddressOf() },
            { RESIDUAL_VERTICAL_HLSL, "CompositeResidualVertical", _psoVertical.GetAddressOf() },
        };
        for (const auto &cso : csos) {
            if (!CreateComputePsoFor(_device.Get(), cso.hlsl, cso.entry, _rsCompute.Get(),
                                     cso.pso, "compute", err, errLen)) {
                return false;
            }
        }
    }
    return true;
}


bool D3D12Context::RebuildScaling(int internalW, int internalH, char *err, size_t errLen) noexcept {
    // Replaces every slot's scaling textures; the caller must hold a PoolHold
    // (all slots idle, pool sealed) so no frame can still reference them.
    // 恒等归一(2026-09-25):内部尺寸 == 源尺寸时,缩放/残差管线退化为
    // 恒等全分辨率搬运(等尺寸 Lanczos2/Catmull-Rom,产出与直连逐位相同),
    // 归一为 scaling-off —— 不建 6 张全尺寸中间纹理,全部消费方经
    // HasScaling()==false 自动走直连。InternalSize 对 >=100% 已返回原尺寸,
    // 此处兜住"scalingEnabled=1 + res=100"组合(出厂默认)每帧白付的
    // 4 个全分辨率 pass。请求侧参数不动(面板如实显示用户请求)。
    if (internalW >= _width && internalH >= _height) {
        ClearScalingResources();
        return true;
    }
    for (int i = 0; i < kSlotCount; ++i) {
        ClearScalingForSlot(_slots[i]);
        if (!CreateScalingForSlot(_slots[i], internalW, internalH, err, errLen)) {
            _scalingReady = false;
            _internalWidth = 0;
            _internalHeight = 0;
            return false;
        }
    }
    _internalWidth = internalW;
    _internalHeight = internalH;
    _scalingReady = true;
    return true;
}

void D3D12Context::ClearScalingResources() noexcept {
    // Caller must hold a PoolHold (see RebuildScaling).
    for (int i = 0; i < kSlotCount; ++i) {
        ClearScalingForSlot(_slots[i]);
    }
    // zero the reported internal size too: stats consumers treat 0 as
    // "scaling disabled" instead of a stale percentage of a previous state
    _internalWidth = 0;
    _internalHeight = 0;
    _scalingReady = false;
}

bool D3D12Context::CreateScalingForSlot(FrameSlot &slot, int internalW, int internalH, char *err, size_t errLen) noexcept {
    if (!_rsCompute && !CreateComputeObjects(err, errLen)) return false; // 幂等保护(Initialize 已建)

    // rebuild internal textures (sizes depend on the resolution percent)
    slot.reducedColor.Reset();
    slot.reducedDenoised.Reset();
    slot.controlledRes.Reset();
    slot.horizontalRes.Reset();
    slot.reducedMotion.Reset();
    slot.reducedConfidence.Reset();
    if (!CreateColorTexture(slot.reducedColor.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(slot.reducedDenoised.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    // R16G16B16A16_FLOAT: the controlled residual is signed (denoised − color
    // after fine controls); a UNORM texture would clamp the negative (darken-
    // noise) half of the correction away (Magpie DLSSNRFilter.cpp uses 16F).
    if (!CreateColorTexture(slot.controlledRes.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(slot.horizontalRes.GetAddressOf(), _width, internalH,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    // guidance 降采样目标(内部尺寸):densify 输出的源尺寸运动在缩放启用
    // 时按 Magpie DownsampleGuidance 收缩(置信度加权,运动向量乘
    // MotionScale 换算到内部像素单位)。
    if (!CreateColorTexture(slot.reducedMotion.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }
    if (!CreateColorTexture(slot.reducedConfidence.GetAddressOf(), internalW, internalH,
                            DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
        return false;
    }

    // (re)write scaling SRV/UAV descriptors(槽位 = enum HeapSlot)。
    HeapBinder heap{ _device.Get(), slot };
    _device->CreateShaderResourceView(slot.reducedColor.Get(), nullptr, heap.cpu(HeapSlot::SrvReducedColor));
    _device->CreateShaderResourceView(slot.reducedDenoised.Get(), nullptr, heap.cpu(HeapSlot::SrvReducedDenoised));
    _device->CreateShaderResourceView(slot.horizontalRes.Get(), nullptr, heap.cpu(HeapSlot::SrvHorizontal));
    _device->CreateUnorderedAccessView(slot.reducedColor.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavReducedColor));
    _device->CreateUnorderedAccessView(slot.reducedDenoised.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavReducedDenoised));
    _device->CreateUnorderedAccessView(slot.horizontalRes.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavHorizontal));
    _device->CreateShaderResourceView(slot.controlledRes.Get(), nullptr, heap.cpu(HeapSlot::SrvControlled));
    _device->CreateUnorderedAccessView(slot.controlledRes.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavControlled));
    _device->CreateShaderResourceView(slot.reducedMotion.Get(), nullptr, heap.cpu(HeapSlot::SrvReducedMotion));
    _device->CreateShaderResourceView(slot.reducedConfidence.Get(), nullptr, heap.cpu(HeapSlot::SrvReducedConfidence));
    _device->CreateUnorderedAccessView(slot.reducedMotion.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavReducedMotion));
    _device->CreateUnorderedAccessView(slot.reducedConfidence.Get(), nullptr, nullptr, heap.cpu(HeapSlot::UavReducedConfidence));
    return true;
}

void D3D12Context::ClearScalingForSlot(FrameSlot &slot) noexcept {
    slot.horizontalRes.Reset();
    slot.controlledRes.Reset();
    slot.reducedDenoised.Reset();
    slot.reducedColor.Reset();
    slot.reducedMotion.Reset();
    slot.reducedConfidence.Reset();
}

void D3D12Context::RecordPass(FrameSlot &slot, ID3D12PipelineState *pso, HeapSlot srv0, HeapSlot srv1,
                              HeapSlot uav,
                              UINT dispatchX, UINT dispatchY, const ResidualControls &rc) noexcept {
    // NGX's evaluate may rebind its own descriptor heap / root signature on
    // this command list; rebind ours before touching our descriptors.
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    cl->SetComputeRootSignature(_rsCompute.Get());
    cl->SetPipelineState(pso);
    HeapBinder gpu{ _device.Get(), *cl, slot };

    const UINT srcWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT dstWH[2]{ static_cast<UINT>(_internalWidth), static_cast<UINT>(_internalHeight) };
    cl->SetComputeRoot32BitConstants(0, 2, srcWH, 0);
    cl->SetComputeRoot32BitConstants(0, 2, dstWH, 2);
    const float zero = 0.0f;
    cl->SetComputeRoot32BitConstants(0, 1, &zero, 4);      // Padding0
    const float motion[2]{ 1.0f, 1.0f };
    cl->SetComputeRoot32BitConstants(0, 2, motion, 5);
    // SetComputeRoot32BitConstants with more than one DWORD needs a real array
    // (a scalar temporary's address won't do).
    const float controls[5]{ rc.multiplier, rc.saturation, rc.lightness,
                             rc.shadowStructure, rc.reflectionGlow };
    cl->SetComputeRoot32BitConstants(0, 5, controls, 7);

    cl->SetComputeRootDescriptorTable(1, gpu(srv0));
    cl->SetComputeRootDescriptorTable(2, gpu(srv1));
    cl->SetComputeRootDescriptorTable(3, gpu(uav));
    cl->Dispatch(dispatchX, dispatchY, 1);
}

void D3D12Context::RecordDownsampleVertical(FrameSlot &slot, const ResidualControls &rc) noexcept {
    // full -> (fullW x internalH) into the shared FP16 intermediate; upstream
    // reuses one texture for this and the residual horizontal pass, the port
    // reuses horizontalRes the same way.
    RecordPass(slot, _psoDownsampleVertical.Get(), SrvInput, SrvInput, UavHorizontal, // t0 input, t1 dummy, u0 horizontalRes
               (static_cast<UINT>(_width) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordDownsampleHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept {
    // t0/t1 read the vertical pass's output through its SRV descriptor
    // (slot 3) — slot 6 holds horizontalRes's UAV descriptor, and binding a
    // UAV descriptor into an SRV root table is invalid per the D3D12 spec
    // (the debug layer flags it; other drivers may not read it correctly).
    RecordPass(slot, _psoDownsampleHorizontal.Get(), SrvHorizontal, SrvHorizontal, UavReducedColor, // t0 intermediate, t1 dummy, u0 reducedColor
               (static_cast<UINT>(_internalWidth) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualPrepare(FrameSlot &slot, const ResidualControls &rc) noexcept {
    RecordPass(slot, _psoPrepare.Get(), SrvReducedColor, SrvReducedDenoised, UavControlled, // t0 reducedColor, t1 reducedDenoised, u0 controlledRes
               (static_cast<UINT>(_internalWidth) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept {
    RecordPass(slot, _psoHorizontal.Get(), SrvControlled, SrvControlled, UavHorizontal, // t0 controlledRes, t1 dummy, u0 horizontalRes
               (static_cast<UINT>(_width) + 7) / 8,
               (static_cast<UINT>(_internalHeight) + 7) / 8, rc);
}

void D3D12Context::RecordResidualVertical(FrameSlot &slot, const ResidualControls &rc,
                                          bool equalWidth) noexcept {
    // Equal internal width skips the horizontal pass (Magpie binds the
    // controlled residual straight into the vertical pass's t1).
    const HeapSlot residualSrv = equalWidth ? SrvControlled : SrvHorizontal;
    RecordPass(slot, _psoVertical.Get(), SrvInput, residualSrv, UavOutput, // t0 input, t1 residual, u0 output
               (static_cast<UINT>(_width) + 7) / 8,
               (static_cast<UINT>(_height) + 7) / 8, rc);
}

} // namespace vsdlssnr
