#pragma once
// 计时留痕/探针开关的前置声明收拢(2026-10-05):本体在 dlssnr_context.cpp
// (TimingStatusLine = TimingLog 包装;ProbeEnabled = VSDLSSNR_PROBE 门)。
// dlssnr_context.h 拖 d3d12_context.h + nvsdk_ngx.h,轻 TU 不值得 —— 此前
// 4 个拆分 TU + of_backend.h 各手抄一份声明,签名漂移只能到链接期才爆。
// 重 TU(d3d12_context.cpp 等各 context)照旧 include dlssnr_context.h。
namespace vsdlssnr {

void TimingStatusLine(const char *line) noexcept;
bool ProbeEnabled() noexcept;

} // namespace vsdlssnr
