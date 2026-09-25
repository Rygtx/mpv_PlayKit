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
// RecordColorOutput 的源颜色形态(选 PSO 对;输出契约 Sdr=随源位深,
// Hdr* = P10 420):Sdr = BGRA8 码域;HdrScRgb = FP16 scRGB 线性(逐像素
// PqEncode);HdrPqCodes = FP16 PQ 码域(PQ 域插帧产物,码直读免 pow)。
enum class ColorOutKind : int { Sdr = 0, HdrScRgb = 1, HdrPqCodes = 2 };

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
    // postB 转换段(TrueHDR 后置重构,2026-09-24):HDR 会话的输出转换
    // (PQ/SDR → YUV)依赖 TrueHDR 专用队列逐帧产出,与 postA(FG 推理)
    // 之间隔着跨队列 TrueHDR 链 —— 独立第三 CL + 栅栏。非 HDR 会话不录制
    // 不提交,资源闲置无害。
    ComPtr<ID3D12CommandAllocator> postAllocator;
    ComPtr<ID3D12GraphicsCommandList> postCommandList;
    // postB 段无专用等待事件(无消费者,已于 2026-09-25 删除);
    // postB 栅栏等待统一走 WaitFenceValuePublic + 事件入参。
    uint64_t postFenceValue = 0;      // postB 段提交的栅栏值
    uint64_t fgFenceValue = 0;        // postA(fg CL)提交的栅栏值(计时锚)
    // base 段 GPU 完成时间戳路径(2026-09-25:WaitBaseFrame CPU 阻塞删除,
    // RTX/fg/post 提交链与 base GPU 执行重叠)。timestamp query 与 NGX 同 CL
    // 会 SEH(2026-09-05 实锤)—— 独立微型 CL 括号对夹住各段(同一
    // _submitMutex 保证队列序),EndQuery + ResolveQueryData 写 READBACK 缓冲;
    // post CL FIFO 在其后,WaitFrame(post 栅栏)完成即蕴含全部 ts 完成,
    // Finish 无额外等待直接读。GetClockCalibration 在提交时采样,GPU tick →
    // QPC 线性换算在 Finish 做(值存 tsGpuCal/tsCpuCal,随槽生命周期稳定
    // —— Finish 读完后才 ReleaseSlot)。
    //
    // 括号布局(2026-09-25 账目诚实化:每段只记自己名下的 GPU 执行时间,
    // 队列积压单独成段,不折进任何处理段):
    //   [preBase][base][postBase]          base 纯执行 = postBase−preBase
    //   [preFg][fg][postFg]                DLSSG 纯执行(SumbitFgFrame 内)
    //   post CL 首尾内嵌 EndQuery(4/5)    post 纯执行(自绘 shader 无 NGX,
    //                                      可同 CL 打点;其跨队列栅栏等待
    //                                      属于排队,不属 conv)
    //   0=preBase 1=postBase 2=preFg 3=postFg 4=post0 5=post1(readback 布局)
    ComPtr<ID3D12QueryHeap> tsQueryHeap;    // 6 查询(容量 8 取整)
    ComPtr<ID3D12CommandAllocator> tsAllocator;   // postBase 括号
    ComPtr<ID3D12GraphicsCommandList> tsCommandList;
    ComPtr<ID3D12CommandAllocator> tsPreBaseAllocator; // preBase 括号
    ComPtr<ID3D12GraphicsCommandList> tsPreBaseCommandList;
    ComPtr<ID3D12CommandAllocator> tsPreFgAllocator;   // preFg/postFg 括号
    ComPtr<ID3D12GraphicsCommandList> tsPreFgCommandList;
    ComPtr<ID3D12CommandAllocator> tsPostFgAllocator;
    ComPtr<ID3D12GraphicsCommandList> tsPostFgCommandList;
    ComPtr<ID3D12Resource> tsReadback;      // 64 字节 READBACK(6×UINT64),persist-mapped
    void *tsReadbackMapped = nullptr;
    UINT64 tsGpuCal = 0;                    // 校准点 GPU tick(base 提交时)
    UINT64 tsCpuCal = 0;                    // 校准点 QPC
    UINT64 submitQpc = 0;                   // SubmitBaseFrame 入口 QPC(排队段锚)
    bool tsValid = false;                   // ts CL 已提交且校准成功(失败帧回退阻塞观测)

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
    // DLSS FG(仅 _fgSlots 建立时分配):FG 插值输出,**按插值槽一组**
    // (TrueHDR 后置:postA 全部 gen 写完才有 TrueHDR 链消费 —— 单纹理会被
    // 下一 gen 覆写)。恒 BGRA8(DLSSG 恒在 SDR 域插值;HDR 会话由逐帧
    // TrueHDR 提升为 FP16 scRGB —— DLSSG 的 ColorBuffersHDR 路径实测压高光,
    // 2026-09-23 实验定案)。UAV(DLSSG 契约);NSR 化后供转换/HDR 输入。
    // 实验 fgHdrInterp = FP16,内容 = **PQ 码域 BT.2020 RGB(≤1.0)**:DLSSG
    // 的 HDR 路径对 >1.0 scRGB 线性值不保真(插值帧高光钳 ~0.875 = 70 nits),
    // 感知码域走 LDR 路径无损(原生 HDR 直通同域实证,2026-09-24 定案)。
    ComPtr<ID3D12Resource> fgInterp[kFgGenSlots];
    // PQ 域插帧的 DLSSG backbuffer(仅 _fgHdrInterp):TrueHDR 真实帧输出
    // hdrColor(FP16 scRGB)经编码 pass(HdrToPq)写入 —— FP16 PQ 码 @PIPE,
    // ColorBuffersHDR=0。插值输出 fgInterp[g] 同域,post 直读码做 BT.2020
    // 矩阵(免逐像素 PqEncode)。UAV(编码写)→ NSR(DLSSG 读)。
    ComPtr<ID3D12Resource> fgBack;
    // TrueHDR 逐插值帧输出(仅 _fgSlots && _hdrPipe):FP16 scRGB @PIPE,
    // postB 经 SRV(44-48)做 PQ 转换。NGX 写后衰减 COMMON。
    ComPtr<ID3D12Resource> hdrFg[kFgGenSlots];
    // RTX Video 管线纹理(PIPE 尺寸;VSR/HDR 请求时创建,无则占位视图):
    //   vsrColor — VSR 输出(BGRA8,UAV)
    //   hdrColor — TrueHDR 真实帧输出(FP16 scRGB,UAV;PQ 转换源)
    //   motionDense — 源尺寸运动场的 PIPE 尺寸双线性放大(FG MVecs 契约 =
    //                 backbuffer 同尺寸;PIPE==src 时无纹理直接用 motion)
    ComPtr<ID3D12Resource> vsrColor;      // PIPEW×PIPEH BGRA8,UAV
    ComPtr<ID3D12Resource> hdrColor;      // PIPEW×PIPEH FP16,UAV
    ComPtr<ID3D12Resource> motionDense;   // PIPEW×PIPEH R16G16_FLOAT,UAV
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
    // 32/33=fgInterp[0] 的 SRV/UAV 占位(历史槽位;逐 gen 视图在 39-43。
    // 非 FG 槽 = outputColor 占位视图 —— 绝不写 NULL 描述符,见 14-17 注释)
    // 39-43=srvFgInterp[0..4](FG 插值输出逐 gen SRV,转换读/HDR 输入读)
    // 34=uavDebugDiff(共享差异调试纹理的 UAV,"差异调试 ×20" 视图写入目标;
    // 资源为 context 级单例,每槽堆各持一份视图)
    // 35=srvZeroMotion(静态零运动纹理的 SRV,光流场视图无真运动帧绑定;
    // 资源为 context 级单例 _motion,常驻 NSR)
    // 44-48=srvHdrFg[0..4](TrueHDR 逐插值帧输出 FP16 SRV,PQ 转换读;
    // 非 HDR 槽 = hdrColor/outputColor 占位)
    // 49=uavFfxInput(FFX 会话输入,R8G8B8A8 OF extent;BindOfResources 填充)
    // 50=srvFfxSparse(FFX 稀疏流 R16G16_SINT,densify 读)
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
    // 全局提交栅栏(SubmitBaseFrame 的 signal 源;RTX 专用队列以它为
    // 生产者等待锚)。
    ID3D12Fence *Fence() const noexcept { return _fence.Get(); }

    // Control-path recording (NGX CreateFeature / diagnostics dump): guarded
    // by CtlMutex, never overlaps a slot's frame work on this list.
    std::mutex &CtlMutex() noexcept { return _ctlMutex; }
    bool BeginCtlRecording() noexcept;
    ID3D12GraphicsCommandList *CtlCommandList() const noexcept { return _ctlCommandList.Get(); }
    // err 非空时带出精确原因(Close 失败 hr / 等待失败原因);失败同时落
    // timing log + info queue 消息(见 ReportInfoQueue)。where = 失败位置
    // 标签(必填,7 个调用点全部显式传),进日志与 err
    // ("guidance clear: ctl close failed hr=0x...")。
    bool ExecuteCtlAndWait(char *err, size_t errLen, const char *where) noexcept;
    // ctl 路径失败点把 info queue 存量(ERROR/CORRUPTION)拉进 timing log
    // —— debug layer 报错此前只进调试器输出,mpv 进程内不可观测
    // (VSDLSSNR_D3D12_DEBUG=1 时 _infoQueue 存在)。复用 DebugDumpInfoQueue。
    void ReportInfoQueue(const char *where) noexcept;
    // True once a fence wait timed out (GPU hang / device removal): callers
    // should stop evaluating instead of stalling the full wait every frame.
    bool IsDeviceLost() const noexcept { return _deviceLost.load(std::memory_order_relaxed); }

    // depth = YUV 位深(8/10)。同尺寸换深度必须走重建(hotMatch 侧拦截,
    // 否则 R8 槽纹理遇 P10 打包 = 数据撕裂)。
    // fg = 建 DLSS FG 槽资源(插值输出纹理 + 第二组回读缓冲)。create-time
    // 语义:FG 激活与否决定帧资源形态,变化走 PoolHold 全量重建(同 Rebind
    // 尺寸变化路径),不做逐槽懒补。
    // RTX Video 双尺寸(创建时,无 RTX 时 pipe/out = 源尺寸):
    //   pipeW/H — VSR 输出 / TrueHDR / FG backbuffer 所在的管线尺寸
    //             (vsr 开 = min(目标, 源×4);关 = 源)
    //   outW/H  — YUV 输出平面尺寸(vsr 开 = 目标;关 = 源)
    //   vsr     — 建 vsrColor 槽纹理(PIPE≠src 时必有;=src 且 vsr 旁路时无)
    //   hdr     — TrueHDR:输出平面切 P10(PQ)
    //   fgHdrInterp — 实验性补帧 HDR 域插帧(PQ 域):fgInterp 切 FP16(PQ
    //                 码域内容)、hdrFg 不建、fgBack 建立(DLSSG backbuffer;
    //                 插值输出即 FG 的 FP16 产物,无逐帧 TrueHDR)
    //   subW/subH — 输入色度抽取档(0=全、1=半;420=(1,1)、422=(1,0)、
    //                 444=(0,0))。SDR 输出同布局,HDR P10 输出恒 420。
    //   rgb       — VS RGBP 计划族直读(零矩阵;平面序 G/B/R)
    bool CreateFrameResources(int width, int height, int depth, bool fg,
                              int pipeW, int pipeH, int outW, int outH,
                              bool vsr, bool hdr, bool fgHdrInterp,
                              int subW, int subH, bool rgb,
                              char *err, size_t errLen) noexcept;
    bool FgSlots() const noexcept { return _fgSlots; }
    // RTX Video 管线几何(create-time 定格;ProcessFrame/插件侧共用)。
    int PipeWidth() const noexcept { return _pipeW; }
    int PipeHeight() const noexcept { return _pipeH; }
    int OutWidth() const noexcept { return _outW; }
    int OutHeight() const noexcept { return _outH; }
    bool HdrPipe() const noexcept { return _hdrPipe; }
    // yuvOut/readback 平面资源格式(OUT 位深;HDR 会话 P10 = R16)。dump
    // footprint 必须用这个 —— 按源 BitDepth 推格式在"8bit 源 + P10 出"的
    // 混合会话(RTX VSR+HDR)恒错,R8 footprint 拷 R16 资源 = Close 报
    // E_INVALIDARG(2026-09-25 dump 三连失败真因)。
    DXGI_FORMAT OutFormat() const noexcept { return _outFmt; }
    // fgInterp 槽纹理格式(SDR 域 BGRA8 / fgHdrInterp 会话 FP16)。
    DXGI_FORMAT FgInterpFormat() const noexcept {
        return _fgHdrInterp ? DXGI_FORMAT_R16G16B16A16_FLOAT
                            : DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    bool VsrPipe() const noexcept { return _vsrSlots; }

    // 诊断探针:把 InfoQueue 已存消息格式化进 buf(debug layer 开启时)。
    // 非 SetErr 失败点(如命令列表 Close 失败)定位用。
    void DebugDumpInfoQueue(char *buf, size_t len) const noexcept;

    // diagnostics: dump a texture's raw rows to a file (VSDLSSNR_DUMP);
    // format must match the resource (CopyTextureRegion has no cross-family
    // conversion). Uses the control path; caller holds CtlMutex. err/errLen
    // 可选失败原因出参(2026-09-25:此前四类失败点全部静默)。
    bool DumpTextureToFile(ID3D12Resource *tex, int width, int height,
                           const wchar_t *path,
                           DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                           char *err = nullptr, size_t errLen = 0) noexcept;

    // Residual scaling rebuilds. The caller must hold a PoolHold (all slots
    // idle, pool sealed) — the rebuild replaces every slot's scaling
    // textures; Initialize may call these directly (single-threaded).
    bool RebuildScaling(int internalW, int internalH, char *err, size_t errLen) noexcept;
    // Drop the residual pipeline entirely (scaling disabled).
    void ClearScalingResources() noexcept;
    bool HasScaling() const noexcept { return _scalingReady; }
    // 适配器 PCI VendorId(0x10DE=NVIDIA,0x1002=AMD;nvof 厂商门用)。
    UINT VendorId() const noexcept { return _vendorId; }
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
    // RTX Video 管线的颜色输出(PIPE 尺寸源 → OUT 尺寸 YUV 平面):
    //   Sdr       → BGRA8 源,归一化 uv 双线性缩放采样(SDR;矩阵/范围
    //                同 RecordYuvOutput);1:1 时与旧路径逐位同价。
    //   HdrScRgb  → FP16 scRGB 线性源(HDR):709→2020 线性域转换 →
    //                PQ(ST 2084)编码 → BT.2020 limited → P10 平面。
    //                scRGB 语义:1.0 = 80 nits(SDR 参考白)。
    //   HdrPqCodes → FP16 PQ 码域源(真HDR 插帧的 fgInterp[g]):码直读,
    //                免逐像素 PqEncode;BT.2020 limited → P10 平面。
    // 源状态契约与 RecordYuvOutput 相同:stateBefore(UAV/NSR)→ NSR 转换 →
    // 收尾归 COMMON;yuvOut 留 UAV 交 RecordReadbackCopy。dstW/H = OUT 尺寸。
    void RecordColorOutput(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                           ID3D12Resource *srcColor, UINT srcSrvIndex,
                           ColorOutKind kind, int srcW, int srcH,
                           ColorMatrix matrix, ColorRange range,
                           D3D12_RESOURCE_STATES stateBefore) noexcept;
    // PQ 域插帧的编码 pass(TrueHDR 真实帧产物 → DLSSG backbuffer):hdrColor
    // (FP16 scRGB,stateBefore=NGX 衰减 COMMON)→ 码域 2020 编码 → fgBack
    // (FP16 PQ 码 ≤1.0)→ NSR 作 DLSSG 输入。DLSSG 的 ColorBuffersHDR=1
    // 路径对 >1.0 线性值不保真(插值帧高光钳 ~0.875),感知码域走 LDR 路径
    // 无损(原生 HDR 直通同域实证);编码一次随真实帧,插值输出码域直读。
    // 仅 fgHdrInterp 且 fg 段开启时被记录(fg CL 上、eval 前 —— SubmitFgFrame
    // 等 TrueHDR 链尾栅栏,跨队列就绪已闭合)。
    void RecordHdrToPq(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                       int w, int h,
                       D3D12_RESOURCE_STATES stateBefore) noexcept;
    // 源尺寸稠密运动场 → PIPE 尺寸双线性放大(FG MVecs 契约 = backbuffer
    // 同尺寸,像素单位向量;双线性对 R16G16F 向量场 = 线性插值,语义保真)。
    // 状态契约:motion(NSR)→ 本 pass SRV 读;motionDense COMMON→UAV→NSR。
    // 仅 PIPE≠src 且 FG 激活时被记录。
    void RecordMotionScale(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                           int srcW, int srcH) noexcept;
    // 光流输入降采样(#46/#48):YUV→RGB 转换已在同一 CL 上产出 inputColor
    // (NSR),本 pass 直接采样槽 0 srvInput 双线性写 NVOF 注册输入纹理
    // inputIndex(0/1,UAV 23/24)。调用方(NvofContext)负责注册纹理的
    // COMMON→UAV→COMMON 屏障。
    void RecordNvofDownsample(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                              int dstW, int dstH, int inputIndex) noexcept;

    // ---- AMD 光流后端(FFX;PSO 全部在本类,context 只做编排)----
    // 会话纹理视图写入每槽堆:FFX(uavFfxInput 49 / srvFfxSparse 50)。资源
    // 为 null 的槽位不动(绝不写 NULL 描述符)。会话建立时调用(PoolHold 内)。
    bool BindOfResources(ID3D12Resource *ffxInput, ID3D12Resource *ffxSparse) noexcept;
    // FFX Prepare:inputColor(槽 0 SRV,BGRA8)box 平均下采样写 ffxInput
    // (49,R8G8B8A8 OF extent;typed SRV 返回逻辑 RGBA,直写无需换序)。
    // 录在 FFX 会话 copy CL(postCopy 之后、门内);调用方负责 ffxInput 的
    // UAV 态屏障。
    void RecordFfxPrepare(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                          uint32_t srcW, uint32_t srcH,
                          uint32_t dstW, uint32_t dstH) noexcept;
    // FFX densify:稀疏流(50,R16G16_SINT,sparseExtent,单位 = OF extent
    // 像素)双线性上采样到 denseW×denseH,Magpie DENSIFY_HLSL 原样;向量
    // 换算 scale = denseExtent/OF extent(源尺寸管线 = 源/OF,follow 内部
    // 管线 = 会话/OF)。录在 FFX 会话 densify CL(postExecute 回调)。
    void RecordFfxDensify(ID3D12GraphicsCommandList &cl, FrameSlot &slot,
                          uint32_t denseW, uint32_t denseH,
                          uint32_t ofW, uint32_t ofH,
                          uint32_t sparseW, uint32_t sparseH,
                          float scaleX, float scaleY,
                          UINT uavMotion, UINT uavConfidence) noexcept;

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
    // post/fg 段提交:waitFence/waitValue = RTX 专用队列的完成栅栏(可选;
    // 排在本次 Execute 之前 —— post CL 消费 VSR/TrueHDR 的输出,跨队列
    // 生产者-消费者顺序由此保证)。提交后 slot->fgFenceValue = 本次栅栏值
    // (计时锚;WaitFrame 目标由后续 postB 或本值决定)。
    bool SubmitFgFrame(FrameSlot &slot, ID3D12Fence *waitFence, uint64_t waitValue,
                       char *err, size_t errLen) noexcept;
    // post 转换段(常驻,全部形态)录制起点与提交:与 fg CL 同型。每帧录制
    // 全部输出转换(真实帧 + 逐 gen;原 hdrPostSplit 的 postB 是其 HDR 特例),
    // 提交 = 本帧最后一次主队列提交(slot->fenceValue 恒 = postFenceValue)。
    // waitFenceA/B = 跨队列生产者栅栏(可选,排队在 Execute 前):legacy HDR
    // 会话 A = hdrDoneFence(B = fgFence);单生产者形态 B 留空。
    bool BeginPostRecording(FrameSlot &slot) noexcept;
    bool SubmitPostFrame(FrameSlot &slot,
                         ID3D12Fence *waitFenceA, uint64_t waitValueA,
                         ID3D12Fence *waitFenceB, uint64_t waitValueB,
                         char *err, size_t errLen) noexcept;
    bool WaitBaseFrame(FrameSlot &slot, char *err, size_t errLen) noexcept;
    bool WaitFrame(FrameSlot &slot, char *err, size_t errLen) noexcept;   // fence wait, device-lost aware
    // base 完成时间戳路径可用性(GetTimestampFrequency 失败 = 0,ProcessFrame
    // 回退阻塞观测;真实 D3D12 驱动上恒可用)。
    bool GpuTsEnabled() const noexcept { return _gpuTsEnabled; }
    double GpuTsFreq() const noexcept { return _gpuTsFreq; }
    // 任意栅栏值的有界 CPU 等待(分段计时锚用;调用方保证 event 不与
    // WaitBase/WaitFrame 的并发使用交错 —— 帧路径内全部串行)。
    bool WaitFenceValuePublic(uint64_t value, HANDLE event, char *err, size_t errLen) noexcept {
        return WaitFenceValue(value, event, err, errLen);
    }
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
    // 差异调试视图(面板"差异调试 ×20",OptiScaler DebugView=3 同语义):
    // |outputColor − inputColor| 逐通道最大差 ×20 的灰度图替换输出。
    // 调用契约(dlssnr_context 帧路径):outputColor 处于 UAV 态(eval/残差/
    // 直通三路帧末一致),inputColor 处于 COMMON;调用后 outputColor 保持
    // UAV(RecordYuvOutput 的 stateBefore 契约不变),inputColor 归 COMMON。
    // _debugDiff 为 context 级共享单纹理:dispatch+copyback 在同一条槽 CL
    // 上原子成对,队列按提交序串行,并发帧的记录对不交错。
    void RecordDebugDiff(FrameSlot &slot) noexcept;
    // 光流场调试视图(面板"调试视图=光流场"):稠密运动场(R16G16_FLOAT,
    // 像素单位)方向→色相(HSV 环,图像坐标 y 向下:红=右、绿=下、青=左、
    // 紫=上),幅值→亮度,
    // 静止/无光流 = 黑。复用 _debugDiff 中转拷回 outputColor(契约同
    // RecordDebugDiff,但 outputColor 无需 NSR 化 —— 本视图不读它)。
    //   realMotion: true = 读真运动场 —— useReduced 决定源尺寸 slot.motion
    //               (NSR,槽 10)或 follow 内部场 slot.reducedMotion(NSR,
    //               槽 18;调用点保证 HasScaling)。状态由 evaluate 消费链
    //               保持,归位仍归 recordGuidancePark(本函数不动)。
    //               false = 绑静态零纹理(槽 35,常驻 NSR):slot.motion
    //               在首 densify 前内容未定义,绑它会把陈旧数据当真流显示
    //               —— 零纹理保证"黑 = 无光流数据"的语义成立,无需屏障。
    //   dispatch 恒按源尺寸,运动场 texel 在 shader 内最近邻映射(follow
    //   半尺寸场放大铺满,_debugDiff 无陈旧边缘)。
    void RecordFlowView(FrameSlot &slot, bool useReduced, bool realMotion) noexcept;

    ID3D12Resource *InputColor(FrameSlot &s) const noexcept { return s.inputColor.Get(); }
    ID3D12Resource *OutputColor(FrameSlot &s) const noexcept { return s.outputColor.Get(); }
    ID3D12Resource *FgInterp(FrameSlot &s, int gen) const noexcept { return s.fgInterp[gen].Get(); }
    // TrueHDR 逐插值帧输出(仅 _fgSlots && _hdrPipe 有纹理)。
    ID3D12Resource *HdrFg(FrameSlot &s, int gen) const noexcept { return s.hdrFg[gen].Get(); }
    // 描述符堆槽位(RecordYuvOutput / FG 转换共用)。
    static constexpr UINT kSrvOutputColor = 22;  // outputColor 的 SRV
    static constexpr UINT kSrvFgInterpBase = 39; // fgInterp[0..4] 的 SRV(FG 槽)
    static constexpr UINT kUavDebugDiff = 34;    // 共享差异调试纹理的 UAV
    static constexpr UINT kSrvVsrColor = 36;     // vsrColor 的 SRV(RTX 输出转换读)
    static constexpr UINT kSrvHdrColor = 37;     // hdrColor 的 SRV(FP16 scRGB → PQ)
    static constexpr UINT kUavMotionDense = 38;  // motionDense 的 UAV(mvec 放大写)
    static constexpr UINT kSrvHdrFgBase = 44;    // hdrFg[0..4] 的 SRV(PQ 转换读)
    static constexpr UINT kUavFgBack = 51;       // fgBack 的 UAV(PQ 域插帧编码 pass 写)
    // YUV 原生化 dump/调试:输出平面([0]=Y [1]=U [2]=V)与位深。
    ID3D12Resource *YuvOutPlane(FrameSlot &s, int plane) const noexcept { return s.yuvOut[plane].Get(); }
    ID3D12Resource *YuvInPlane(FrameSlot &s, int plane) const noexcept { return s.yuvIn[plane].Get(); }
    int BitDepth() const noexcept { return _bitDepth; }
    // 输入色度平面尺寸(subW/H 派生;dump/校验侧按真实布局读,勿再自推)。
    int ChromaWidth() const noexcept { return _chromaW; }
    int ChromaHeight() const noexcept { return _chromaH; }
    // 输出色度平面几何(OUT 尺寸;RTX VSR 会话 yuvOut 在目标尺寸,与源不同)。
    int OutChromaWidth() const noexcept { return _outChromaW; }
    int OutChromaHeight() const noexcept { return _outChromaH; }
    bool IsRgb() const noexcept { return _isRgb; }
    // 管线色缓冲格式:>8bit 且无 RTX = RGBA16F(NR 全程 10bit);RTX 会话
    // BGRA8(TrueHDR 拒 FP16,否决制);VSDLSSNR_NR_FORMAT=fp16/bgra8 强制
    // 覆盖。dump 侧必须与资源一致(CopyTextureRegion 跨格式 E_INVALIDARG)。
    DXGI_FORMAT ColorFormat() const noexcept { return _inColorFmt; }
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
    // PIPE 尺寸零深度(FG + VSR 放大;仅在需要时创建,否则空 —— 调用方
    // 以 motionDense 的有无判别同一条件,勿单独判空)。
    ID3D12Resource *DepthPipe() const noexcept { return _depthPipe.Get(); }

private:
    bool CreateSlotResources(FrameSlot &slot, int depth, char *err, size_t errLen) noexcept;
    bool CreateScalingForSlot(FrameSlot &slot, int iw, int ih, char *err, size_t errLen) noexcept;
    void ClearScalingForSlot(FrameSlot &slot) noexcept;
    // FG 槽纹理/回读缓冲/描述符(仅 _fgSlots);CreateSlotResources 尾部调用。
    bool CreateFgSlotResources(FrameSlot &slot, char *err, size_t errLen) noexcept;
    bool WaitFenceValue(uint64_t value, HANDLE event, char *err, size_t errLen) noexcept;
    // 微型括号 CL:Reset(含 force-close 自愈)→ EndQuery(tsIdx) →
    // ResolveQueryData 写 readback 第 tsIdx 槽(字节偏移 tsIdx*8)→ Close。
    // 调用方负责在 _submitMutex 内 ECL(队列序紧贴被测段)。失败仅放弃本帧
    // 该括号(Finish 回退旧栅栏差分账目),不致命。
    bool RecordTsBracket(FrameSlot &slot, ID3D12CommandAllocator *alloc,
                         ID3D12GraphicsCommandList *cl, UINT tsIdx) noexcept;
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
    // 管线色格式策略(实例:依赖 _bitDepth;allowFp16 = !hdr && !vsr)。
    DXGI_FORMAT NrColorFormat(bool allowFp16) const noexcept;
    bool CreateComputeObjects(char *err, size_t errLen) noexcept;
    void SetErr(char *err, size_t errLen, HRESULT hr, const char *what) const noexcept;

    ComPtr<ID3D12Device> _device;
    ComPtr<IDXGIAdapter1> _adapter; // the adapter the device was created on
    UINT _vendorId = 0;             // 该适配器的 PCI VendorId(0x10DE=NVIDIA,0x1002=AMD;nvof 厂商门用)
    ComPtr<ID3D12InfoQueue> _infoQueue;
    bool _debug = false;
    ComPtr<ID3D12CommandQueue> _queue;
    ComPtr<ID3D12Fence> _fence;
    // GPU 时间戳频率(Hz;base 完成时间戳路径);0 = 不可用。
    double _gpuTsFreq = 0.0;
    bool _gpuTsEnabled = false;
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
    int _bitDepth = 0;  // YUV 位深(8/10/12/14/16;不叫 _depth —— 那是零 guidance 深度纹理)
    int _subW = 1;      // 输入色度抽取档(1=半,0=全):420=(1,1) 422=(1,0) 444=(0,0)
    int _subH = 1;
    bool _isRgb = false; // VS RGBP 直读(零矩阵;平面序 G/B/R)
    int _chromaW = 0;   // 色度平面尺寸(subW ? (w+1)>>1 : w,输入/源)
    int _chromaH = 0;
    // RTX Video 双尺寸(create-time 定格;无 RTX = 与源一致)。
    //   pipe  — VSR 输出 / TrueHDR / FG backbuffer
    //   out   — YUV 输出平面(hdr 开 = 恒 P10:格式 R16_UNORM、2 字节平面)
    int _pipeW = 0;
    int _pipeH = 0;
    int _outW = 0;
    int _outH = 0;
    int _outChromaW = 0;
    int _outChromaH = 0;
    bool _vsrSlots = false;         // 槽池含 vsrColor / motionDense
    bool _hdrPipe = false;          // TrueHDR 激活(hdrColor/hdrFg FP16 / P10 输出)
    bool _fgHdrInterp = false;      // 实验性补帧 HDR 域插帧(fgInterp FP16 / hdrFg 不建)
    DXGI_FORMAT _inColorFmt = DXGI_FORMAT_B8G8R8A8_UNORM; // 管线色(NR input/output 缓冲)
    DXGI_FORMAT _outFmt = DXGI_FORMAT_R8_UNORM;      // yuvOut/readback 平面格式
    UINT _outPlaneBytes = 1;

    // shared read-only resources
    ComPtr<ID3D12Resource> _motion; // resident NON_PIXEL_SHADER_RESOURCE
    ComPtr<ID3D12Resource> _depth;  // resident NON_PIXEL_SHADER_RESOURCE
    // RTX Video + FG 且 PIPE≠src 时的 PIPE 尺寸零深度(FG Depth 子矩形 =
    // backbuffer 尺寸;与 _depth 同为常驻 NSR 零纹理,仅在需要时创建)。
    ComPtr<ID3D12Resource> _depthPipe; // resident NON_PIXEL_SHADER_RESOURCE
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
    // 差异调试视图:2 SRV(input/output)+ 1 UAV(debugDiff)+ 4 常量
    // (extent 2 + 放大系数 1 + pad 1)。
    ComPtr<ID3D12RootSignature> _rsDebugDiff;
    ComPtr<ID3D12PipelineState> _psoDebugDiff;
    // 光流场调试视图:1 SRV(motion 或 reducedMotion)+ 1 UAV(debugDiff)
    // + 4 常量(extent 2 + 幅值→亮度比例 1 + pad 1)。
    ComPtr<ID3D12RootSignature> _rsFlowView;
    ComPtr<ID3D12PipelineState> _psoFlowView;
    // 差异调试中间纹理(W×H BGRA8,UAV):dispatch 写差值 → 同 CL 拷回
    // outputColor(RGBA8 无 UAV load,读写同纹理非法,必须中转)。
    ComPtr<ID3D12Resource> _debugDiff;
    // YUV↔RGB 转换(YUV 原生化):深度/矩阵/范围全走 root constants,
    // R8/R16_UNORM 的 Texture2D<float> 视图同构 —— 仅 3 个 PSO:
    // convertIn(Y/U/V 3 SRV → inputColor 1 UAV,8 常量);
    // convertOut luma/chroma(outputColor 1 SRV → yuvOut 2 UAV——luma 用
    // u0、chroma 用 u0/u1,共享根签名,10 常量:系数 4 + 范围 4 + 尺寸 2)。
    ComPtr<ID3D12RootSignature> _rsConvertIn;
    ComPtr<ID3D12PipelineState> _psoConvertIn;
    ComPtr<ID3D12PipelineState> _psoConvertInRgb; // VS RGBP 直读(零矩阵)
    ComPtr<ID3D12PipelineState> _psoRgbOut;       // RGBP 直写(u0/u1/u2 = G/B/R)
    ComPtr<ID3D12PipelineState> _psoRgbScaled;    // RGBP RTX 缩放版
    ComPtr<ID3D12RootSignature> _rsConvertOut;
    ComPtr<ID3D12PipelineState> _psoConvertOutLuma;
    ComPtr<ID3D12PipelineState> _psoConvertOutChroma;
    // RTX Video 输出转换(PIPE→OUT;与 _rsConvertOut 同根签名布局 ——
    // 12 常量 + t0 + u0/u1,仅换 PSO):
    //   Scaled — BGRA8 源归一化 uv 双线性缩放(VSR-only 路径)
    //   PQ     — FP16 scRGB → 2020/PQ → P10(TrueHDR 路径)
    ComPtr<ID3D12PipelineState> _psoConvertScaledLuma;
    ComPtr<ID3D12PipelineState> _psoConvertScaledChroma;
    ComPtr<ID3D12PipelineState> _psoPqLuma;
    ComPtr<ID3D12PipelineState> _psoPqChroma;
    // PQ 域插帧(fgHdrInterp):scRGB→码域编码(HdrToPq)+ 码域→P10
    // (PqCodesToYuv*,码直读免逐像素 PqEncode)。
    ComPtr<ID3D12PipelineState> _psoHdrToPq;
    ComPtr<ID3D12PipelineState> _psoPqCodesLuma;
    ComPtr<ID3D12PipelineState> _psoPqCodesChroma;
    // mvec 放大(源→PIPE;CreateOfPso 通用形态:1 SRV + 1 UAV + 4 常量)。
    ComPtr<ID3D12RootSignature> _rsMotionScale;
    ComPtr<ID3D12PipelineState> _psoMotionScale;

    // AMD 光流后端 PSO(FFX Prepare/Densify;录制在各自会话 CL 上,
    // d3d12_context.cpp 内嵌 HLSL 同款编译)。
    ComPtr<ID3D12RootSignature> _rsFfxPrepare;
    ComPtr<ID3D12PipelineState> _psoFfxPrepare;
    ComPtr<ID3D12RootSignature> _rsFfxDensify;
    ComPtr<ID3D12PipelineState> _psoFfxDensify;

    // frame slot pool
    static constexpr int kSlotCount = 3;
    FrameSlot _slots[kSlotCount];
    int _freeStack[kSlotCount];
    int _freeCount = 0;
    bool _drain = false; // PoolHold 排他排空:置位后 AcquireSlot 阻塞,防
                         // 归还的槽被后续帧请求偷走(否则 PoolHold 等三槽
                         // 全空会被 mpv 追赶期的连续请求饿死,2026-09-25
                         // 真机 1.5s 排空实锤)
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
