// TS-RV 探针(2026-10-02 复核):upload/readback heap 上建 typed buffer SRV
// 是否触发异步 DEVICE_HUNG(d3d12_context.h YUV 管线绕道约束的原始依据,
// 2026-09-07 单机单次二分定位:"创建成功、下一次驱动调用报 device removed")。
//
// 阶段1(默认 50 轮):UPLOAD buffer → typed SRV(R8_UNORM)→ 后续驱动调用
//   (GetDeviceRemovedReason / CreateCommittedResource)→ 逐轮查 removed。
// 阶段2(阶段1干净才跑,默认 20 轮):GPU 真消费 —— D3DCompile 运行时编译
//   微型 CS 读 upload 上的 SRV 写 UAV → dispatch → 回读。
//
// 独立进程:炸了只死自己(TDR 影响整机 GPU 数秒,屏幕会闪黑)。判定:
//   任一轮 device removed → CONFIRMED;全部干净 → NOT REPRODUCED。
//
// 编译:vcvars64 && cl /EHsc /O2 /DUNICODE /Fe:tsrv_probe.exe tsrv_probe.cpp
//       /link d3d12.lib dxgi.lib d3dcompiler.lib
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

template <typename T>
static void Rel(T **p) {
    if (*p) { (*p)->Release(); *p = nullptr; }
}

static bool Removed(ID3D12Device *dev) {
    return dev->GetDeviceRemovedReason() != S_OK;
}

int main(int argc, char **argv) {
    const int loops1 = argc > 1 ? atoi(argv[1]) : 50;
    const int loops2 = argc > 2 ? atoi(argv[2]) : 20;
    IDXGIFactory4 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *dev = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // 对齐原始观测环境(2026-09-07 fe56401:debug layer 开启)。
    ID3D12Debug *dbg = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    Rel(&dbg);
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapters1(0, &adapter)) ||
        FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) ||
        FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        printf("TS-RV: device init FAILED\n");
        return 1;
    }
    const char *verdict = "NOT REPRODUCED";
    // ---- 阶段1:创建 + 后续调用(与原始观测同形态)----
    for (int i = 0; i < loops1; ++i) {
        D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 1u << 20;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *buf = nullptr;
        if (FAILED(dev->CreateCommittedResource(
                &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&buf)))) {
            printf("TS-RV: s1[%d] create FAILED\n", i);
            break;
        }
        D3D12_DESCRIPTOR_HEAP_DESC dh{};
        dh.NumDescriptors = 4;
        dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ID3D12DescriptorHeap *heap = nullptr;
        dev->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&heap));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8_UNORM; // typed buffer SRV(涉事形态)
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = (UINT)bd.Width;
        dev->CreateShaderResourceView(buf, &srv,
                                      heap->GetCPUDescriptorHandleForHeapStart());
        bool hit = Removed(dev); // "下一次驱动调用"
        ID3D12Resource *extra = nullptr;
        dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                     IID_PPV_ARGS(&extra));
        hit = hit || Removed(dev);
        Rel(&heap); Rel(&buf); Rel(&extra);
        if (hit) {
            printf("TS-RV: s1[%d] DEVICE-REMOVED -> CONFIRMED\n", i);
            verdict = "CONFIRMED";
            break;
        }
        if (i % 10 == 9) printf("TS-RV: s1 %d/%d ok\n", i + 1, loops1);
    }
    // ---- 阶段2:GPU 真消费 ----
    if (verdict[0] == 'N') {
        static const char *cs =
            "StructuredBuffer<uint> src : register(t0);\n"
            "RWStructuredBuffer<uint> dst : register(u0);\n"
            "[numthreads(1,1,1)] void main(uint ti : SV_DispatchThreadID) {\n"
            "    dst[ti] = src[ti];\n}\n";
        ID3DBlob *code = nullptr, *errs = nullptr;
        if (FAILED(D3DCompile(cs, strlen(cs), nullptr, nullptr, nullptr, "main",
                              "cs_5_0", 0, 0, &code, &errs))) {
            printf("TS-RV: s2 D3DCompile FAILED, stage2 skipped: %.200s\n",
                   errs ? (const char *)errs->GetBufferPointer() : "?");
            Rel(&errs);
        } else {
            // root sig:两个 descriptor table(SRV t0 / UAV u0)
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[0].NumDescriptors = 1;
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[1].NumDescriptors = 1;
            D3D12_ROOT_PARAMETER rp[2]{};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[0].DescriptorTable.NumDescriptorRanges = 1;
            rp[0].DescriptorTable.pDescriptorRanges = &ranges[0];
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable.NumDescriptorRanges = 1;
            rp[1].DescriptorTable.pDescriptorRanges = &ranges[1];
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 2;
            rsd.pParameters = rp;
            ID3DBlob *rsBlob = nullptr;
            ID3D12RootSignature *root = nullptr;
            ID3D12PipelineState *pso = nullptr;
            if (FAILED(D3D12SerializeRootSignature(
                    &rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, nullptr)) ||
                FAILED(dev->CreateRootSignature(
                    0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                    IID_PPV_ARGS(&root)))) {
                printf("TS-RV: s2 root sig FAILED, stage2 skipped\n");
            } else {
                D3D12_COMPUTE_PIPELINE_STATE_DESC psd{};
                psd.pRootSignature = root;
                psd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
                if (FAILED(dev->CreateComputePipelineState(&psd, IID_PPV_ARGS(&pso)))) {
                    printf("TS-RV: s2 PSO FAILED, stage2 skipped\n");
                }
            }
            Rel(&rsBlob);
            if (pso) {
                for (int i = 0; i < loops2; ++i) {
                    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
                    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
                    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
                    D3D12_RESOURCE_DESC bd{};
                    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    bd.Width = 4096; bd.Height = 1; bd.DepthOrArraySize = 1;
                    bd.MipLevels = 1; bd.SampleDesc.Count = 1;
                    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    ID3D12Resource *upload = nullptr, *result = nullptr, *back = nullptr;
                    // 资源创建 HRESULT 全查(2026-10-05 评审修):失败轮打印并
                    // 跳过该轮 —— null 描述符 dispatch 会以假 CONFIRMED 污染判定。
                    bool created =
                        SUCCEEDED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&upload)));
                    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    created = created &&
                        SUCCEEDED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 nullptr, IID_PPV_ARGS(&result)));
                    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
                    created = created &&
                        SUCCEEDED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&back)));
                    D3D12_DESCRIPTOR_HEAP_DESC dh{};
                    dh.NumDescriptors = 4;
                    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                    dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                    ID3D12DescriptorHeap *heap = nullptr;
                    created = created &&
                        SUCCEEDED(dev->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&heap)));
                    ID3D12CommandAllocator *alloc = nullptr;
                    ID3D12GraphicsCommandList *cl = nullptr;
                    created = created &&
                        SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                              IID_PPV_ARGS(&alloc)));
                    created = created &&
                        SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                                         nullptr, IID_PPV_ARGS(&cl)));
                    bool hit = false;
                    if (created) {
                        const D3D12_CPU_DESCRIPTOR_HANDLE h0 =
                            heap->GetCPUDescriptorHandleForHeapStart();
                        const D3D12_CPU_DESCRIPTOR_HANDLE h1{
                            h0.ptr + (SIZE_T)dev->GetDescriptorHandleIncrementSize(
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
                        // SRV 落在 UPLOAD buffer 上(涉事形态);UAV 在 DEFAULT。
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                        srv.Format = DXGI_FORMAT_R32_UINT;
                        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        srv.Shader4ComponentMapping =
                            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv.Buffer.NumElements = 1024;
                        dev->CreateShaderResourceView(upload, &srv, h0);
                        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                        uav.Format = DXGI_FORMAT_R32_UINT;
                        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                        uav.Buffer.NumElements = 1024;
                        dev->CreateUnorderedAccessView(result, nullptr, &uav, h1);
                        const D3D12_GPU_DESCRIPTOR_HANDLE g0 =
                            heap->GetGPUDescriptorHandleForHeapStart();
                        const D3D12_GPU_DESCRIPTOR_HANDLE g1{
                            g0.ptr + (UINT64)dev->GetDescriptorHandleIncrementSize(
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};

                        ID3D12DescriptorHeap *heaps[]{heap};
                        cl->SetDescriptorHeaps(1, heaps);
                        cl->SetComputeRootSignature(root);
                        cl->SetComputeRootDescriptorTable(0, g0);
                        cl->SetComputeRootDescriptorTable(1, g1);
                        cl->Dispatch(1, 1, 1);
                        cl->Close();
                        ID3D12CommandList *lists[]{cl};
                        queue->ExecuteCommandLists(1, lists);
                        queue->Signal(fence, (UINT64)i + 1);
                        fence->SetEventOnCompletion((UINT64)i + 1, fenceEvent);
                        // 等票返回值检查(2026-10-05 评审修):非 WAIT_OBJECT_0 =
                        // 该轮未完成,打印并跳过、不计入判定(总体 CONFIRMED /
                        // NOT REPRODUCED 语义不变;removed 查询只对完成轮做)。
                        if (WaitForSingleObject(fenceEvent, 5000) != WAIT_OBJECT_0) {
                            printf("TS-RV: s2[%d] fence wait TIMEOUT, round skipped\n", i);
                        } else {
                            hit = Removed(dev);
                        }
                    } else {
                        printf("TS-RV: s2[%d] resource create FAILED, round skipped\n", i);
                    }
                    Rel(&cl); Rel(&alloc); Rel(&heap);
                    Rel(&back); Rel(&result); Rel(&upload);
                    if (hit) {
                        printf("TS-RV: s2[%d] DEVICE-REMOVED -> CONFIRMED\n", i);
                        verdict = "CONFIRMED";
                        break;
                    }
                    if (i % 5 == 4) printf("TS-RV: s2 %d/%d ok\n", i + 1, loops2);
                }
            }
            Rel(&pso); Rel(&root);
        }
        Rel(&code); Rel(&errs);
    }
    printf("TS-RV: VERDICT %s (s1=%d s2=%d)\n", verdict, loops1, loops2);
    Rel(&fence); Rel(&queue); Rel(&dev); Rel(&adapter); Rel(&factory);
    if (fenceEvent) CloseHandle(fenceEvent);
    return verdict[0] == 'C' ? 2 : 0;
}
