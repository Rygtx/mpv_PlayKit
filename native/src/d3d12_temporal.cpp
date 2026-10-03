#include "d3d12_context.h"
#include "d3d12_internal.h"
#include "d3d12_shaders.h"

#include <algorithm>
#include <cstdio>

// 抗闪烁时域稳定器与调试视图子系统(RebuildTemporal/RecordTemporal/DebugDiff/FlowView)—— 方法体逐字迁移。

namespace vsdlssnr {

void TimingStatusLine(const char *line) noexcept; // 本体在 dlssnr_context(免重 include)
bool ProbeEnabled() noexcept;                    // 同上(VSDLSSNR_PROBE)

bool D3D12Context::CreateTemporalObjects(char *err, size_t errLen) noexcept {
    // 抗闪烁时域稳定器(静态采样器;main/reduce 两 PSO 无条件常驻)。
    // 抗闪烁时域稳定器:b0 14 常量(Size2/UseMotion/Route/Weight/
    // MotionExtent2/LowSize2/Pad + Region4)+ t0-t7 八张独立 SRV 表 +
    // u0/u1/u2 三张独立 UAV 表 + 线性静态采样器。主 shader 一份覆盖
    // Route 1-4,mode4 另有半分辨率 reduce —— 两个 PSO 无条件常驻(PSO
    // 生命周期跟着"使用条件"而不是"首次搭车路径",#43-①)。
    {
        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        if (!CreateComputeRs(_device.Get(), 14, 8, 3, _rsTemporal.GetAddressOf(),
                             "temporal", &samp, err, errLen)) {
            return false;
        }
        const struct {
            const char *hlsl;
            const char *entry;
            ComPtr<ID3D12PipelineState> *pso;
        } temporalShaders[] = {
            { TEMPORAL_MAIN_HLSL, "main", &_psoTemporalMain },
            { TEMPORAL_REDUCE_HLSL, "main", &_psoTemporalReduce },
        };
        for (const auto &ts : temporalShaders) {
            if (!CreateComputePsoFor(_device.Get(), ts.hlsl, ts.entry, _rsTemporal.Get(),
                                     ts.pso->GetAddressOf(), "temporal", err, errLen)) {
                return false;
            }
        }
    }
    return true;
}


bool D3D12Context::CreateDebugObjects(char *err, size_t errLen) noexcept {
    // debug diff / 光流场调试视图(共享 _debugDiff 中转)。
    // 差异调试视图:b0 4 常量(extent 2+amp 1+pad 1)+ t0/t1(input/output)
    // + u0(共享 _debugDiff)。
    if (!CreateComputeRs(_device.Get(), 4, 2, 1, _rsDebugDiff.GetAddressOf(),
                         "debug diff", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), DEBUG_DIFF_HLSL, "DebugDiffMain",
                             _rsDebugDiff.Get(), _psoDebugDiff.GetAddressOf(),
                             "debug diff", err, errLen)) {
        return false;
    }
    // 光流场调试视图:b0 6 常量(extent 2+outExtent 2+scale 1+pad 1)+ t0
    // (运动场,SrvMotion/SrvReducedMotion 按取材二选一)+ u0(_debugDiff 中转)。
    if (!CreateComputeRs(_device.Get(), 6, 1, 1, _rsFlowView.GetAddressOf(),
                         "flow view", nullptr, err, errLen) ||
        !CreateComputePsoFor(_device.Get(), FLOW_VIEW_HLSL, "FlowViewMain",
                             _rsFlowView.Get(), _psoFlowView.GetAddressOf(),
                             "flow view", err, errLen)) {
        return false;
    }
    return true;
}


bool D3D12Context::RebuildTemporal(int mode, char *err, size_t errLen) noexcept {
    // 替换 context 级历史纹理与每槽 temporalOut;调用方必须持 PoolHold
    // (同 RebuildScaling:所有槽空闲、池封死,无在飞帧引用)。mode 与
    // 当前一致时 no-op(调用方的逐帧同步据此免锁快路径)。
    mode = std::clamp(mode, 0, 4);
    if (mode == _temporalMode) return true;

    // 全量释放(降档/换档/换尺寸统一走这里;纹理 Release 即可,视图随后
    // 覆盖为占位 —— 绝不遗留悬空视图)。
    for (auto &t : _tempHist) t.Reset();
    for (auto &t : _tempGuide) t.Reset();
    for (auto &t : _tempLow) t.Reset();
    for (int i = 0; i < kSlotCount; ++i) _slots[i].temporalOut.Reset();
    _temporalMode = 0;
    if (mode == 0) {
        // 视图回占位(inputColor/outputColor —— placeholder 惯例,绝不 NULL
        // 描述符)。布局 = enum HeapSlot 时域区。
        for (int i = 0; i < kSlotCount; ++i) {
            HeapBinder h{ _device.Get(), _slots[i] };
            for (HeapSlot d : {HeapSlot::SrvTemporalOut, HeapSlot::Hist0Srv, HeapSlot::Hist1Srv,
                               HeapSlot::Guide0Srv, HeapSlot::Guide1Srv,
                               HeapSlot::Low0Srv, HeapSlot::Low1Srv}) {
                _device->CreateShaderResourceView(_slots[i].inputColor.Get(), nullptr, h.cpu(d));
            }
            for (HeapSlot d : {HeapSlot::UavTemporalOut, HeapSlot::Hist0Uav, HeapSlot::Hist1Uav,
                               HeapSlot::Guide0Uav, HeapSlot::Guide1Uav,
                               HeapSlot::Low0Uav, HeapSlot::Low1Uav}) {
                _device->CreateUnorderedAccessView(_slots[i].outputColor.Get(), nullptr, nullptr, h.cpu(d));
            }
        }
        return true;
    }

    // 历史/引导 ping-pong(全尺寸 FP16,alpha = 有效性标记)+ mode4 半分辨率
    // low。初始内容不定义是安全的:首个 temporal dispatch 的 weight=0
    // (状态无效)不读历史,NextHistory/NextGuide/low 全量覆写。
    for (int i = 0; i < 2; ++i) {
        if (!CreateColorTexture(_tempHist[i].GetAddressOf(), _width, _height,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen) ||
            !CreateColorTexture(_tempGuide[i].GetAddressOf(), _width, _height,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
            return false;
        }
    }
    if (mode == 4) {
        for (int i = 0; i < 2; ++i) {
            if (!CreateColorTexture(_tempLow[i].GetAddressOf(), (_width + 1) >> 1, (_height + 1) >> 1,
                                    DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
                return false;
            }
        }
    }
    // 每槽 temporalOut(管线色同格式 UAV,COMMON 起步;纹理先于视图 ——
    // NULL 描述符异步 TDR 防线,#50 同款顺序铁律)。格式随管线色:下游
    // (VSR/TrueHDR/转换)消费的就是"outputColor 会给出的东西"。
    for (int i = 0; i < kSlotCount; ++i) {
        if (!CreateColorTexture(_slots[i].temporalOut.GetAddressOf(), _width, _height,
                                _inColorFmt, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, err, errLen)) {
            return false;
        }
    }
    // 视图写入(context 级纹理 × 每槽堆;占位全覆盖)。
    for (int i = 0; i < kSlotCount; ++i) {
        HeapBinder h{ _device.Get(), _slots[i] };
        _device->CreateShaderResourceView(_tempHist[0].Get(), nullptr, h.cpu(HeapSlot::Hist0Srv));
        _device->CreateShaderResourceView(_tempHist[1].Get(), nullptr, h.cpu(HeapSlot::Hist1Srv));
        _device->CreateShaderResourceView(_tempGuide[0].Get(), nullptr, h.cpu(HeapSlot::Guide0Srv));
        _device->CreateShaderResourceView(_tempGuide[1].Get(), nullptr, h.cpu(HeapSlot::Guide1Srv));
        _device->CreateUnorderedAccessView(_tempHist[0].Get(), nullptr, nullptr, h.cpu(HeapSlot::Hist0Uav));
        _device->CreateUnorderedAccessView(_tempHist[1].Get(), nullptr, nullptr, h.cpu(HeapSlot::Hist1Uav));
        _device->CreateUnorderedAccessView(_tempGuide[0].Get(), nullptr, nullptr, h.cpu(HeapSlot::Guide0Uav));
        _device->CreateUnorderedAccessView(_tempGuide[1].Get(), nullptr, nullptr, h.cpu(HeapSlot::Guide1Uav));
        _device->CreateUnorderedAccessView(_slots[i].temporalOut.Get(), nullptr, nullptr, h.cpu(HeapSlot::UavTemporalOut));
        _device->CreateShaderResourceView(_slots[i].temporalOut.Get(), nullptr, h.cpu(HeapSlot::SrvTemporalOut));
        // mode 1-3 的 low 视图必须回占位(4→2/3 降档时旧视图悬空指向已
        // 释放纹理;Route!=4 不读 t6/t7 但绝不留悬空描述符);
        // mode 4 绑真纹理。mode<4 时 _tempLow 为空 —— 不可 CreateSRV(NULL)。
        if (mode == 4) {
            _device->CreateShaderResourceView(_tempLow[0].Get(), nullptr, h.cpu(HeapSlot::Low0Srv));
            _device->CreateShaderResourceView(_tempLow[1].Get(), nullptr, h.cpu(HeapSlot::Low1Srv));
            _device->CreateUnorderedAccessView(_tempLow[0].Get(), nullptr, nullptr, h.cpu(HeapSlot::Low0Uav));
            _device->CreateUnorderedAccessView(_tempLow[1].Get(), nullptr, nullptr, h.cpu(HeapSlot::Low1Uav));
        } else {
            _device->CreateShaderResourceView(_slots[i].inputColor.Get(), nullptr, h.cpu(HeapSlot::Low0Srv));
            _device->CreateShaderResourceView(_slots[i].inputColor.Get(), nullptr, h.cpu(HeapSlot::Low1Srv));
            _device->CreateUnorderedAccessView(_slots[i].outputColor.Get(), nullptr, nullptr, h.cpu(HeapSlot::Low0Uav));
            _device->CreateUnorderedAccessView(_slots[i].outputColor.Get(), nullptr, nullptr, h.cpu(HeapSlot::Low1Uav));
        }
    }
    _temporalMode = mode;
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "DLSSNR STATUS: temporal rebuilt route=%d history=%dx%d", mode, _width, _height);
    TimingStatusLine(msg);
    return true;
}

void D3D12Context::RecordTemporal(FrameSlot &slot, int mode, int next, bool useMotion,
                                  UINT motionSrvIndex, UINT motionW, UINT motionH,
                                  float weight) noexcept {
    // 上游 DLSSNRTemporal::Draw 的移植:mode4 先 reduce(半分辨率残差+引导)
    // 后主 pass。t0/t1 都绑 inputColor(单 pass 链 base=输入,上游 count==1
    // 时 input/base 同纹理);t2 = outputColor(NR 链原始输出,调用点保证
    // UAV→NSR 契约由本函数屏障自管)。useMotion=false 时 t5 照绑(占位),
    // shader 侧 UseMotion=0 全路径守卫绝不读取。
    // 状态时间线(与现架构对齐,调用点 = base CL 的 eval 段末、evalLock 域内):
    //   入:outputColor UAV(eval/残差帧末不变量)、inputColor COMMON、
    //      hist/guide[prev] COMMON→NSR、hist/guide[next]+low COMMON→UAV;
    //   出:temporalOut **COMMON**(显式落位,下游 VSR/FG/转换的 from-state
    //      全按 COMMON 对接 —— 不依赖 NGX 衰减语义)、outputColor/inputColor
    //      归 COMMON(下游不再消费)、hist/guide/low 归 COMMON(UAV 跨 ECL
    //      不自动衰减,显式归位保帧末全资源 COMMON 不变量)。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    const int previous = next ^ 1;

    D3D12_RESOURCE_BARRIER pre[9];
    UINT preCount = 0;
    pre[preCount++] = Transition(slot.outputColor.Get(),
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[preCount++] = Transition(slot.inputColor.Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[preCount++] = Transition(slot.temporalOut.Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[preCount++] = Transition(_tempHist[next].Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[preCount++] = Transition(_tempGuide[next].Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[preCount++] = Transition(_tempHist[previous].Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[preCount++] = Transition(_tempGuide[previous].Get(),
                                 D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool lowNeeded = mode == 4;
    if (lowNeeded) {
        pre[preCount++] = Transition(_tempLow[0].Get(),
                                     D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pre[preCount++] = Transition(_tempLow[1].Get(),
                                     D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    cl->ResourceBarrier(preCount, pre);

    cl->SetComputeRootSignature(_rsTemporal.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    // cbuffer 布局(与 TEMPORAL_*_HLSL 同步,三处同步铁律):
    // Size@0 UseMotion@2 Route@3 Weight@4 MotionExtent@5 LowSize@7 Pad@9
    // Region@10(4)。
    const UINT sizeWH[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const UINT motionWH[2]{ motionW, motionH };
    const UINT lowWH[2]{ static_cast<UINT>((_width + 1) >> 1), static_cast<UINT>((_height + 1) >> 1) };
    const UINT route = static_cast<UINT>(mode);
    const UINT useM = useMotion ? 1u : 0u;
    const UINT pad0 = 0u;
    const UINT region[4]{ 0u, 0u, sizeWH[0], sizeWH[1] };
    cl->SetComputeRoot32BitConstants(0, 2, sizeWH, 0);
    cl->SetComputeRoot32BitConstants(0, 1, &useM, 2);
    cl->SetComputeRoot32BitConstants(0, 1, &route, 3);
    cl->SetComputeRoot32BitConstants(0, 1, &weight, 4);
    cl->SetComputeRoot32BitConstants(0, 2, motionWH, 5);
    cl->SetComputeRoot32BitConstants(0, 2, lowWH, 7);
    cl->SetComputeRoot32BitConstants(0, 1, &pad0, 9);
    cl->SetComputeRoot32BitConstants(0, 4, region, 10);

    if (lowNeeded) {
        // reduce:t0 Input / t1 Base / t2 Raw(读),u0 Residual / u1 Guide(写)。
        cl->SetPipelineState(_psoTemporalReduce.Get());
        cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));
        cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::SrvInput));
        cl->SetComputeRootDescriptorTable(3, gpu(kSrvOutputColor));
        cl->SetComputeRootDescriptorTable(9, gpu(HeapSlot::Low0Uav));
        cl->SetComputeRootDescriptorTable(10, gpu(HeapSlot::Low1Uav));
        cl->Dispatch((lowWH[0] + 7) / 8, (lowWH[1] + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER lowReady[2]{
            Transition(_tempLow[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(_tempLow[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cl->ResourceBarrier(2, lowReady);
    }

    // 主 pass:u0 Output(temporalOut)/ u1 NextHistory / u2 NextGuide。
    cl->SetPipelineState(_psoTemporalMain.Get());
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));                     // t0 Input
    cl->SetComputeRootDescriptorTable(2, gpu(HeapSlot::SrvInput));                     // t1 Base(=Input)
    cl->SetComputeRootDescriptorTable(3, gpu(kSrvOutputColor));       // t2 Raw
    cl->SetComputeRootDescriptorTable(4, gpu(kHistSrv[previous]));     // t3 History
    cl->SetComputeRootDescriptorTable(5, gpu(kGuideSrv[previous]));     // t4 PreviousGuide
    cl->SetComputeRootDescriptorTable(6, gpu(motionSrvIndex));        // t5 Motion
    cl->SetComputeRootDescriptorTable(7, lowNeeded ? gpu(HeapSlot::Low0Srv) : gpu(kHistSrv[previous])); // t6
    cl->SetComputeRootDescriptorTable(8, lowNeeded ? gpu(HeapSlot::Low1Srv) : gpu(kGuideSrv[previous])); // t7
    cl->SetComputeRootDescriptorTable(9, gpu(kUavTemporalOut));       // u0 Output
    cl->SetComputeRootDescriptorTable(10, gpu(kHistUav[next]));        // u1 NextHistory
    cl->SetComputeRootDescriptorTable(11, gpu(kGuideUav[next]));        // u2 NextGuide
    cl->Dispatch((sizeWH[0] + 7) / 8, (sizeWH[1] + 7) / 8, 1);

    // 归位:input/output/历史/引导/low/temporalOut 全部回 COMMON(本 pass
    // 是 outputColor/inputColor 的最后消费者;temporalOut 显式 COMMON 落位,
    // 下游 base 尾 NSR 化 / fgBar / post 转换按 COMMON 契约对接);motion 留
    // NSR(recordGuidancePark 按既有契约归位)。
    D3D12_RESOURCE_BARRIER post[9];
    UINT postCount = 0;
    post[postCount++] = Transition(slot.outputColor.Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(slot.inputColor.Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(slot.temporalOut.Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(_tempHist[next].Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(_tempGuide[next].Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(_tempHist[previous].Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
    post[postCount++] = Transition(_tempGuide[previous].Get(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
    if (lowNeeded) {
        post[postCount++] = Transition(_tempLow[0].Get(),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_COMMON);
        post[postCount++] = Transition(_tempLow[1].Get(),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_COMMON);
    }
    cl->ResourceBarrier(postCount, post);
}

void D3D12Context::RecordDebugDiff(FrameSlot &slot) noexcept {
    // 差异调试视图(契约见头文件):input COMMON / output UAV 入,输出
    // 保持 UAV、input 归 COMMON 出。dispatch 写共享 _debugDiff,同 CL 内
    // COPY_SOURCE 化后拷回 outputColor —— RGBA8 无 UAV load,读写同纹理
    // 非法,中转纹理是必须的。每槽堆的槽 34 视图都指向共享资源,dispatch
    // + 拷回在同一 CL 上原子成对,队列提交序串行,并发帧不交错。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    cl->SetComputeRootSignature(_rsDebugDiff.Get());
    cl->SetPipelineState(_psoDebugDiff.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    // cbuffer 布局(与 DEBUG_DIFF_HLSL 同步,三处同步铁律):
    // extent@0 amp@2 pad@3。
    const UINT extent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const float amp = 20.0f;
    const UINT pad = 0u;
    cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl->SetComputeRoot32BitConstants(0, 1, &amp, 2);
    cl->SetComputeRoot32BitConstants(0, 1, &pad, 3);

    D3D12_RESOURCE_BARRIER pre[3]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cl->ResourceBarrier(3, pre);
    cl->SetComputeRootDescriptorTable(1, gpu(HeapSlot::SrvInput));               // t0 inputColor
    cl->SetComputeRootDescriptorTable(2, gpu(kSrvOutputColor)); // t1 outputColor
    cl->SetComputeRootDescriptorTable(3, gpu(kUavDebugDiff));   // u0 debugDiff
    cl->Dispatch((static_cast<UINT>(_width) + 7) / 8,
                 (static_cast<UINT>(_height) + 7) / 8, 1);
    // input 用完即归位;debugDiff → COPY_SOURCE、output → COPY_DEST 接拷贝
    // (状态转换对先前 UAV 写入自带同步语义,无需额外 UAV barrier)。
    D3D12_RESOURCE_BARRIER mid[3]{
        Transition(slot.inputColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cl->ResourceBarrier(3, mid);
    cl->CopyResource(slot.outputColor.Get(), _debugDiff.Get());
    D3D12_RESOURCE_BARRIER post[2]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(2, post);
}

void D3D12Context::RecordFlowView(FrameSlot &slot, bool useReduced, bool realMotion,
                                  D3D12_RESOURCE_STATES outColorIn) noexcept {
    // 光流场调试视图(契约见头文件)。dispatch 恒按源尺寸(OutExtent),
    // 运动场 texel 在 shader 内最近邻映射 —— follow 内部场(半尺寸)也铺
    // 满输出,_debugDiff 无陈旧边缘。dispatch 写共享 _debugDiff → COPY_
    // SOURCE → 拷回 outputColor(与 RecordDebugDiff 同一条槽 CL 上原子成对;
    // 本视图不读 outputColor,免去 UAV→NSR 往返,直接 UAV→COPY_DEST→UAV)。
    ID3D12GraphicsCommandList *cl = slot.commandList.Get();
    // t0 取材:真运动帧按管线取 slot 场(已 NSR);无真运动帧绑静态零
    // 纹理(槽 35,常驻 NSR)—— 不碰 slot.motion(首 densify 前内容未定义)。
    const HeapSlot srvIndex = !realMotion ? SrvZeroMotion : (useReduced ? SrvReducedMotion : SrvMotion);
    cl->SetComputeRootSignature(_rsFlowView.Get());
    cl->SetPipelineState(_psoFlowView.Get());
    HeapBinder gpu{ _device.Get(), *cl, slot };

    // cbuffer 布局(与 FLOW_VIEW_HLSL 同步,三处同步铁律):
    // extent@0 outExtent@2 scale@4 pad@5。
    const UINT extent[2]{ realMotion && useReduced ? static_cast<UINT>(_internalWidth) : static_cast<UINT>(_width),
                          realMotion && useReduced ? static_cast<UINT>(_internalHeight) : static_cast<UINT>(_height) };
    const UINT outExtent[2]{ static_cast<UINT>(_width), static_cast<UINT>(_height) };
    const float scale = 0.125f; // 8px/帧满亮
    const UINT pad = 0u;
    cl->SetComputeRoot32BitConstants(0, 2, extent, 0);
    cl->SetComputeRoot32BitConstants(0, 2, outExtent, 2);
    cl->SetComputeRoot32BitConstants(0, 1, &scale, 4);
    cl->SetComputeRoot32BitConstants(0, 1, &pad, 5);

    // 入态:debugDiff→UAV / output→COPY_DEST(outColorIn:NR 开帧 eval 留
    // UAV,NR 关帧跨 ECL 衰减 COMMON —— 运动场三路取材全部 NSR 常驻或由
    // evaluate 消费链保持,无需屏障)。
    D3D12_RESOURCE_BARRIER pre[2]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(slot.outputColor.Get(), outColorIn, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cl->ResourceBarrier(2, pre);
    cl->SetComputeRootDescriptorTable(1, gpu(srvIndex));        // t0 运动场/零纹理
    cl->SetComputeRootDescriptorTable(2, gpu(kUavDebugDiff));   // u0 debugDiff
    cl->Dispatch((static_cast<UINT>(_width) + 7) / 8,
                 (static_cast<UINT>(_height) + 7) / 8, 1);
    // 出态:debugDiff → COPY_SOURCE 接拷贝 → 归位;真运动帧的 slot 场
    // NSR 入、NSR 出,归位仍归 recordGuidancePark(本函数不动其状态)。
    D3D12_RESOURCE_BARRIER mid[1]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cl->ResourceBarrier(1, mid);
    cl->CopyResource(slot.outputColor.Get(), _debugDiff.Get());
    D3D12_RESOURCE_BARRIER post[2]{
        Transition(_debugDiff.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(slot.outputColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cl->ResourceBarrier(2, post);
}

} // namespace vsdlssnr
