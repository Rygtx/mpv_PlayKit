#pragma once
// NGX NR 模型选档(2026-10-04 自 plugin.cpp 拆出):按 GPU 系列(arch+impl
// 联合判断,跨族撞号)在 ngx\ 下挑社区改版,原版无条件兜底。首拍定格 +
// 可信度自检见实现注释。与 VS 滤镜生命周期零耦合 —— plugin.cpp 只在
// create 时调一次。
#include <filesystem>
#include <string>

namespace vsdlssnr {

// 返回选中的模型 dll 绝对路径(原版/变体;全缺也走原版名,由下游
// LoadLibrary 失败走既有 passthrough)。NGX core 的 app dir 取
// dll.parent_path(),与选择无关。
std::wstring SelectNgxDllVariant(const std::filesystem::path &ngxDir);

// 原版模型 dll 名(部署契约名,plugin.cpp 默认路径解析同源)。
inline constexpr char NGX_SNIPPET_DLL_NAME[] = "nvngx_dlssnr.dll";

} // namespace vsdlssnr
