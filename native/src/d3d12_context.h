#pragma once
// Owns the D3D12 environment for vs_dlssnr. The filter runs under VS
// fmParallel: several frames are in flight at once, each on its own FrameSlot
// (command list + staging + frame textures + descriptor heap). The single
// DIRECT queue serializes slot submissions in submit order, so per-slot
// textures never race on the GPU; CPU-side pack/unpack of concurrent frames
// overlaps the GPU work of other slots, which is what closes the per-frame
// GPU idle gaps of the old single-slot pipeline.
//
// NGX evaluate is additionally serialized by a mutex in DlssnrContext (the
// feature/parameter block is a singleton; on the GPU the queue order already
// serializes the dispatches).
//
// Motion/depth (zero guidance) are read-only and stay resident in
// NON_PIXEL_SHADER_RESOURCE for their whole lifetime — no per-frame state
// transitions, safe for concurrent consumer command lists.

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdint>

#include "dlssnr_params.h" // kFgMultMax(FG 插值槽数上界)

namespace vsdlssnr {

using Microsoft::WRL::ComPtr;

// 每源帧的最大插值帧数(倍数 M-1,M 封顶 4)—— FG 回读缓冲按此建组。
inline constexpr int kFgGenSlots = kFgMultMax - 1;

// Full-subresource transition barrier — the one barrier builder for the whole
// plugin (frame path, NGX context and diagnostics all used to hand-roll the
// same struct fill).
inline D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

// Residual fine-control set (Magpie 0.6.5 r2-fix1/2d37f8c0): applied once per
// internal-resolution pixel in PrepareResidual, before the Catmull-Rom passes.
struct ResidualControls {
    float multiplier = 1.0f;
    float saturation = 1.0f;
    float lightness = 1.0f;
    float shadowStructure = 1.0f;
    float reflectionGlow = 1.0f;
};

// YUV↔RGB 转换的色彩参数(props _Matrix/_ColorRange → root constants 的语义
// 输入;具体系数/范围常量在记录函数里按 _depth 推导)。矩阵只支持 709/601,
// HDR 传输函数不在本滤镜范围(SDR 链)。
enum class ColorMatrix : int { BT709 = 0, BT601 = 1 };
enum class ColorRange : int { Full = 0, Limited = 1 };

// Per-frame-in-flight resources; acquired from the slot pool for the duration
// of one getFrame call.
struct FrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    HANDLE fenceEvent = nullptr;      // dedicated event for this slot's fence wait
    uint64_t fenceValue = 0;          // fence value of this slot's last submission
    // FG 分段提交(处理用时拆账):base CL(基础管线:NR/直通 + 真实帧输出)
    // 先提交并 signal baseFenceValue,FG 插帧链(推理 + 插值输出)录在独立
    // fg CL 上后提交并 signal fenceValue。两段 GPU 耗时 = 两个栅栏完成点的
    // CPU 墙钟差 —— timestamp query 与 NGX 同 CL 会 SEH(2026-09-05 实锤),
    // 栅栏差分是唯一无损精确拆分法。
    ComPtr<ID3D12CommandAllocator> fgAllocator;
    ComPtr<ID3D12GraphicsCommandList> fgCommandList;
    HANDLE baseFenceEvent = nullptr;  // base 段栅栏等待专用事件(fg 等待用 fenceEvent)
    uint64_t baseFenceValue = 0;      // base 段提交的栅栏值(恒 ≤ fenceValue)

    // YUV 原生管道:VS 帧(YUV420P8/P10 三平面)与 GPU 之间纯行拷贝。
    // [0]=Y 全分辨率,[1]=U [2]=V 半分辨率;pitch 256 对齐,persist-mapped。
    // **不直接对 upload buffer 建 buffer SRV**:typed buffer SRV 在本机驱动
    // (RTX 3080)上触发异步 DEVICE_HUNG(创建成功、下一次驱动调用报 device
    // removed,2026-09-07 实测;与 NULL 描述符异步 TDR 同族,#41-②)——
    // GPU 侧消费一律经 yuvIn 纹理(Texture2D SRV,全仓惯用形态)。
    ComPtr<ID3D12Resource> uploadYuv[3];
    void *uploadYuvMapped[3] = {};
    size_t uploadPitchYuv[3] = {};
    ComPtr<ID3D12Resource> readbackYuv[3];
    void *readbackYuvMapped[3] = {};
    size_t readbackPitchYuv[3] = {};
    // GPU 侧 YUV 纹理:In = upload 拷贝进来供 YUV→RGB 采样;Out = RGB→YUV
    // dispatch 写出供 readback。格式 R8_UNORM(8bit)/R16_UNORM(10bit),
    // 10bit 存储字 = VS P10 采样值(右对齐 0-1023,2026-09-08 实测)。
    ComPtr<ID3D12Resource> yuvIn[3];
    ComPtr<ID3D12Resource> yuvOut[3];
    ComPtr<ID3D12Resource> inputColor;   // W×H BGRA8,UAV(YUV→RGB 转换直写)
    ComPtr<ID3D12Resource> outputColor;  // W×H BGRA8, UAV (NGX / composite write)
    // residual scaling pipeline, sized by the current input_resolution
    ComPtr<ID3D12Resource> reducedColor;
    ComPtr<ID3D12Resource> reducedDenoised;
    ComPtr<ID3D12Resource> controlledRes; // internalW×internalH, signed FP16 (controls applied)
    ComPtr<ID3D12Resource> horizontalRes;
    // NVOF guidance(清单 #6):densify 输出的稠密运动/置信度(源尺寸),
    // 以及缩放启用时的降采样版(内部尺寸)。OF 关闭时保持 COMMON 不被触碰。
    ComPtr<ID3D12Resource> motion;        // W×H R16G16_FLOAT,UAV(densify 写)
    ComPtr<ID3D12Resource> confidence;    // W×H R8_UNORM,UAV
    ComPtr<ID3D12Resource> reducedMotion;     // internalW×internalH R16G16_FLOAT
    ComPtr<ID3D12Resource> reducedConfidence; // internalW×internalH R8_UNORM
    // DLSS FG(仅 _fgSlots 建立时分配):FG 插值输出(BGRA8,UAV —— DLSSG
    // 契约:输出为 UAV;NSR 化后走 RGB→YUV 第二遍转换)。
    ComPtr<ID3D12Resource> fgInterp;      // W×H BGRA8,UAV
    // FG 插值帧的独立回读缓冲(与真实帧的 readbackYuv 并存:同一条 CL 上
    // 先后多次转换+回读,真实帧回读不能被插值帧覆写)。按插值槽分组
    // [gen 0..kFgGenSlots-1][plane] —— 倍数 M 时 M-1 个插值帧各自落一组,
    // 每组转换+回读后 yuvOut 归位供下一槽复用。
    ComPtr<ID3D12Resource> readbackFg[kFgGenSlots][3];
    void *readbackFgMapped[kFgGenSlots][3] = {};
    size_t readbackPitchFg[kFgGenSlots][3] = {};
    // slot-local shader-visible heap: 0=srvInput 1=srvReducedColor
    // 2=srvReducedDenoised 3=srvHorizontal 4=uavReducedColor 5=uavReducedDenoised
    // 6=uavHorizontal 7=uavOutput 8=srvControlled 9=uavControlled
    // 10=srvMotion 11=srvConfidence 12=uavMotion 13=uavConfidence
    // 14=srvFlowF 15=srvFlowB 16=srvCostF 17=srvCostB(NVOF 会话纹理,
    // BindNvofResources 填充)18=srvReducedMotion 19=srvReducedConfidence
    // 20=uavReducedMotion 21=uavReducedConfidence
    // 22=srvOutput(outputColor 的 SRV,RGB→YUV 转换读;原 srvNvofSrc 随
    // nvofSrcTex 删除让位——降采样直接采样槽 0 srvInput)
    // 23=uavNvofInput0 24=uavNvofInput1(NVOF 注册输入纹理的 UAV,
    // BindNvofResources 填充)
    // 25=srvYuvIn0 26=srvYuvIn1 27=srvYuvIn2(YUV→RGB 转换采样)
    // 28=uavYuvOut0 29=uavYuvOut1 30=uavYuvOut2(RGB→YUV 写出)
    // 31=uavInput(inputColor 的 UAV,YUV→RGB 转换直写)
    // 32=srvFgInterp 33=uavFgInterp(FG 插值输出;非 FG 槽 = outputColor
    // 占位视图 —— 绝不写 NULL 描述符,见 14-17 注释)
    ComPtr<ID3D12DescriptorHeap> srvUavHeap;
};

class D3D12Context {
public:
    D3D12Context() = default;
    ~D3D12Context();
    D3D12Context(const D3D12Context &) = delete;
    D3D12Context &operator=(const D3D12Context &) = delete;

    bool Initialize(char *err, size_t errLen) noexcept;
    void Finalize() noexcept;

    ID3D12Device *Device() const noexcept { return _device.Get(); }
    IDXGIAdapter1 *Adapter() const noexcept { return _adapter.Get(); }
    ID3D12CommandQueue *Queue() const noexcept { return _queue.Get(); }

    // Control-path recording (NGX CreateFeature / diagnostics dump): guarded
    // by CtlMutex, never overlaps a slot's frame work on this list.
    std::mutex &CtlMutex() noexcept { return _ctlMutex; }
    bool BeginCtlRecording() noexcept;
    ID3D12GraphicsCommandList *CtlCommandList() const noexcept { return _ctlCommandList.Get(); }
    bool ExecuteCtlAndWait() noexcept;
    // True once a fence wait timed out (GPU hang / device removal): callers
    // should stop evaluating instead of stalling the full wait every frame.
    bool IsDeviceLost() const noexcept { return _deviceLost.load(std::memory_order_relaxed); }

    // depth = YUV 位深(8/10)。同尺寸换深度必须走重建(hotMatch 侧拦截,
    // 否则 R8 槽纹理遇 P10 打包 = 数据撕裂)。
    // fg = 建 DLSS FG 槽资源(插值输出纹理 + 第二组回读缓冲)。create-time
    // 语义:FG 激活与否决定帧资源形态,变化走 PoolHold 全量重建(同 Rebind
    // 尺寸变化路径),不做逐槽懒补。
    bool CreateFrameResources(int width, int height, int depth, bool fg,
                              char *err, size_t errLen) noexcept;
    bool FgSlots() const noexcept { return _fgSlots; }

    // diagnostics: dump a texture's raw rows to a file (VSDLSSNR_DUMP);
    // format must match the resource (CopyTextureRegion has no cross-family
    // conversion). Uses the control path; caller holds CtlMutex.
    bool DumpTextureToFile(ID3D12Resource *tex, int width, int height,
                           const wchar_t *path,
                           DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) noexcept;

    // Residual scaling rebuilds. The caller must hold a PoolHold (all slots
    // idle, pool sealed) — the rebuild replaces every slot's scaling
    // textures; Initialize may call these directly (single-threaded).
    bool RebuildScaling(int internalW, int internalH, char *err, size_t errLen) noexcept;
    // Drop the residual pipeline entirely (scaling disabled).
    void ClearScalingResources() noexcept;
    bool HasScaling() const noexcept { return _scalingReady; }
    int InternalWidth() const noexcept { return _internalWidth; }
    int InternalHeight() const noexcept { return _internalHeight; }

    // Frame slot pool. Acquire blocks while kSlotCount frames are in flight;
    // the caller must ReleaseSlot exactly once on every path.
    FrameSlot *AcquireSlot() noexcept;
    void ReleaseSlot(FrameSlot *slot) noexcept;

    // NVOF flow/cost 纹理的 SRV 写入每个槽的描述符堆(14-17),注册输入
    // 纹理的 UAV 写 23/24(GPU 光流输入降采样直写目标);资源为 null 时
    // 写占位描述符(densify/降采样只在会话存活时被记录,占位永不被有效
    // 读取;绝不写 NULL 描述符)。
    bool BindNvofResources(ID3D12Resource *flowFwd, ID3D12Resource *flowBwd,
                           ID3D12Resource *costFwd, ID3D12Resource *costBwd,
                           ID3D12Resource *inputFwd, ID3D12Resource *inputBwd) noexcept;
    // densify(Magpie NVOF_Densify HLSL 原样):S10.5 网格 → 稠密运动 +
    // 置信度。在 NVOF 会话的 nvof CL 上执行(门内、execute 完成后),
    // 调用方负责 motion/confidence 的 UAV 态转移。gridSize/旗标来自
    // NvofContext 会话。motionScale = 流向量单位换算(会话输入像素 →
    // 源像素;输入未降采样时为 1,1)。
    // densify:网格流 → 稠密运动场。denseW/H = 稠密目标尺寸(= dispatch 范围,
    // shader 的 SourceExtent);uavMotion/uavConfidence = 目标 UAV 描述符 ——
    // 源尺寸管线写 12/13(slot.motion/confidence),follow 内部管线直接写
    // 20/21(slot.reducedMotion/reducedConfidence,NGX 缩放消费纹理)。
    void RecordDensify(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                       uint32_t denseW, uint32_t denseH,
                       uint32_t flowW, uint32_t flowH, uint32_t gridSize,
                       bool hasForwardCost, bool hasBackward, bool hasBackwardCost,
                       float motionScaleX, float motionScaleY,
                       UINT uavMotion, UINT uavConfidence) noexcept;
    // 缩放启用时的 guidance 降采样(Magpie DownsampleGuidance;深度输出
    // 在本宿主是死重 —— depth 恒为零纹理,NGX 直接消费静态零纹理)。
    void RecordGuidanceDownsample(FrameSlot &slot) noexcept;
    // YUV↔RGB GPU 转换(双站点:录在 nvof CL 或槽 CL,见 Plan 定稿)。
    // **cl 由调用方显式给定** —— postCopy 回调拿到的是 nvof CL,槽 CL 路径
    // 传 slot.commandList;录错列表 = 命令被对方 Reset 销毁(首测实锤:
    // 转换被 Reset 吃掉,NVOF/NGX 全链吃零 → 黑帧)。
    // ConvertInput:3×yuvUpload→yuvIn 拷贝(COPY_DEST→NSR)→ dispatch 采样
    // Y/U/V 双线性上采色度、按矩阵/范围展开 → UAV 直写 inputColor →
    // stateAfter(NSR=NGX 待读 / COMMON=直通拷贝);yuvIn 收尾归 COMMON。
    // 纯录制,无失败路径(void)。
    void RecordConvertInput(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                            ColorMatrix matrix, ColorRange range,
                            D3D12_RESOURCE_STATES stateAfter) noexcept;
    void RecordYuvOutput(ID3D12GraphicsCommandList &cl, FrameSlot &slot, ColorMatrix matrix,
                         ColorRange range, D3D12_RESOURCE_STATES outputStateBefore,
                         ID3D12Resource *srcColor = nullptr,
                         UINT srcSrvIndex = kSrvOutputColor) noexcept;
    // 光流输入降采样(#46/#48):YUV→RGB 转换已在同一 CL 上产出 inputColor
    // (NSR),本 pass 直接采样槽 0 srvInput 双线性写 NVOF 注册输入纹理
    // inputIndex(0/1,UAV 23/24)。调用方(NvofContext)负责注册纹理的
    // COMMON→UAV→COMMON 屏障。
    void RecordNvofDownsample(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                              int dstW, int dstH, int inputIndex) noexcept;

    // RAII: drain the pool (wait until every slot is released) and hold the
    // pool mutex, so AcquireSlot cannot hand out a slot while the holder
    // replaces shared NGX state / per-slot textures. Never call this while
    // holding a slot yourself.
    class PoolHold {
    public:
        explicit PoolHold(D3D12Context &ctx) noexcept;
        ~PoolHold() noexcept;
        PoolHold(const PoolHold &) = delete;
        PoolHold &operator=(const PoolHold &) = delete;
    private:
        D3D12Context *_ctx;
        std::unique_lock<std::mutex> _lock;
        friend class D3D12Context;
    };

    // Per-slot frame path.
    // YUV 原生:VS 三平面 → uploadYuv[3] 纯行拷贝(无像素算术)。
    bool PackInput(FrameSlot &slot, const uint8_t *const *srcPlanes, const int64_t *srcStrides,
                   int width, int height, char *err, size_t errLen) noexcept;
    bool BeginFrameRecording(FrameSlot &slot) noexcept;
    // FG 分段 CL 的录制起点(防砖 retry 与 base 同款)。仅 FG 插帧链录在
    // fgCommandList 上;门关帧不 Begin/不提交,slot.fenceValue 停在 base 值。
    bool BeginFgRecording(FrameSlot &slot) noexcept;
    // 记录:yuvOut ×3 UAV→COPY_SOURCE→拷贝→COMMON(在 RecordYuvOutput 之后,
    // yuvOut 处于 UAV 态)。fgGen >= 0 = 拷入 FG 插值帧第 fgGen 组回读缓冲;
    // 同一条 CL 上真实帧 + 各插值槽的转换+回读共用 yuvOut,目标缓冲必须
    // 两两不同。cl 由调用方显式给定(真实帧 = base CL,插值 = fg CL)。
    bool RecordReadbackCopy(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                            char *err, size_t errLen, int fgGen = -1) noexcept;
    // 分段提交(处理用时拆账):base = 基础管线(NR/直通 + 真实帧输出),
    // fg = 插帧链(FG 推理 + 插值输出)。base 先提交(栅栏值 baseFenceValue),
    // fg CL 录完后提交(栅栏值 fenceValue)。CPU 侧先后等两个栅栏,完成点
    // 差 = 各段 GPU 耗时(等 fg 前先等 base:fg 排 base 后,醒来时 base 必
    // 已完成,等待开销 = 一次事件唤醒)。waitFence/waitValue = NVOF copy
    // 栅栏(可选,base 段 GPU 消费 flow 前排队;现状恒空 —— StageFrame 已
    // CPU 等待 NVOF execute 完成)。
    bool SubmitBaseFrame(FrameSlot &slot, ID3D12Fence *waitFence, uint64_t waitValue,
                         char *err, size_t errLen) noexcept;
    bool SubmitFgFrame(FrameSlot &slot, char *err, size_t errLen) noexcept;
    bool WaitBaseFrame(FrameSlot &slot, char *err, size_t errLen) noexcept;
    bool WaitFrame(FrameSlot &slot, char *err, size_t errLen) noexcept;   // fence wait, device-lost aware
    // GPU 完成后调用:回读缓冲 → VS 三平面(纯 CPU 行拷贝,色度半尺寸)。
    // fgGen >= 0 = 读 FG 插值帧第 fgGen 组缓冲。
    bool UnpackOutput(FrameSlot &slot, uint8_t **dstPlanes, int64_t *dstStrides,
                      int width, int height, char *err, size_t errLen,
                      int fgGen = -1) noexcept;

    // 五段残差 compute 路径的命令记录(在该槽已 BeginFrameRecording 的列表上)。
    // 降采样两段可分离(Lanczos2),horizontalRes 复用为降采样 FP16 中间纹理
    // (与上游 resampleIntermediate 的复用方式一致)。
    void RecordDownsampleVertical(FrameSlot &slot, const ResidualControls &rc) noexcept;
    void RecordDownsampleHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept;
    void RecordResidualPrepare(FrameSlot &slot, const ResidualControls &rc) noexcept;
    void RecordResidualHorizontal(FrameSlot &slot, const ResidualControls &rc) noexcept;
    // equalWidth: 内部宽度与源一致时跳过 horizontal pass,垂直 pass 直读
    // controlledRes(Magpie 的 verticalResidual 绑定切换)。
    void RecordResidualVertical(FrameSlot &slot, const ResidualControls &rc,
                                bool equalWidth) noexcept;

    ID3D12Resource *InputColor(FrameSlot &s) const noexcept { return s.inputColor.Get(); }
    ID3D12Resource *OutputColor(FrameSlot &s) const noexcept { return s.outputColor.Get(); }
    ID3D12Resource *FgInterp(FrameSlot &s) const noexcept { return s.fgInterp.Get(); }
    // 描述符堆槽位(RecordYuvOutput / FG 转换共用)。
    static constexpr UINT kSrvOutputColor = 22; // outputColor 的 SRV
    static constexpr UINT kSrvFgInterp = 32;    // fgInterp 的 SRV(FG 槽)
    // YUV 原生化 dump/调试:输出平面([0]=Y [1]=U [2]=V)与位深。
    ID3D12Resource *YuvOutPlane(FrameSlot &s, int plane) const noexcept { return s.yuvOut[plane].Get(); }
    ID3D12Resource *YuvInPlane(FrameSlot &s, int plane) const noexcept { return s.yuvIn[plane].Get(); }
    int BitDepth() const noexcept { return _bitDepth; }
    ID3D12Resource *ReducedColor(FrameSlot &s) const noexcept { return s.reducedColor.Get(); }
    ID3D12Resource *ReducedDenoised(FrameSlot &s) const noexcept { return s.reducedDenoised.Get(); }
    ID3D12Resource *ControlledRes(FrameSlot &s) const noexcept { return s.controlledRes.Get(); }
    ID3D12Resource *HorizontalRes(FrameSlot &s) const noexcept { return s.horizontalRes.Get(); }
    // PARAM_MVEC 的取材:真光流(densify/降采样输出)或静态零纹理。
    ID3D12Resource *MotionResource(FrameSlot &s, bool realMotion,
                                   bool scaling) const noexcept {
        if (!realMotion) return _motion.Get();
        return scaling ? s.reducedMotion.Get() : s.motion.Get();
    }
    // 最近 1 秒的帧数即帧率(计数式)。播放节奏由宿主决定:卡顿时宿主积压,
    // 恢复后突发+并发拉帧(fmParallel 入口 Δt 可到亚毫秒),倒数式 EMA 会把
    // 追赶吞吐当帧率冲高;计数对并发与突发免疫,停顿(窗口内无新帧)读数
    // 自然回落。
    double FrameRateWindow() noexcept;
    void NotifyFrameTick(double qpcSeconds) noexcept; // 帧入口计数打点
    // 零 guidance(Force Zero,等价 Magpie guidanceMode=1):
    // motion R16G16_FLOAT、depth R32_FLOAT,内容全 0,常驻 NSR 只读
    ID3D12Resource *Motion() const noexcept { return _motion.Get(); }
    ID3D12Resource *Depth() const noexcept { return _depth.Get(); }

private:
    bool CreateSlotResources(FrameSlot &slot, int depth, char *err, size_t errLen) noexcept;
    bool CreateScalingForSlot(FrameSlot &slot, int iw, int ih, char *err, size_t errLen) noexcept;
    void ClearScalingForSlot(FrameSlot &slot) noexcept;
    // FG 槽纹理/回读缓冲/描述符(仅 _fgSlots);CreateSlotResources 尾部调用。
    bool CreateFgSlotResources(FrameSlot &slot, char *err, size_t errLen) noexcept;
    bool WaitFenceValue(uint64_t value, HANDLE event, char *err, size_t errLen) noexcept;
    // Shared body of the three raw buffer creations (upload / readback /
    // diagnostics dump): heap type, initial state and the 256-aligned pitch
    // are the only differences between them.
    bool CreateRawBuffer(UINT64 bytes, D3D12_HEAP_TYPE heapType,
                         D3D12_RESOURCE_STATES initialState,
                         ID3D12Resource **out, size_t &alignedPitch,
                         UINT bytesPerRow, char *err, size_t errLen) noexcept;

    // Shared prologue of the four residual passes; only the PSO, descriptor
    // slots and dispatch dims differ. cbuffer layout (root constants):
    // SourceExtent@0, TargetExtent@2, Padding0@4, MotionScale@5,
    // ResidualMultiplier@7, ResidualSaturation@8, ResidualLightness@9,
    // ShadowStructureMultiplier@10, ReflectionGlowMultiplier@11 — mirrors the
    // four HLSL cbuffer blocks (Magpie ResampleConstants, 48 bytes).
    void RecordPass(FrameSlot &slot, ID3D12PipelineState *pso, UINT srv0, UINT srv1, UINT uav,
                    UINT dispatchX, UINT dispatchY, const ResidualControls &rc) noexcept;
    // guidance 降采样专用:同 12 常量 cbuffer,但 SRV/UAV 各两个
    // (t0/t1 = motion/confidence,u0/u1 = reducedMotion/reducedConfidence)。
    void RecordGuidancePass(FrameSlot &slot, ID3D12PipelineState *pso,
                            UINT srv0, UINT srv1, UINT uav0, UINT uav1,
                            UINT dispatchX, UINT dispatchY) noexcept;

    bool CreateColorTexture(ID3D12Resource **out, int width, int height,
                            DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState,
                            D3D12_RESOURCE_FLAGS flags,
                            char *err, size_t errLen) noexcept;
    bool CreateComputeObjects(char *err, size_t errLen) noexcept;
    void SetErr(char *err, size_t errLen, HRESULT hr, const char *what) const noexcept;

    ComPtr<ID3D12Device> _device;
    ComPtr<IDXGIAdapter1> _adapter; // the adapter the device was created on
    ComPtr<ID3D12InfoQueue> _infoQueue;
    bool _debug = false;
    ComPtr<ID3D12CommandQueue> _queue;
    ComPtr<ID3D12Fence> _fence;
    std::atomic<uint64_t> _fenceValue{0};
    std::atomic<bool> _deviceLost{false};
    // serializes fetch_add + Signal so the fence value order matches the
    // queue's ExecuteCommandLists order (a fence value must never regress)
    std::mutex _submitMutex;

    // control path: CreateFeature / diagnostics (never concurrent with itself)
    std::mutex _ctlMutex;
    ComPtr<ID3D12CommandAllocator> _ctlAllocator;
    ComPtr<ID3D12GraphicsCommandList> _ctlCommandList;
    HANDLE _ctlEvent = nullptr;

    int _width = 0;
    int _height = 0;
    int _bitDepth = 0;  // YUV 位深(8/10;不叫 _depth —— 那是零 guidance 深度纹理)
    int _chromaW = 0;   // 色度平面尺寸 = (w+1)>>1 / (h+1)>>1
    int _chromaH = 0;

    // shared read-only resources
    ComPtr<ID3D12Resource> _motion; // resident NON_PIXEL_SHADER_RESOURCE
    ComPtr<ID3D12Resource> _depth;  // resident NON_PIXEL_SHADER_RESOURCE
    ComPtr<ID3D12DescriptorHeap> _rtvHeap;

    // residual compute objects (shared: PSOs are stateless)
    ComPtr<ID3D12RootSignature> _rsCompute;
    ComPtr<ID3D12PipelineState> _psoDownsampleVertical;
    ComPtr<ID3D12PipelineState> _psoDownsampleHorizontal;
    ComPtr<ID3D12PipelineState> _psoPrepare;
    ComPtr<ID3D12PipelineState> _psoHorizontal;
    ComPtr<ID3D12PipelineState> _psoVertical;
    // NVOF guidance(清单 #6):densify + guidance 降采样各用独立根签名
    // (densify = 4 SRV + 2 UAV + 8 常量;降采样 = 2 SRV + 2 UAV + 12 常量)。
    ComPtr<ID3D12RootSignature> _rsDensify;
    ComPtr<ID3D12PipelineState> _psoDensify;
    ComPtr<ID3D12RootSignature> _rsGuidance;
    ComPtr<ID3D12PipelineState> _psoGuidanceDownsample;
    // 光流输入降采样(#48):inputColor(NSR,槽 0 srvInput)双线性写 NVOF
    // 注册输入纹理(1 SRV + 1 UAV + 5 常量)。
    ComPtr<ID3D12RootSignature> _rsNvofDownsample;
    ComPtr<ID3D12PipelineState> _psoNvofDownsample;
    // YUV↔RGB 转换(YUV 原生化):深度/矩阵/范围全走 root constants,
    // R8/R16_UNORM 的 Texture2D<float> 视图同构 —— 仅 3 个 PSO:
    // convertIn(Y/U/V 3 SRV → inputColor 1 UAV,8 常量);
    // convertOut luma/chroma(outputColor 1 SRV → yuvOut 2 UAV——luma 用
    // u0、chroma 用 u0/u1,共享根签名,10 常量:系数 4 + 范围 4 + 尺寸 2)。
    ComPtr<ID3D12RootSignature> _rsConvertIn;
    ComPtr<ID3D12PipelineState> _psoConvertIn;
    ComPtr<ID3D12RootSignature> _rsConvertOut;
    ComPtr<ID3D12PipelineState> _psoConvertOutLuma;
    ComPtr<ID3D12PipelineState> _psoConvertOutChroma;

    // frame slot pool
    static constexpr int kSlotCount = 3;
    FrameSlot _slots[kSlotCount];
    int _freeStack[kSlotCount];
    int _freeCount = 0;
    std::mutex _poolMutex;
    std::condition_variable _poolCv;

    int _internalWidth = 0;
    int _internalHeight = 0;
    bool _scalingReady = false;
    bool _fgSlots = false; // 槽池当前含 FG 资源(CreateFrameResources 的 fg 旗标)
    std::mutex _tickMutex;
    static constexpr int kTickRingCap = 1024; // 1s 窗的容量上限(超出按 1024fps 封顶)
    double _tickRing[kTickRingCap] = {};
    int _tickHead = 0;  // 下一写入位
    int _tickCount = 0; // 有效条目数(绕环前等于已写个数)
};

} // namespace vsdlssnr
