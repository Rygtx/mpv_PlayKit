#pragma once
// 光流后端公共接口(AMD 光流移植;与 NvofContext 并列的后端挂载点)。
// 两实现:NvofContext(NVIDIA NVOF 引擎,在役链路,时序模型见其头注释)与
// FxofContext(AMD FidelityFX OF,跨厂商)。接口面 = NvofContext 原公开面;
// 语义契约一致:
//   - StageFrame 帧序门:迟到/缺口帧一律播种(publishZero),会话内部
//     历史只接受按帧序的连续输入;
//   - waitFenceValue != 0 = 本帧真运动已产出(realMotion 判据);
//   - inputIndex >= 0 = 本帧输入槽位(迟到帧 -1,调用方据此补转换);
//   - LastStageMs = 门内等待 + 提交 + CPU 等待合计(nvof 段计时同语义)。
// PSO 与描述符槽位在 D3D12Context(BindOfResources/Record* 助手);context
// 类只做编排(门、栅栏、FFX API 调用)。
// (原第三实现 HalfResContext 兜底已于 2026-09-21 随 FFX 的 AMD 卡验证
//  通过而移除 —— 无存活场景;of_frame_gate.h 仍由 FxofContext 使用。)

#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "dlssnr_params.h" // kOfBackend* 常量(Kind() 返回值;params 为唯一权威)

namespace vsdlssnr {

class D3D12Context;

// StageFrame 回调(原 NvofContext 类内定义,上移共享):
//   postExecute:真运动产出后在会话 CL 上录制 densify/估算(门内、同线程);
//               inputIndex = 本帧写入的输入槽位(NVOF/FFX 忽略)。
//   postCopy:   拷贝/转换 CL 上的输入准备(RecordConvertInput 等)。
using OfPostExecuteFn = std::function<void(ID3D12GraphicsCommandList *, int inputIndex)>;
using OfPostCopyFn = std::function<void(ID3D12GraphicsCommandList *, int inputIndex)>;

struct OfStageResult {
    uint64_t waitFenceValue = 0; // 非 0 = 本帧真运动已产出(densify 已录)
    bool publishZero = false;    // 本帧清零发布 per-slot motion/confidence
    bool historyReset = false;   // 本帧对 NGX 置 PARAM_RESET
    int inputIndex = -1;         // 本帧写入的输入槽位(dump 用)
};

class IOpticalFlowBackend {
public:
    IOpticalFlowBackend() = default;
    virtual ~IOpticalFlowBackend() = default;
    IOpticalFlowBackend(const IOpticalFlowBackend &) = delete;
    IOpticalFlowBackend &operator=(const IOpticalFlowBackend &) = delete;

    // 创建/重建会话。调用方必须持 PoolHold(槽池封死,门必然空闲)。
    virtual bool Initialize(D3D12Context &d3d12, int width, int height,
                            int quality, char *err, size_t errLen) noexcept = 0;
    virtual void Finalize() noexcept = 0;
    virtual bool Enabled() const noexcept = 0;
    virtual int Quality() const noexcept = 0;
    virtual int Width() const noexcept = 0;
    virtual int Height() const noexcept = 0;
    // 历史失效(seek = 新时间线):下一帧重新播种(清零发布,不产出)。
    virtual void ResetHistory() noexcept = 0;
    // 在途拷贝排空(early-return 路径释放槽位前;防宿主复用 upload 撕裂)。
    virtual void WaitCopyIdle() noexcept = 0;
    virtual double LastStageMs() const noexcept = 0;
    virtual OfStageResult StageFrame(int frameIndex, ID3D12Resource *srcTex,
                                     const OfPostExecuteFn &postExecute,
                                     const OfPostCopyFn &postCopy,
                                     bool inputWrittenByPostCopy) noexcept = 0;
    // 诊断 dump 探针:本帧写入的输入纹理(index 0/1)。
    virtual ID3D12Resource *InputTexture(int index) const noexcept = 0;
    // SK_OF_MODE 能力串(backend 特有段;off/zero 前缀由 DlssnrContext 统一)。
    virtual const char *ModeString(char *buf, size_t len) noexcept = 0;
    // 后端种类(kOfBackendNvof/Ffx;densify lambda 分支与 NVOF 专属
    // 探针判定用 —— 免 RTTI/dynamic_cast)。
    virtual int Kind() const noexcept = 0;
};

} // namespace vsdlssnr
