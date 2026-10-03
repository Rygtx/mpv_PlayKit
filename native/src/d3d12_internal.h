#pragma once
// d3d12 子系统 TU 共享内部助手(2026-10-04 拆出):根签名/PSO 工厂与槽堆
// 句柄绑定(多 TU 真共用件)。单消费者助手不下放此处:YuvCoeffsFor 归
// d3d12_convert.cpp 本地、CreateOfPso 归 d3d12_of.cpp 本地(2026-10-05)。
#include "d3d12_context.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

namespace vsdlssnr {

// 槽描述符堆的句柄计算(全仓唯一实现;槽位一律传 enum HeapSlot):录制侧
// 构造即 SetDescriptorHeaps + 取 GPU 基址,创建侧用无 CL 构造仅取基址。
// 原"5 行 SetDescriptorHeaps/gpuBase/inc/gpu lambda"前奏 ×14 与创建侧
// slotHandle lambda ×5 均由此收敛。
struct HeapBinder {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase{};
    UINT inc = 1;
    // 录制侧:绑定堆(命令列表每 Reset 后必须重绑)并取双基址。
    HeapBinder(ID3D12Device *dev, ID3D12GraphicsCommandList &cl, FrameSlot &slot) {
        ID3D12DescriptorHeap *heaps[]{ slot.srvUavHeap.Get() };
        cl.SetDescriptorHeaps(1, heaps);
        cpuBase = slot.srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    // 创建侧:堆刚建好,只写描述符不录命令。
    HeapBinder(ID3D12Device *dev, FrameSlot &slot) {
        cpuBase = slot.srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        gpuBase = slot.srvUavHeap->GetGPUDescriptorHandleForHeapStart();
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i) const {
        return D3D12_CPU_DESCRIPTOR_HANDLE{ cpuBase.ptr + static_cast<SIZE_T>(i) * inc };
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i) const {
        return D3D12_GPU_DESCRIPTOR_HANDLE{ gpuBase.ptr + static_cast<UINT64>(i) * inc };
    }
    // 录制侧调用形态:句柄实例直接当函数用(替代原 gpu lambda)。
    D3D12_GPU_DESCRIPTOR_HANDLE operator()(UINT i) const { return gpu(i); }
};


// 通用 compute 根签名工厂:b0(numConsts)+ srvCount 张独立 SRV 表 +
// uavCount 张独立 UAV 表。参数序 = 常量、SRV、UAV —— 全仓录制点按此绑定
// 表索引(SetComputeRootDescriptorTable(1..N)),改序 = 全部 Record* 失配。
// 此前该形态在 CreateComputeObjects 内手写 8 份(每份 60-90 行),唯一真
// 差异只有个数;"同表重叠 range 非法"禁忌由工厂内独立表参数构造性规避。
// sampler 非空 = 追加静态采样器(时域稳定器专用)。
inline bool CreateComputeRs(ID3D12Device *device, UINT numConsts, UINT srvCount, UINT uavCount,
                     ID3D12RootSignature **rs, const char *label,
                     const D3D12_STATIC_SAMPLER_DESC *sampler,
                     char *err, size_t errLen) noexcept {
    // err 直写(本 helper 是自由函数,SetErr 是 D3D12Context 成员)。
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s: %s", label, what);
        return false;
    };
    D3D12_DESCRIPTOR_RANGE srvRanges[8]{};
    for (UINT i = 0; i < srvCount; ++i) {
        srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors = 1;
        srvRanges[i].BaseShaderRegister = i;
        srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_DESCRIPTOR_RANGE uavRanges[4]{};
    for (UINT i = 0; i < uavCount; ++i) {
        uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[i].NumDescriptors = 1;
        uavRanges[i].BaseShaderRegister = i;
        uavRanges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER params[13]{};
    UINT n = 0;
    params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[n].Constants.ShaderRegister = 0;
    params[n].Constants.Num32BitValues = numConsts;
    params[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ++n;
    for (UINT i = 0; i < srvCount; ++i, ++n) {
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[n].DescriptorTable.NumDescriptorRanges = 1;
        params[n].DescriptorTable.pDescriptorRanges = &srvRanges[i];
        params[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (UINT i = 0; i < uavCount; ++i, ++n) {
        params[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[n].DescriptorTable.NumDescriptorRanges = 1;
        params[n].DescriptorTable.pDescriptorRanges = &uavRanges[i];
        params[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = n;
    rsDesc.pParameters = params;
    if (sampler) {
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = sampler;
    }
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           rsBlob.GetAddressOf(), rsErr.GetAddressOf()))) {
        return fail("SerializeRootSignature failed");
    }
    if (FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                           rsBlob->GetBufferSize(), IID_PPV_ARGS(rs)))) {
        return fail("CreateRootSignature failed");
    }
    return true;
}

// 对既有根签名编译一个入口并建 PSO(与 CreateComputeRs 配对;多入口共用
// 一张根签名时循环本函数)。
inline bool CreateComputePsoFor(ID3D12Device *device, const char *hlsl, const char *entry,
                         ID3D12RootSignature *rs, ID3D12PipelineState **pso,
                         const char *label, char *err, size_t errLen) noexcept {
    auto fail = [&](const char *what) {
        if (err && errLen) std::snprintf(err, errLen, "%s: %s", label, what);
        return false;
    };
    ComPtr<ID3DBlob> code, csErr;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, "cs_5_0",
                          0, 0, code.GetAddressOf(), csErr.GetAddressOf()))) {
        return fail(csErr ? static_cast<const char *>(csErr->GetBufferPointer())
                          : "D3DCompile failed");
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rs;
    psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(pso)))) {
        return fail("CreateComputePipelineState failed");
    }
    return true;
}


} // namespace vsdlssnr
