// dlssnr 面板全部 ImGui(2026-10-03 自 panel_app.cpp 拆出):DrawUi 头尾
// (标题栏/遥测带/状态带/全局行/处理用时)+ 四个页签函数。页签间共享的
// 双列布局件(原 DrawUi 内 5 个 lambda)收拢为 UiCtx:方法体逐字迁移,
// 页签函数经本地引用别名保持正文零改名(仅 lambda 调用点加 ui. 前缀)。

#include "panel_shared.h"
#include "fonts.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// 参数区双列几何(96dpi 基准):滑块/下拉统一轨道宽 200(kTrackW,
// 96dpi 下 160px,精调所需的最短长度),列间留白 24(kColGap,右列标签
// 不贴左列控件)。
inline constexpr float kTrackW = 200.0f;
inline constexpr float kColGap = 24.0f;

// clang-format off
// Labels/tips are UTF-8 (the project compiles with /utf-8); the old
// wchar_t tables + per-frame WideCharToMultiByte are gone.
// 成员指针直达字段,避免键名 -> 字段的双份 if 链漂移;滑块范围同样直接
// 引用 dlssnr_params.h 的常量(文档注释里的 0-1 等只是给用户看的)。
constexpr struct { const char *key; const char *label; const char *tip;
                   float lo, hi; float DlssnrParams::*field; } kFineSliders[] = {
    { "residual_saturation", "残差饱和度", "DLSSNR 饱和度变化的倍率:1=保持,0=移除(默认 1)。", kResidualFineMin, kResidualFineMax, &DlssnrParams::residualSaturation },
    { "residual_lightness",  "残差亮度",   "DLSSNR 明度变化的倍率:1=保持,0=移除(默认 1)。", kResidualFineMin, kResidualFineMax, &DlssnrParams::residualLightness },
    { "shadow_structure",    "阴影结构",   "暗部分量倍率:调低减暗部噪点,调高增强暗部结构(默认 1)。", kResidualFineMin, kResidualFineMax, &DlssnrParams::shadowStructureMultiplier },
    { "reflection_glow",     "反射辉光",   "亮部分量倍率:调低抑制高光泛光,调高增强辉光(默认 1)。", kResidualFineMin, kResidualFineMax, &DlssnrParams::reflectionGlowMultiplier },
};
constexpr const char *kStyleNames[] = { "0(默认)", "1(自然)", "2(电影)" };
// 光流后端(0=ffx 1=nvof)
constexpr const char *kOfBackendNames[] = { "FFX (AMD 光流,默认)", "NVOF (NVIDIA 引擎)" };
// FG 路由(0=自动 1=纯官方)
constexpr const char *kFgRouteNames[] = { "自动 (预载 0.3.x 代理)", "纯官方 (不预载)" };
// 光流质量(上游 motionVectorQuality 0-5,文案对齐上游 resw)
constexpr const char *kOfQualityNames[] = {
    "无", "性能", "均衡(推荐)", "质量", "高质量(高开销)", "最高质量(极高开销)"
};
// FFX 质量档位(独立三档,与上游 6 档解耦:1 = Performance 半分辨率,
// 2 = Quality 全分辨率 —— 上游 1-2/3-5 在 FFX 内各只对应一种行为)
constexpr const char *kFfxQualityNames[] = { "无", "性能 (1/2 分辨率)", "质量 (全分辨率,默认)" };
// 抗闪烁时域稳定器(上游 antiFlicker 0-4)
constexpr const char *kAntiFlickerNames[] = {
    "无", "静态累积", "光流累积", "光流累积+", "低频时域重建"
};
// 预设/风格/光流质量/各滑块/开关原以 kEnums/kSliders/kFlags 成员指针表
// 驱动通用循环;两列归并后每行控件异构(组合/滑块/复选框/整行),改为
// DrawUi 内联 + pairLabel/pairCombo/pairSlider lambda,tip 随行内联。
// clang-format on

// Tooltip with automatic line wrap (long Chinese hints would clip otherwise)
void ShowTip(const char *u8tip) noexcept {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 16.0f);
    ImGui::TextUnformatted(u8tip);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// StatsPayload.fgRouteEff → 中文标签(状态带与诊断页共用;off/空/未知 = 不显示)
const char *FgRouteLabel(const char *v) noexcept {
    if (!v || !v[0]) return "";
    if (std::strcmp(v, "official-hook") == 0) return "官方 NGX(0.3.x 代理接管)";
    if (std::strcmp(v, "official") == 0) return "官方 NGX 直连";
    if (std::strcmp(v, "copy") == 0) return "未生效(复制帧)";
    return "";
}


// 不一致红色(请求了但没在跑):状态带 / 帧生成页 / 诊断页共用。
inline const ImVec4 kErrRed(1.0f, 0.42f, 0.42f, 1.0f);

// 变长诊断串(state_detail/of_detail/rtx_detail 等,生产端上限 200 字节)在
// 固定 648 窗宽下会被 ImGui 默认的"永不换行"在右缘裁掉 —— 一律经此换行;
// 窗口高度每帧自适应(DrawUi 收口段),换行不丢内容。
void TextDisabledWrapped(const char *s) noexcept {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", s);
    ImGui::PopTextWrapPos();
}

// 同上,彩色版(诊断页"请求 vs 实际"行:格式串 + 变长 detail)。
void TextColoredWrapped(const ImVec4 &col, const char *fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColoredV(col, fmt, args);
    ImGui::PopTextWrapPos();
    va_end(args);
}
// 常规信息灰(状态带 FG 行/诊断页取值列)
inline const ImVec4 kDimTxt(0.62f, 0.64f, 0.70f, 1.0f);

// ---------------------------------------------------------------------------
// 双列布局件(原 DrawUi 内 lambda;方法体逐字迁移,捕获名 → 成员名):
// y 推进与布局常量随 UiCtx 走,页签函数本地别名后正文零改名。
// ---------------------------------------------------------------------------
struct UiCtx {
    // DrawUi 头部算好的布局常量(值语义,页签只读);y = 行推进游标。
    float s = 1, marginX = 0, colCtrl = 0, trackW = 0;
    float halfW = 0, colGap = 0, pairLabelW = 0, labelDy = 0, rowH = 0;
    ImVec2 wpos{};
    float y = 0;

    // Label 与控件垂直居中:控件文本在 frame 内偏移 FramePadding.y,标签
    // 按同一中心线对齐。旧的硬编码 +7/+8*s 在高 DPI 下漂移(字体随 s 放大,
    // FramePadding 固定不放大),表现为参数名与滑块错位。
    void pairLabel(int col, const char *label, const char *tip) {
        ImGui::SetCursorScreenPos(
            ImVec2(wpos.x + marginX + col * (halfW + colGap), wpos.y + y + labelDy));
        ImGui::TextUnformatted(label);
        if (tip && ImGui::IsItemHovered()) ShowTip(tip);
    }
    void pairCombo(const char *key, int DlssnrParams::*f, int count,
                   const char *const *names, int col) {
        ImGui::SetCursorScreenPos(
            ImVec2(wpos.x + marginX + col * (halfW + colGap) + pairLabelW, wpos.y + y));
        ImGui::SetNextItemWidth(trackW);
        int v = g_app.params.*f;
        if (ImGui::Combo((std::string("##") + key).c_str(), &v, names, count)) {
            g_app.params.*f = v;
            g_app.liveDirty = true;
        }
    }
    void pairSlider(const char *key, float DlssnrParams::*f, float lo, float hi, int col) {
        ImGui::SetCursorScreenPos(
            ImVec2(wpos.x + marginX + col * (halfW + colGap) + pairLabelW, wpos.y + y));
        ImGui::SetNextItemWidth(trackW);
        float v = g_app.params.*f;
        if (ImGui::SliderFloat((std::string("##") + key).c_str(), &v, lo, hi, "%.2f")) {
            // NGX accepts continuous float steps (verified: 0.01 steps produce
            // distinct outputs), so no snapping to Magpie's UI-level 0.05 grid.
            g_app.params.*f = v;
            g_app.liveDirty = true;
        }
    }
    // 半格复选框(标签 + 隐名复选框,与滑块行同款对齐)
    void pairCheck(const char *key, int DlssnrParams::*f, int col) {
        ImGui::SetCursorScreenPos(
            ImVec2(wpos.x + marginX + col * (halfW + colGap) + pairLabelW, wpos.y + y));
        bool v = g_app.params.*f != 0;
        if (ImGui::Checkbox((std::string("##") + key).c_str(), &v)) {
            g_app.params.*f = v ? 1 : 0;
            g_app.liveDirty = true;
        }
    }
    // 页签选择记忆落 ini([panel] page),与 advancedOpen 同款。
    void savePagePref(int p) {
        g_app.page = p;
        wchar_t base[MAX_PATH], path[MAX_PATH], val[8];
        if (BasePath(base, MAX_PATH)) {
            swprintf_s(path, MAX_PATH, L"%s\\%s", base, INI_FILE);
            swprintf_s(val, 8, L"%d", p);
            WritePrivateProfileStringW(L"panel", L"page", val, path);
        }
    }
};

// ---- 页:DrawNrPage ----
void DrawNrPage(UiCtx &ui) noexcept {
    // 本地引用别名:页签正文自 DrawUi 原样迁移,裸名直连 UiCtx。
    const float s = ui.s, marginX = ui.marginX, colCtrl = ui.colCtrl,
                trackW = ui.trackW, pairLabelW = ui.pairLabelW,
                labelDy = ui.labelDy, rowH = ui.rowH;
        const ImVec2 &wpos = ui.wpos;
        float &y = ui.y;
        // 神经渲染总开关(整行;live 即时,只关 NR 不影响补帧/光流)
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y + labelDy));
    ImGui::TextUnformatted("神经渲染");
    if (ImGui::IsItemHovered())
        ShowTip("关闭 = 跳过处理,补帧/光流/RTX 照常;全关时滤镜零开销。面板设置优先于 vpy 脚本。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + colCtrl, wpos.y + y));
    {
        bool v = g_app.params.nrEnabled != 0;
        if (ImGui::Checkbox("##nr_enabled", &v)) {
            // passthrough = 本实例没有会话(全关 create / init 失败 / 死亡
            // 态),开 NR 无法 live 恢复 —— 需要重建,自动触发原地 seek。
            // live 会话(含 NR 已关)恒 filterState="ok"("NR off (panel)"
            // 边沿体已随解耦删除),走 live 门即时生效。
            const bool needsReseek =
                v && strcmp(g_app.snap.filterState, "passthrough") == 0;
            g_app.params.nrEnabled = v ? 1 : 0;
            g_app.liveDirty = true;
            if (needsReseek) g_app.reseekDirty = true;
        }
    }
    y += rowH;

    // (风格 | 光流后端):两个模式下拉并排。预设下拉已移除(2026-09-14)
    // —— 310.9 DLL 不消费 DLSSNR.Hint.Render.Preset,A/B 实测输出恒等
    // (死旋钮);上游 r2-fix1 同款处置。preset 参数链(vpy/ini/IPC/NGX
    // 写入)保留:vpy 是 validate_params 的 preset 哨兵断言测试通道,新 DLL
    // 激活 preset 时(哨兵变 DIFF)面板加回下拉即可。
    // 光流后端为创建时参数:切档触发 OF 会话原位重建,下一帧生效,无需
    // mpv 重启;显式档失败不跨后端回落。
    ui.pairLabel(0, "风格", nullptr);
    ui.pairCombo("style", &DlssnrParams::style, 3, kStyleNames, 0);
    ui.pairLabel(1, "光流后端", "FFX = AMD 光流,通用;NVOF = NVIDIA 专属,备选。\n"
              "切档下一帧生效,质量档位选项随之变化。");
    ui.pairCombo("of_backend", &DlssnrParams::ofBackend, 2, kOfBackendNames, 1);
    y += rowH;

    // (光流质量 | 光流降采样):光流组。质量选项随后端变化(单下拉双字段:
    // NVOF→motionVectorQuality 0-5 六档;FFX→ffxQuality 0-2 三档)。
    {
        const int ofBackend = std::clamp(g_app.params.ofBackend, kOfBackendMin, kOfBackendMax);
        const char *ofQualityTip =
            ofBackend == kOfBackendFfx
                ? nullptr
                : "档位越高越精确也越耗时;不支持时自动回退\"无\"。";
        ui.pairLabel(0, "光流质量", ofQualityTip);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
        ImGui::SetNextItemWidth(trackW);
        bool changed = false;
        if (ofBackend == kOfBackendFfx) {
            int sel = std::clamp(g_app.params.ffxQuality, kFfxQualityMin, kFfxQualityMax);
            if (ImGui::Combo("##motion_vector_quality", &sel, kFfxQualityNames, 3)) {
                g_app.params.ffxQuality = sel;
                changed = true;
            }
        } else { // nvof:上游 0-5
            int v = g_app.params.motionVectorQuality;
            if (ImGui::Combo("##motion_vector_quality", &v, kOfQualityNames, 6)) {
                g_app.params.motionVectorQuality = v;
                changed = true;
            }
        }
        if (changed) g_app.liveDirty = true;
    }
    ui.pairLabel(1, "光流降采样", "按缩放后的分辨率算光流,省时但精度略降;未开缩放或开帧生成时无效。");
    ui.pairCheck("nvof_follow_scaling", &DlssnrParams::nvofFollowScaling, 1);
    y += rowH;

    // (抗闪烁 | —):时域稳定器。NR 对逐帧随机噪声的响应强度不同 =
    // 画面闪烁;本器把"NR 改动量"沿时间轴平均掉。档位即"怎么平均"。
    ui.pairLabel(0, "抗闪烁",
              "跨帧平均,消除 NR 的逐帧闪烁(强度调高后易出现)。\n"
              "静态累积:假设画面不动,固定镜头用,不会拖影。\n"
              "光流累积:按运动对齐再平均,动态画面首选。\n"
              "光流累积+:加通断记忆,治噪点忽闪忽灭,最强也最激进。\n"
              "低频时域重建:只平滑大面积慢闪,不动细节,最贵。\n"
              "2/3 档需光流质量非\"无\";切档立即生效。");
    ui.pairCombo("anti_flicker", &DlssnrParams::antiFlicker, 5, kAntiFlickerNames, 0);
    y += rowH;

    // (强度 | 局部色调)
    ui.pairLabel(0, "强度", "越高处理越明显(默认 1)。");
    ui.pairSlider("intensity", &DlssnrParams::intensity, kStrengthMin, kStrengthMax, 0);
    ui.pairLabel(1, "局部色调", "明暗过渡区域的处理力度(默认 1)。");
    ui.pairSlider("local_tone", &DlssnrParams::localToneStrength, kStrengthMin, kStrengthMax, 1);
    y += rowH;

    // (局部结构 | 皮肤结构)
    ui.pairLabel(0, "局部结构", "越高保留越多细节纹理(默认 1)。");
    ui.pairSlider("local_structure", &DlssnrParams::localStructureStrength, kStrengthMin, kStrengthMax, 0);
    ui.pairLabel(1, "皮肤结构", "人物皮肤区域的细节保留(默认 0)。");
    ui.pairSlider("skin_structure", &DlssnrParams::skinStructureStrength, kSkinMin, kSkinMax, 1);
    y += rowH;

    // (分辨率缩放 | 残差乘数):缩放总开关独占标准控件位,不与滑杆同行
    // 挤占 —— 开关前置会把 %滑杆推出轨道列(与其他滑块左缘不齐)。
    ui.pairLabel(0, "分辨率缩放",
              "按缩小的分辨率推理再重建回源,省性能。");
    ui.pairCheck("scaling_enabled", &DlssnrParams::scalingEnabled, 0);
    ui.pairLabel(1, "残差乘数", "缩放重建时细节的增强倍率(默认 1)。");
    ui.pairSlider("residual_multiplier", &DlssnrParams::residualMultiplier,
               kResidualMultMin, kResidualMultMax, 1);
    y += rowH;

    // (缩放比例 | 自动蒙版):%滑杆占标准控件位(与全局轨道对齐),缩放
    // 关闭时置灰。百分比改动会短暂重建模型(毫秒级),松手才推送(live)。
    ui.pairLabel(0, "缩放比例",
              "推理分辨率占源分辨率的比例;缩放关闭时无效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const bool scalingOn = g_app.params.scalingEnabled != 0;
        if (!scalingOn) ImGui::BeginDisabled(true);
        int ir = g_app.params.inputResolutionPercent;
        if (ImGui::SliderInt("##input_resolution", &ir, kResPctMin, kResPctMax, "%d%%")) {
            g_app.params.inputResolutionPercent = std::clamp(ir, kResPctMin, kResPctMax);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // Debounce: rebuilding per drag tick would recreate the feature +
            // textures every frame; push once on slider release instead.
            g_app.liveDirty = true;
        }
        if (!scalingOn) ImGui::EndDisabled();
    }
    ui.pairLabel(1, "自动蒙版", "由模型自动识别画面中需区别处理的区域,无需手动指定。");
    ui.pairCheck("auto_mask", &DlssnrParams::useAutoMask, 1);
    y += rowH;

    // 精调四条两两并排(与主强度滑杆同款轨道宽)
    auto drawSliderRows = [&](const auto &table) {
        const int count = static_cast<int>(sizeof(table) / sizeof(table[0]));
        for (int i = 0; i < count; i += 2) {
            ui.pairLabel(0, table[i].label, table[i].tip);
            ui.pairSlider(table[i].key, table[i].field, table[i].lo, table[i].hi, 0);
            if (i + 1 < count) {
                ui.pairLabel(1, table[i + 1].label, table[i + 1].tip);
                ui.pairSlider(table[i + 1].key, table[i + 1].field,
                           table[i + 1].lo, table[i + 1].hi, 1);
            }
            y += rowH;
        }
    };

    // 残差精调(高级):默认收起;展开状态记忆在 ini [panel] advanced。
    // SetNextItemOpen 把 ini 值喂给 ImGui 的内部开合存储——CollapsingHeader
    // 的状态在 ImGui 自己的 storage 里,不播种的话首帧永远读到"收起",还会
    // 把 ini 值回写成 0。
    ImGui::SetNextItemOpen(g_app.advancedOpen, ImGuiCond_Once);
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    const bool advanced = ImGui::CollapsingHeader("残差精调(高级)");
    if (ImGui::IsItemHovered())
        ShowTip("1 = 保持 DLSSNR 原变化,一般无需调整。");
    if (advanced != g_app.advancedOpen) {
        g_app.advancedOpen = advanced;
        wchar_t base[MAX_PATH], path[MAX_PATH];
        if (BasePath(base, MAX_PATH)) {
            swprintf_s(path, MAX_PATH, L"%s\\%s", base, INI_FILE);
            WritePrivateProfileStringW(L"panel", L"advanced", advanced ? L"1" : L"0", path);
        }
    }
    // 用实际渲染高度推进 y(标题条高度随字体/DPI 变化,硬编码预算会和
    // 下一个区块贴死或重叠)。
    y = ImGui::GetItemRectMax().y - wpos.y + 8 * s;
    if (advanced) drawSliderRows(kFineSliders);
}

// ---- 页:DrawFgPage ----
void DrawFgPage(UiCtx &ui) noexcept {
    // 本地引用别名:页签正文自 DrawUi 原样迁移,裸名直连 UiCtx。
    const float marginX = ui.marginX, trackW = ui.trackW,
                pairLabelW = ui.pairLabelW, rowH = ui.rowH;
        const ImVec2 &wpos = ui.wpos;
        float &y = ui.y;
        // (帧生成 | FG 路由):档位与路由都是创建期参数并排 —— 改档位触发
    // mpv 重载,路由进程级重启生效。倍数选择 关/2x/3x/4x/5x/6x,同步
    // fgEnabled + fgMultiplier 两键。
    ui.pairLabel(0, "帧生成", "输出帧率 = 源 ×2–×6。需光流质量非\"无\",否则只复制帧;失败自动回退 1:1。\n"
              "显示端刷新率建议不低于输出帧率。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        // 运行库上限 MaxGeneratedFrames 由部署 ini 常开到 5(fetch-deps 归一),
        // 倍数选择无需触碰代理 ini —— 改档原地重载即时生效。
        const int items = kFgMultMax; // 关 + 2x..6x
        const char *labels[items] = { "关", "2x", "3x", "4x", "5x", "6x" };
        int sel = g_app.params.fgEnabled ? std::clamp(g_app.params.fgMultiplier, kFgMultMin, kFgMultMax) - 1 : 0;
        if (ImGui::Combo("##fg_mode", &sel, labels, items)) {
            if (sel <= 0) {
                g_app.params.fgEnabled = 0;
            } else {
                g_app.params.fgEnabled = 1;
                g_app.params.fgMultiplier = sel + 1; // 2..6
            }
            g_app.liveDirty = true;
            // 输出契约(帧数/节奏)随创建档位定格,任何档位/开关的真变化都
            // 需要 mpv 重建滤镜 —— 自动触发原地 seek(payload 先落,重建的
            // 新实例即采纳);IPC 不可用时插件侧退化为会话内降档复制。
            g_app.reseekDirty = true;
        }
    }
    ui.pairLabel(1, "FG 路由", "自动 = 预载代理,20/30 系需要;纯官方 = 直连官方运行时,40/50 用。\n"
              "重启 mpv 生效,实际路由见\"诊断\"页。");
    ui.pairCombo("fg_route", &DlssnrParams::fgRoute, 2, kFgRouteNames, 1);
    y += rowH;

    // Optimized 内核档(整行;dlssg_for_sm86 一致性档位。代理只在进程加载
    // 时读一次 ini → 重启 mpv 生效;存储单点 = 代理 ini,面板启动回读)
    ui.pairLabel(0, "内核档位", "1 = 与官方画面完全相同(默认);2/3 更快但画质有损。重启 mpv 生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const int items = 4;
        const char *labels[items] = { "0 原厂", "1 逐位一致 (默认)", "2 有损 (PSNR>50dB)",
                                      "3 全部有损" };
        int sel = std::clamp(g_app.fgOptimized, 0, 3);
        if (ImGui::Combo("##fg_optimized", &sel, labels, items)) {
            if (WriteFgOptimizedIni(sel)) {
                g_app.fgOptimized = sel;
            } else {
                // 写失败(UI 与磁盘静默分叉曾经只进日志):不采纳选择,
                // 下拉弹回现值 + 状态栏说明,重启后"档位自己变回去"不再发生。
                snprintf(g_app.status, sizeof(g_app.status),
                         "内核档位写入失败(代理配置缺失或被占用)");
            }
        }
    }
    y += rowH;

            // 跨页依赖提示:光流是 NR(主消费者)与 FG 的共享资源,设置留在
            // 神经渲染页不复制控件;此处按钮跳转,复用启动页签恢复机制
            //(SetSelected 需逐帧重喂 + 强制重绘,见 pageRestore 注释)。
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
            ImGui::TextDisabled("帧生成需光流质量非\"无\",否则只复制帧。");
            ImGui::SameLine();
            if (ImGui::SmallButton("前往光流设置")) {
                g_app.page = 0;
                g_app.pageRestore = true;
                g_app.restoreFrames = 8;
            }
            y += rowH;
}

// ---- 页:DrawRtxPage ----
void DrawRtxPage(UiCtx &ui) noexcept {
    // 本地引用别名:页签正文自 DrawUi 原样迁移,裸名直连 UiCtx。
    const float s = ui.s, marginX = ui.marginX, trackW = ui.trackW,
                halfW = ui.halfW, colGap = ui.colGap,
                pairLabelW = ui.pairLabelW, rowH = ui.rowH;
        const ImVec2 &wpos = ui.wpos;
        float &y = ui.y;
        // (VSR 超分 | VSR 质量):开关为创建时参数(变化触发 mpv 原地重载);
    // 质量 1-4 为 per-eval live,拖动即时生效(官方 bicubic 0 档不暴露 ——
    // 关闭走开关,不再付一份 GPU 价买双线性)。
    ui.pairLabel(0, "VSR 超分", "RTX Video AI 超分,只做放大;1:1/缩小时自动旁路。\n"
              "需 ngx\\ 下有 nvngx_vsr.dll。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    {
        // 勾选框记忆:非零模式恒同步进 lastVsrMode(ini 载入/payload
        // adopt/下拉切换任何来源都经过这里,每帧校对免多点维护),重新
        // 勾选恢复它 —— 此前恒落回自动,手动倍率选择被静默丢弃(勾回后
        // VSR 在 4K 窗口旁路,用户以为还开着,2026-09-26)。
        if (g_app.params.rtxVsrMode != 0) g_app.lastVsrMode = g_app.params.rtxVsrMode;
        bool vsrOn = g_app.params.rtxVsrMode != 0;
        if (ImGui::Checkbox("##rtx_vsr_enabled", &vsrOn)) {
            g_app.params.rtxVsrMode = vsrOn ? g_app.lastVsrMode : 0;
            g_app.liveDirty = true;
            // 槽资源几何(vsrColor/yuvOut 尺寸/输出格式)随创建定格,开关
            // 真变化只能链重建 —— 与帧生成开关同款 reseek 语义。
            g_app.reseekDirty = true;
        }
    }
    ui.pairLabel(1, "VSR 质量", "数字越大越精细也越慢。拖动下一帧生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const bool vsrOn = g_app.params.rtxVsrMode != 0;
        if (!vsrOn) ImGui::BeginDisabled(true);
        int q = std::clamp(g_app.params.rtxVsrStrength, kVsrStrengthMin, kVsrStrengthMax);
        if (ImGui::SliderInt("##vsr_strength", &q, kVsrStrengthMin, kVsrStrengthMax, "%d")) {
            g_app.params.rtxVsrStrength = q;
            g_app.liveDirty = true;
        }
        if (!vsrOn) ImGui::EndDisabled();
    }
    y += rowH;

    // (模式 | RTX Video HDR):模式与 HDR 开关都是创建时参数并排。
    ui.pairLabel(0, "模式", "自动 = 跟随窗口大小;手动 = 按下方倍率固定放大。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    ImGui::SetNextItemWidth(trackW);
    {
        const bool vsrOn = g_app.params.rtxVsrMode != 0;
        if (!vsrOn) ImGui::BeginDisabled(true);
        static const char *kVsrModeNames[] = { "自动 (窗口适配)", "手动 (倍率)" };
        // 显示索引 = mode-1(mode 1=自动→item 0,2=手动→item 1;开态可达,
        // 无 0 档)。曾用原始 mode 作索引:自动(1) 显示成 items[1]="手动",
        // 点自动后下拉弹回"手动"(mode 实际已切对,纯显示错位,真机实锤)。
        int sel = std::clamp(g_app.params.rtxVsrMode, 1, kVsrModeMax) - 1;
        if (ImGui::Combo("##vsr_mode", &sel, kVsrModeNames, 2)) {
            g_app.params.rtxVsrMode = std::clamp(sel + 1, 1, kVsrModeMax);
            g_app.liveDirty = true;
            g_app.reseekDirty = true;
        }
        if (!vsrOn) ImGui::EndDisabled();
    }
    ui.pairLabel(1, "RTX Video HDR", "SDR → HDR10,输出切 10bit BT.2020;显示切换面板自动完成。\n"
              "需 ngx\\ 下有 nvngx_truehdr.dll。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    {
        bool hdr = g_app.params.rtxHdrEnabled != 0;
        if (ImGui::Checkbox("##rtx_hdr_enabled", &hdr)) {
            g_app.params.rtxHdrEnabled = hdr ? 1 : 0;
            g_app.liveDirty = true;
            g_app.reseekDirty = true; // 输出格式(P8/P10)随创建定格
        }
    }
    y += rowH;

    // (放大倍率 | HDR 对比度):倍率为创建时参数(松手才推送,免拖动连环
    // 重载),模式=手动时启用;对比度为 per-eval live。
    ui.pairLabel(0, "放大倍率", "手动模式的目标倍率,松手后生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    {
        const bool manual = g_app.params.rtxVsrMode == 2;
        if (!manual) ImGui::BeginDisabled(true);
        float sc = std::clamp(g_app.params.rtxVsrScale, kVsrScaleMin, kVsrScaleMax);
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::SliderFloat("##vsr_scale", &sc, kVsrScaleMin, kVsrScaleMax, "%.2fx"))
            g_app.params.rtxVsrScale = sc;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // 松手才推送:拖动过程连续重载会连环重建链(与分辨率缩放 %滑杆
            // 同款 debounce)。检查必须在 SliderFloat 的返回值分支外:松手帧
            // SliderBehavior 先 ClearActiveID 再 return false,该帧返回值恒
            // 为 false,而 IsItemDeactivatedAfterEdit 恰好只在松手帧为真 ——
            // 嵌在里面是死代码,拖完松手 payload 从不落地、重载从不触发。
            g_app.liveDirty = true;
            g_app.reseekDirty = true;
        }
        if (!manual) ImGui::EndDisabled();
    }
    // HDR 四参滑杆共用体(HDR 关时置灰;per-eval)。
    auto hdrSlider = [&](int DlssnrParams::*f, int lo, int hi,
                         const char *id, const char *fmt, bool intMode) {
        const bool hdrOn = g_app.params.rtxHdrEnabled != 0;
        if (!hdrOn) ImGui::BeginDisabled(true);
        ImGui::SetNextItemWidth(trackW);
        if (intMode) {
            int v = std::clamp(g_app.params.*f, lo, hi);
            if (ImGui::SliderInt(id, &v, lo, hi, fmt)) {
                g_app.params.*f = v;
                g_app.liveDirty = true;
            }
        } else {
            float v = static_cast<float>(std::clamp(g_app.params.*f, lo, hi));
            if (ImGui::SliderFloat(id, &v, static_cast<float>(lo),
                                   static_cast<float>(hi), fmt)) {
                g_app.params.*f = static_cast<int>(v + 0.5f);
                g_app.liveDirty = true;
            }
        }
        if (!hdrOn) ImGui::EndDisabled();
    };
    ui.pairLabel(1, "HDR 对比度", "明暗差强度(默认 100),拖动下一帧生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    hdrSlider(&DlssnrParams::rtxHdrContrast, kHdrContrastMin, kHdrContrastMax, "##hdr_contrast", "%.0f", false);
    y += rowH;

    // (HDR 饱和度 | HDR 中间灰)
    ui.pairLabel(0, "HDR 饱和度", "色彩强度(默认 100),拖动下一帧生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    hdrSlider(&DlssnrParams::rtxHdrSaturation, kHdrSaturationMin, kHdrSaturationMax, "##hdr_saturation", "%.0f", false);
    ui.pairLabel(1, "HDR 中间灰", "平均亮度基准(默认 50),拖动下一帧生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + (halfW + colGap) + pairLabelW, wpos.y + y));
    hdrSlider(&DlssnrParams::rtxHdrMiddleGray, kHdrMiddleGrayMin, kHdrMiddleGrayMax, "##hdr_middle_gray", "%.0f", false);
    y += rowH;

    // HDR 峰值亮度(整行;HDR 关时置灰)+ 跨页提示
    ui.pairLabel(0, "HDR 峰值亮度", "填显示器实际峰值亮度,色调映射最准(默认 1000),拖动下一帧生效。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    hdrSlider(&DlssnrParams::rtxHdrMaxLuminance, kHdrMaxLumMin, kHdrMaxLumMax, "##hdr_peak_nits", "%d nits", true);
    y += rowH;

    // 补帧 HDR 域插帧(整行;HDR/FG 关时置灰)+ 代价提示。
    // 标签 ≤5 中文字宽(标签列宽限制,超宽侵入控件列与开关重叠,#53e 同款)。
    ui.pairLabel(0, "HDR域插帧", "插值帧直接在 HDR 域生成,省去每帧一次 HDR 转换;高倍数/高分辨率收益更大。");
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX + pairLabelW, wpos.y + y));
    {
        const bool usable = g_app.params.rtxHdrEnabled != 0 && g_app.params.fgEnabled != 0;
        if (!usable) ImGui::BeginDisabled(true);
        bool hi = g_app.params.fgHdrInterp != 0;
        if (ImGui::Checkbox("##fg_hdr_interp", &hi)) {
            g_app.params.fgHdrInterp = hi ? 1 : 0;
            g_app.liveDirty = true;
            g_app.reseekDirty = true; // 槽资源/FG create 格式随创建定格
        }
        if (!usable) ImGui::EndDisabled();
        ImGui::SameLine(0, 12 * s);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f),
                           usable ? "实验性 —— 画面异常(闪烁/亮度跳变)请关闭"
                                  : "需 HDR 与补帧同时开启");
    }
    y += rowH;

            // 跨页依赖提示
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
            ImGui::TextDisabled("提示: 实际生效状态见\"诊断\"页;HDR 显示切换由面板自动完成,无需改 mpv.conf。");
            y += rowH;
}

// ---- 页:DrawDiagPage ----
void DrawDiagPage(UiCtx &ui) noexcept {
    // 本地引用别名:页签正文自 DrawUi 原样迁移,裸名直连 UiCtx。
    const float s = ui.s, marginX = ui.marginX, colCtrl = ui.colCtrl,
                trackW = ui.trackW;
        const ImVec2 &wpos = ui.wpos;
        float &y = ui.y;
                ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));

            // ---- 调试与日志(从神经渲染页/全局行迁入:监控与诊断类控件
            // 集中在本页,调参页只留调参控件)----
            ImGui::TextUnformatted("—— 调试与日志 ——");
            {
                ImGui::TextUnformatted("调试视图");
                if (ImGui::IsItemHovered())
                    ShowTip("仅当前会话有效,不写入保存设置。\n"
                            "差异 ×20:NR 改动量灰度,白 = 改动大,灰 = 没动画面(需 NR 开)。\n"
                            "光流场:色相 = 方向(红右/绿下/青左/紫上),亮度 = 速度,黑 = 无运动数据。");
                ImGui::SameLine(0, 8 * s);
                // 三态:关 / 差异 ×20 / 光流场(live,不持久化)
                {
                    const int items = 3;
                    const char *labels[items] = { "关", "差异 ×20", "光流场" };
                    int sel = std::clamp(g_app.params.debugView, 0, kDebugViewMax);
                    ImGui::SetNextItemWidth(trackW);
                    if (ImGui::Combo("##debug_view", &sel, labels, items)) {
                        g_app.params.debugView = sel;
                        g_app.liveDirty = true;
                    }
                }
                ImGui::SameLine(0, 24 * s);
                bool logOn = g_app.timingLog;
                if (ImGui::Checkbox("写入性能日志", &logOn)) {
                    g_app.timingLog = logOn;
                    wchar_t iniPath[MAX_PATH];
                    if (BasePath(iniPath, MAX_PATH)) {
                        wchar_t iniFile[MAX_PATH];
                        swprintf_s(iniFile, L"%s\\%s", iniPath, INI_FILE);
                        WritePrivateProfileStringW(L"panel", L"log", logOn ? L"1" : L"0", iniFile);
                    }
                    WritePayload(); // logEnabled included
                    g_app.liveDirty = false;
                }
                if (ImGui::IsItemHovered())
                    ShowTip("每秒记录性能数据到 dlssnr_timing.log,排查问题时附上。");
            }

            ImGui::Spacing();
            // ---- 会话事实:请求 vs 实际(不一致 = 红)----
            ImGui::TextUnformatted("—— 会话 ——");
            {
                // 224 = "直通(画面未增强)" 前缀 + stateDetail 全长(207)+NUL
                char stDesc[224] = "未加载滤镜";
                bool stRed = false;
                if (g_app.snap.gpuName[0]) {
                    if (std::strcmp(g_app.snap.filterState, "passthrough") == 0) {
                        // deliberate = 用户主动全关("NR+FG+RTX disabled");
                        // "NR off (panel)" 边沿体已删。原 "NR+FG disabled" 14
                        // 字符前缀是加 RTX 前的老串,与现串第 5 字节起错位恒
                        // 不命中 —— 全关实例曾被误标红色错误态。
                        const bool deliberate =
                            std::strncmp(g_app.snap.stateDetail, "NR+FG+RTX disabled", 18) == 0;
                        std::snprintf(stDesc, sizeof(stDesc), "直通(画面未增强)%s%s",
                                      g_app.snap.stateDetail[0] ? ": " : "", g_app.snap.stateDetail);
                        stRed = !deliberate;
                    } else if (std::strcmp(g_app.snap.filterState, "ngx_faulted") == 0) {
                        std::snprintf(stDesc, sizeof(stDesc), "NGX 故障已停用%s%s",
                                      g_app.snap.stateDetail[0] ? ": " : "", g_app.snap.stateDetail);
                        stRed = true;
                    } else if (std::strcmp(g_app.snap.filterState, "nvof_zero") == 0) {
                        std::snprintf(stDesc, sizeof(stDesc), "增强中,光流无运动数据");
                        stRed = true;
                    } else {
                        // ok / 空(存活态由周期 tick 携带)。"增强中"直读每帧
                        // 实效位(DSL9):eval = 本帧 NGX 真评估;FG = fgState
                        // "on"(真插值);RTX = 实态串(插件直发)。原按面板
                        // 意图(nrOn/fgOn/rtxOn)推导的镜像已删 —— 插件改门控
                        // 公式时面板静默错标的整类失配由此消灭。
                        const bool evalOn = g_app.snap.evalActive != 0;
                        const bool fgOnFrame = std::strcmp(g_app.snap.fgState, "on") == 0;
                        const bool rtxOn = g_app.snap.rtx[0] &&
                                           std::strcmp(g_app.snap.rtx, "off") != 0;
                        if (evalOn || fgOnFrame || rtxOn) {
                            std::snprintf(stDesc, sizeof(stDesc),
                                          "增强中(NGX 推理运行)");
                        } else {
                            std::snprintf(stDesc, sizeof(stDesc),
                                          "直通(画面未增强)");
                        }
                    }
                }
                TextColoredWrapped(stRed ? kErrRed : kDimTxt, "滤镜状态: %s", stDesc);
                // 实际加载的模型 dll(原版/哪档变体,一眼核对;等价 timing log
                // 的 "ngx model tier=" 行 + "snippet dll" 指纹)。
                ImGui::TextDisabled("模型: %s",
                                    g_app.snap.modelDll[0] ? g_app.snap.modelDll : "(未知)");

                // 请求 vs 实际三列手动网格(参数页 SameLine 网格同款,不用
                // BeginTable:其列宽在此自绘 DPI 体系下不可预期):项目列
                // colCtrl 与参数页控件列对齐,请求列 180s(容下"FFX 质量
                // (全分辨率)"),实际列占余宽,折行 detail 不裁。
                const float reqX = marginX + colCtrl;
                const float actX = reqX + 180 * s;
                ImGui::TextDisabled("项目");
                ImGui::SameLine();
                ImGui::SetCursorPosX(reqX);
                ImGui::TextDisabled("请求");
                ImGui::SameLine();
                ImGui::SetCursorPosX(actX);
                ImGui::TextDisabled("实际");

                // 光流请求 vs 实际
                char ofReq[64];
                if (g_app.params.ofBackend == kOfBackendFfx) {
                    std::snprintf(ofReq, sizeof(ofReq), "FFX %s",
                                  g_app.params.ffxQuality == 0 ? "无"
                                  : g_app.params.ffxQuality == 1 ? "性能(1/2 分辨率)" : "质量(全分辨率)");
                } else {
                    std::snprintf(ofReq, sizeof(ofReq), "NVOF 质量 %d", g_app.params.motionVectorQuality);
                }
                const int ofqReq = std::clamp(g_app.params.ofBackend == kOfBackendFfx
                                                  ? g_app.params.ffxQuality
                                                  : g_app.params.motionVectorQuality,
                                              0, kOfQualityMax);
                const bool ofBroken = std::strcmp(g_app.snap.ofMode, "zero") == 0 && ofqReq > 0;
                // 每帧消费态(DSL9 实效位直读):ofActive = 插件侧消费门
                // (ofNeeded)本帧真值 —— of_mode 是会话级照报,门关帧零提交,
                // 不能当"每帧在算"的证据。"算(NR/FG)"归因仍按意图(消费方
                // 是谁只有参数知道),门开与否交给实效位。
                const bool ofNr = g_app.params.nrEnabled != 0;
                const bool ofFg = g_app.params.fgEnabled != 0 && g_app.params.fgMultiplier > 1;
                const char *ofFrame = ofqReq == 0 ? "质量 0(关)"
                                      : !g_app.snap.ofMode[0] ? "未加载"
                                      : !g_app.snap.ofActive ? "门控跳过(NR/FG 均关,零提交)"
                                      : (ofNr && ofFg) ? "算(NR+FG)"
                                      : ofNr ? "算(NR)" : "算(FG)";
                if (ofBroken) ImGui::TextColored(kErrRed, "光流");
                else ImGui::TextUnformatted("光流");
                ImGui::SameLine();
                ImGui::SetCursorPosX(reqX);
                ImGui::TextUnformatted(ofReq);
                ImGui::SameLine();
                ImGui::SetCursorPosX(actX);
                TextColoredWrapped(ofBroken ? kErrRed : kDimTxt, "%s · 每帧:%s%s%s",
                                   g_app.snap.ofMode[0] ? g_app.snap.ofMode : "(未加载)",
                                   ofFrame,
                                   g_app.snap.ofDetail[0] ? " —— " : "", g_app.snap.ofDetail);

                // 抗闪烁请求 vs 实际:实际档 + 时间线状态 + 最近混合权重。
                // 红显 = 请求开但插件降级(failed),或状态活跃而实档掉 0。
                {
                    const bool afReqOn = g_app.params.antiFlicker > 0;
                    const bool afFailed = std::strcmp(g_app.snap.temporal, "failed") == 0;
                    const bool afDegraded = afReqOn && g_app.snap.temporal[0] &&
                                            std::strcmp(g_app.snap.temporal, "off") != 0 &&
                                            g_app.snap.temporalRoute == 0;
                    const bool afBroken = afFailed || afDegraded;
                    char afReq[32];
                    std::snprintf(afReq, sizeof(afReq), "%s",
                                  afReqOn ? kAntiFlickerNames[std::clamp(
                                                g_app.params.antiFlicker,
                                                kAntiFlickerMin, kAntiFlickerMax)]
                                          : "关");
                    char afEff[112];
                    if (!g_app.snap.temporal[0]) {
                        std::snprintf(afEff, sizeof(afEff), "(未加载)");
                    } else if (afFailed) {
                        std::snprintf(afEff, sizeof(afEff), "重建失败(已降级关闭)");
                    } else if (!afReqOn || std::strcmp(g_app.snap.temporal, "off") == 0) {
                        std::snprintf(afEff, sizeof(afEff), "未开启");
                    } else {
                        std::snprintf(afEff, sizeof(afEff), "档位 %d · %s · w=%.2f",
                                      g_app.snap.temporalRoute,
                                      std::strcmp(g_app.snap.temporal, "steady") == 0 ? "稳态"
                                                                                      : "播种",
                                      g_app.snap.temporalW);
                    }
                    if (afBroken) ImGui::TextColored(kErrRed, "抗闪烁");
                    else ImGui::TextUnformatted("抗闪烁");
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(reqX);
                    ImGui::TextUnformatted(afReq);
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(actX);
                    TextColoredWrapped(afBroken ? kErrRed : kDimTxt, "%s", afEff);
                }

                // FG 请求 vs 实际路由
                char fgReq[96];
                if (!g_app.params.fgEnabled) {
                    std::snprintf(fgReq, sizeof(fgReq), "关");
                } else {
                    static const char *kRouteNames[] = { "自动", "纯官方" };
                    std::snprintf(fgReq, sizeof(fgReq), "开 %dx,路由 %s",
                                  g_app.params.fgMultiplier,
                                  kRouteNames[std::clamp(g_app.params.fgRoute, kFgRouteMin, kFgRouteMax)]);
                }
                const bool fgBroken = g_app.params.fgEnabled != 0 &&
                                      (std::strcmp(g_app.snap.fgState, "unavailable") == 0 ||
                                       std::strcmp(g_app.snap.fgRouteEff, "copy") == 0);
                char fgEff[192]; // 路由标签(≤34)+ " —— " + fgDetail(127)+NUL
                if (!g_app.snap.fgRouteEff[0]) {
                    std::snprintf(fgEff, sizeof(fgEff), "(未加载)");
                } else if (std::strcmp(g_app.snap.fgRouteEff, "off") == 0) {
                    std::snprintf(fgEff, sizeof(fgEff), "未开启");
                } else {
                    const char *rl = FgRouteLabel(g_app.snap.fgRouteEff);
                    std::snprintf(fgEff, sizeof(fgEff), "%s%s%s", rl[0] ? rl : g_app.snap.fgRouteEff,
                                  g_app.snap.fgDetail[0] ? " —— " : "", g_app.snap.fgDetail);
                }
                if (fgBroken) ImGui::TextColored(kErrRed, "帧生成");
                else ImGui::TextUnformatted("帧生成");
                ImGui::SameLine();
                ImGui::SetCursorPosX(reqX);
                ImGui::TextUnformatted(fgReq);
                ImGui::SameLine();
                ImGui::SetCursorPosX(actX);
                TextColoredWrapped(fgBroken ? kErrRed : kDimTxt, "%s", fgEff);

                // 创建倍数 vs live 倍数 vs 运行库上限:上限 < 创建值 = gate
                // 解锁失败回落(输出按高倍率节拍,超出槽位全是复制真实帧)
                // —— 此前这一事实无任何 stats 键,面板全绿,用户毫无感知。
                if (g_app.params.fgEnabled && g_app.snap.fgMultCreate > 0) {
                    const bool multClipped = g_app.params.fgMultiplier > g_app.snap.fgMultCreate;
                    // fg_mult_max 是插值帧数口径(M 倍 = M-1 插值):实效倍数
                    // = 上限+1。直接拿它跟倍数比会把健康的 6x(上限 5 插值)
                    // 误判成被压档红显 —— 曾致"5x 能开 6x 开不了"的假警。
                    const bool capped = g_app.snap.fgMultMax >= 1 &&
                                        g_app.snap.fgMultMax + 1 < g_app.snap.fgMultCreate;
                    char fgAct[224];
                    if (capped) {
                        std::snprintf(fgAct, sizeof(fgAct),
                                      "会话创建 %dx | 运行库上限 %dx"
                                      "(超出槽位为复制帧 —— gate 解锁失败?驱动更新后重试)",
                                      g_app.snap.fgMultCreate, g_app.snap.fgMultMax + 1);
                    } else {
                        std::snprintf(fgAct, sizeof(fgAct), "会话创建 %dx%s",
                                      g_app.snap.fgMultCreate,
                                      multClipped ? "(超出部分需重载生效)" : "");
                    }
                    if (capped || multClipped) ImGui::TextColored(kErrRed, "FG 倍数");
                    else ImGui::TextUnformatted("FG 倍数");
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(reqX);
                    ImGui::Text("面板 %dx", g_app.params.fgMultiplier);
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(actX);
                    TextColoredWrapped((capped || multClipped) ? kErrRed : kDimTxt, "%s", fgAct);
                }

                // RTX 请求 vs 实际:请求开(vsrMode>0 / hdrEnabled)而实态
                // off = 降级(capability/部署问题),原因串 rtx_detail 红显。
                {
                    const bool vsrReq = g_app.params.rtxVsrMode != 0;
                    const bool hdrReq = g_app.params.rtxHdrEnabled != 0;
                    char rtxReq[64];
                    if (!vsrReq && !hdrReq) {
                        std::snprintf(rtxReq, sizeof(rtxReq), "关");
                    } else if (vsrReq && hdrReq) {
                        std::snprintf(rtxReq, sizeof(rtxReq), "VSR + HDR");
                    } else {
                        std::snprintf(rtxReq, sizeof(rtxReq), "%s", vsrReq ? "VSR" : "HDR");
                    }
                    const bool rtxDown = (vsrReq || hdrReq) &&
                                         std::strcmp(g_app.snap.rtx, "off") == 0;
                    if (rtxDown) ImGui::TextColored(kErrRed, "RTX Video");
                    else ImGui::TextUnformatted("RTX Video");
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(reqX);
                    ImGui::TextUnformatted(rtxReq);
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(actX);
                    TextColoredWrapped(rtxDown ? kErrRed : kDimTxt, "%s%s%s",
                                       g_app.snap.rtx[0] ? g_app.snap.rtx : "(未加载)",
                                       g_app.snap.rtxDetail[0] ? " —— " : "", g_app.snap.rtxDetail);
                }
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("—— 排队细分(每帧 last)——");
            // 两列:标签在 marginX,值列与上方"请求"列对齐
            const float qValX = marginX + colCtrl;
            ImGui::TextUnformatted("槽池等待");
            ImGui::SameLine();
            ImGui::SetCursorPosX(qValX);
            ImGui::Text("%.2f ms", g_app.snap.slotWait);
            if (ImGui::IsItemHovered())
                ShowTip("槽池全占时等槽的时长:大 = GPU 超载;与 NGX 等待都小而帧率低 = 上游没来帧。");
            ImGui::TextUnformatted("NGX 串行等待");
            ImGui::SameLine();
            ImGui::SetCursorPosX(qValX);
            ImGui::Text("%.2f ms", g_app.snap.lockWait);
            if (ImGui::IsItemHovered())
                ShowTip("并发槽等 NGX 互斥的时长(与 perf 行 lock= 同源)。");
            ImGui::TextUnformatted("光流门");
            ImGui::SameLine();
            ImGui::SetCursorPosX(qValX);
            ImGui::Text("过期 %d / 重置 %d", g_app.snap.gateExpired, g_app.snap.gateResets);
            if (ImGui::IsItemHovered())
                ShowTip("过期 = 帧迟到(乱序播种),重置 = seek(其后增加属正常);\n持续增长 = 时序异常。");

            ImGui::Spacing();
            ImGui::TextDisabled("全页无红 = 插件正常工作。红色 = 与面板请求不一致。");
            y = ImGui::GetCursorPosY() - wpos.y + 4 * s;
}

} // namespace

// ---------------------------------------------------------------------------
// DrawUi:头尾(标题栏/遥测带/状态带/页签框架/全局行/处理用时)原样保留;
// 页签体在上方四个 Draw*Page(原 lambda 布局件已收拢为 UiCtx 成员)。
// ---------------------------------------------------------------------------
void DrawUi() noexcept {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##panel", nullptr, flags);

    // 绝对定位布局:全部坐标手动计算(96dpi 基准 × uiScale),
    // 窗口高度在末尾按内容实际 y 收口,避免固定尺寸溢出。
    const float s = g_app.uiScale;
    const float th = kTitleBarH * s;
    const float marginX = 18 * s;
    const float colCtrl = 112 * s;      // 控件列起点(标签列 94 = 容 5 个中文字)
    const float trackW = kTrackW * s;   // 滑块/下拉统一轨道宽(全面板唯一宽度)
    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // --- 自绘标题栏(WS_POPUP;拖动由 WM_NCHITTEST HTCAPTION 处理) ---
    dl->AddRectFilled(wpos, ImVec2(wpos.x + wsize.x, wpos.y + th), IM_COL32(30, 32, 42, 255));
    dl->AddLine(ImVec2(wpos.x, wpos.y + th), ImVec2(wpos.x + wsize.x, wpos.y + th), IM_COL32(58, 62, 78, 255));

    // 唯一的标题栏按钮:✕(隐藏到托盘;真正退出在托盘右键菜单)。
    // 线条自绘图标(✕ 字符不在中文字体 glyph 范围,会显示问号)。
    const float bs = kCloseBtnSize * s, bpad = kCloseBtnPad * s;
    const float bxClose = wsize.x - bs - bpad;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + bxClose, wpos.y + (th - bs) * 0.5f));
    if (ImGui::Button("##close", ImVec2(bs, bs))) {
        ShowWindow(g_hwnd, SW_HIDE);
    }
    {
        const bool hov = ImGui::IsItemHovered();
        const ImU32 col = hov ? IM_COL32(240, 130, 130, 255) : IM_COL32(154, 160, 176, 255);
        const ImVec2 c = ImGui::GetItemRectMin();
        const ImVec2 c2 = ImGui::GetItemRectMax();
        const float cy = (c.y + c2.y) * 0.5f, cx = (c.x + c2.x) * 0.5f;
        dl->AddLine(ImVec2(cx - 5 * s, cy - 5 * s), ImVec2(cx + 5 * s, cy + 5 * s), col, 2.0f * s);
        dl->AddLine(ImVec2(cx - 5 * s, cy + 5 * s), ImVec2(cx + 5 * s, cy - 5 * s), col, 2.0f * s);
        if (hov) {
            ShowTip("隐藏到托盘(退出在托盘右键菜单)");
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(wpos.x + 16 * s, wpos.y + (th - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted("DLSSNR 控制面板");

    // --- 性能分析器(照抄 Magpie Overlay Profiler 样式,数据来自插件共享内存) ---
    // 遥测带背景高度流式自适应:用上一帧的带底位置画背景(滞后一帧无感),
    // Collapse 收起/展开时窗口高度随之自动收缩。
    static float s_bandBottom = th + 120 * s;
    dl->AddRectFilled(ImVec2(wpos.x, wpos.y + th), ImVec2(wpos.x + wsize.x, wpos.y + s_bandBottom),
                      IM_COL32(24, 28, 40, 255));
    dl->AddLine(ImVec2(wpos.x, wpos.y + s_bandBottom), ImVec2(wpos.x + wsize.x, wpos.y + s_bandBottom),
                IM_COL32(58, 62, 78, 255));
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + th + 12 * s));

    // Magpie Profiler 头部:GPU 名称 + 帧率(全程显示,无滤镜时占位)。
    // 首行必须手动定位到 marginX:后续行自动回流到 WindowPadding.x,历史
    // 上这里写死 16*s,而 WindowPadding 不缩放,高 DPI 下首行比后续行更靠右。
    ImGui::Text("GPU: %s", g_app.snap.gpuName[0] ? g_app.snap.gpuName : "(等待滤镜加载)");
    // fps 打点在源帧入口(不含插帧):fg 状态非 off 即按 ×M 节奏出帧
    // (on=真插值;dup/unavailable=复制帧,节拍不变),输出 = 源 × fg_mult。
    // 计数本身就是整数,按整数显示(%.1f 是假精度)。
    {
        const bool fgCadence = g_app.snap.fgState[0] && std::strcmp(g_app.snap.fgState, "off") != 0 && g_app.snap.fgMult > 0;
        const int mult = fgCadence ? g_app.snap.fgMult : 1;
        const int src = static_cast<int>(g_app.snap.fps + 0.5);
        if (mult > 1) {
            // 箭头链与分辨率行同款:源 → 插帧输出
            ImGui::Text("帧率: %d → %d FPS", src, src * mult);
        } else {
            ImGui::Text("帧率: %d FPS", src);
        }
    }
    // 连接指示:此前"滤镜未加载 / 插件没在跑 / stats magic 不配对"三种
    // 故障的用户可见表象完全相同(滑块能调、能保存、画面永远不变),
    // 区别只在日志文件。这里一行给出最短诊断路径。
    {
        const char *connTxt;
        ImVec4 col = kDimTxt;
        if (g_app.connState == 1) {
            connTxt = "插件: 已连接";
        } else if (g_app.connState == 2) {
            connTxt = "插件: 版本不匹配 —— 面板与 vs_dlssnr.dll 必须成对更新";
            col = kErrRed;
        } else {
            connTxt = "插件: 未检测到(等待滤镜加载;调参会保存,滤镜加载后生效)";
        }
        ImGui::TextColored(col, "%s", connTxt);
    }

    // 大号:NGX 纯延迟(窗口级字体缩放, imgui 1.91 的 PushFont 是单参;
    // 1.4→1.25 高度收紧,辨识度足够)
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextColored(ImVec4(120 / 255.0f, 190 / 255.0f, 1.0f, 1.0f), "%s", g_app.statsBig);
    ImGui::SetWindowFontScale(1.0f);
    if (g_app.statsRes[0]) {
        ImGui::TextDisabled("%s", g_app.statsRes);
    }
    // 滤镜状态行(StatsPayload.filterState):这是降级/故障在面板上的唯一可见信号
    // (GUI mpv 看不到日志,timing log 没人看)。空 = 正常。主面板保持
    // 纯净调参体验:请求 vs 实际的分级红显集中在"诊断"页。
    {
        const ImVec4 warnCol(1.0f, 0.62f, 0.20f, 1.0f);
        const ImVec4 errCol(1.0f, 0.45f, 0.45f, 1.0f);
        if (std::strcmp(g_app.snap.filterState, "passthrough") == 0) {
            ImGui::TextColored(warnCol, "滤镜已回退直通(画面未增强)");
            if (g_app.snap.stateDetail[0]) TextDisabledWrapped(g_app.snap.stateDetail);
        } else if (std::strcmp(g_app.snap.filterState, "ngx_faulted") == 0) {
            ImGui::TextColored(errCol, "NGX 故障,滤镜已停用 —— 重启 mpv 恢复");
            if (g_app.snap.stateDetail[0]) TextDisabledWrapped(g_app.snap.stateDetail);
        } else if (std::strcmp(g_app.snap.filterState, "nvof_zero") == 0) {
            ImGui::TextColored(warnCol, "光流无运动数据,已降级(增强继续)");
            // 原因直达(of_detail):此前"为什么降级"只活在 timing log。
            if (g_app.snap.ofDetail[0]) TextDisabledWrapped(g_app.snap.ofDetail);
        }
        // 实际光流模式:请求档位 ≠ 实际能力(Turing 无 cost / 驱动拒双向)
        // 时在这里暴露;档位关闭(off)不显示。
        if (g_app.snap.ofMode[0] && std::strcmp(g_app.snap.ofMode, "off") != 0) {
            ImGui::TextDisabled("光流模式: %s", g_app.snap.ofMode);
        }
        // DLSS FG 状态:on=插值中;dup=复制帧(复位/零光流/开关暂关/降级);
        // off=本会话未激活;unavailable=代理初始化失败回退 1:1。
        if (g_app.snap.fgState[0] && std::strcmp(g_app.snap.fgState, "off") != 0) {
            const char *desc = std::strcmp(g_app.snap.fgState, "on") == 0 ? "插值中"
                             : std::strcmp(g_app.snap.fgState, "dup") == 0 ? "复制帧"
                             : std::strcmp(g_app.snap.fgState, "unavailable") == 0 ? "不可用(1:1)"
                             : g_app.snap.fgState;
            if (g_app.snap.fgMult >= 2 && (std::strcmp(g_app.snap.fgState, "on") == 0 ||
                                      std::strcmp(g_app.snap.fgState, "dup") == 0)) {
                // 库上限 < 面板倍数(40 系 gate 解锁失败回落 2x):输出按
                // 面板倍数节拍走但实际密度只有 (上限+1)x —— 必须在此可见,
                // 否则"选了 6x 全绿"而实际每 6 帧里 4 帧是复制帧。
                if (g_app.snap.fgMultMax >= 1 && g_app.snap.fgMultMax + 1 < g_app.snap.fgMult) {
                    ImGui::TextDisabled("帧生成: %s (%dx, 实效 %dx —— 运行库上限)",
                                        desc, g_app.snap.fgMult, g_app.snap.fgMultMax + 1);
                } else {
                    ImGui::TextDisabled("帧生成: %s (%dx)", desc, g_app.snap.fgMult);
                }
            } else {
                ImGui::TextDisabled("帧生成: %s", desc);
            }
        }
    }
    ImGui::Spacing();

    // 遥测带底(供下一帧背景):带内只留状态行。处理用时整段移到窗口
    // 最底部 —— 分段行随帧出现/消失引起的高度变化只影响底部收口,
    // 不再推动上方的参数控件(高度快速抖动问题)。
    s_bandBottom = ImGui::GetCursorPosY() + 8 * s;

    // Label 与控件垂直居中:控件文本在 frame 内偏移 FramePadding.y,标签
    // 按同一中心线对齐。旧的硬编码 +7/+8*s 在高 DPI 下漂移(字体随 s 放大,
    // FramePadding 固定不放大),表现为参数名与滑块错位。
    const float labelDy = (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f;
    // 行高 = 控件 frame + 紧凑间距(#38 度量驱动;间距从 10*s 收紧到 4*s
    // —— 高度预算的主压缩之一,控件 frame 本体不动)。
    const float rowH = ImGui::GetFrameHeight() + 4 * s;

    // 参数行布局:全双列,相关控件两两并排。所有滑块/下拉统一轨道宽
    // trackW(kTrackW,96dpi 下 160px)—— 不再有长短滑块,精调精度全面板
    // 一致。控件列对齐:第一列控件从 colCtrl 起步,第二列从 marginX +
    // (halfW+colGap) + pairLabelW;标签列宽 = colCtrl−marginX = 94(容
    // 5 个中文字),两列之间留 colGap 空隙,右列标签不贴左列控件。
    const float colGap = kColGap * s;
    const float halfW = (wsize.x - 2 * marginX - colGap) * 0.5f; // 单列内容宽
    const float pairLabelW = colCtrl - marginX;

    // 布局件容器(原 5 个 lambda 收拢;y 游标经引用共享,头尾与页签同源)。
    UiCtx ui{s,          marginX, colCtrl,   trackW,
             halfW,      colGap,  pairLabelW, labelDy,
             rowH,       wpos};
    float &y = ui.y;
    y = s_bandBottom + 12 * s;

    // --- 功能页签:神经渲染 / 帧生成分页,不再全挤在一页 ---
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    bool pgNrVis = false, pgFgVis = false, pgRtxVis = false, pgDiagVis = false; // 本帧各页可见性
    if (ImGui::BeginTabBar("##feature_tabs")) {
        // --- 页:神经渲染(NR + 光流 + 分辨率缩放) ---
        if (ImGui::BeginTabItem("神经渲染", nullptr,
                                (g_app.pageRestore && g_app.page == 0)
                                    ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            pgNrVis = true;
            // 内容可见且 page 不符 = 用户刚点击切到本页(点击帧内容尚未可
            // 见,次日帧才渲染 —— IsItemClicked 检测不到,改为以"可见性"
            // 落盘)。恢复期内不落盘:启动早期帧的"可见"是 appearing 布局
            // 的陈旧态,不是用户选择,落盘会把 ini 打回第 0 页。
            if (!g_app.pageRestore && g_app.page != 0) ui.savePagePref(0);
            y = ImGui::GetCursorPosY() - wpos.y + 6 * s;

            DrawNrPage(ui);

            ImGui::EndTabItem();
        }

        // --- 页:帧生成(DLSS-FG 后端与路由) ---
        if (ImGui::BeginTabItem("帧生成", nullptr,
                                (g_app.pageRestore && g_app.page == 1)
                                    ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            pgFgVis = true;
            if (!g_app.pageRestore && g_app.page != 1) ui.savePagePref(1);
            y = ImGui::GetCursorPosY() - wpos.y + 6 * s;

            DrawFgPage(ui);

            ImGui::EndTabItem();
        }

        // --- 页:RTX 超分 / HDR(NVIDIA RTX Video SDK 1.1 的 NGX VSR +
        // TrueHDR;管线顺序 NR→VSR→HDR→FG,官方明文 HDR 必须在 VSR 后)---
        if (ImGui::BeginTabItem("RTX 超分/HDR", nullptr,
                                (g_app.pageRestore && g_app.page == 2)
                                    ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            pgRtxVis = true;
            if (!g_app.pageRestore && g_app.page != 2) ui.savePagePref(2);
            y = ImGui::GetCursorPosY() - wpos.y + 6 * s;

            DrawRtxPage(ui);

            ImGui::EndTabItem();
        }

        // --- 页:诊断(运行时事实 + 排队细分)。timing log 里才有的次级
        // 数据搬到这里;"请求 vs 实际"不一致的行一律红 —— 全页无红 = 插件
        // 正常工作。数据与 dlssnr_timing.log 的 perf/STATUS 行同源。---
        if (ImGui::BeginTabItem("诊断", nullptr,
                                (g_app.pageRestore && g_app.page == 3)
                                    ? ImGuiTabItemFlags_SetSelected
                                    : ImGuiTabItemFlags_None)) {
            pgDiagVis = true;
            if (!g_app.pageRestore && g_app.page != 3) ui.savePagePref(3);
            y = ImGui::GetCursorPosY() - wpos.y + 6 * s;
            DrawDiagPage(ui);

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        // 恢复完成判定:期望页真正可见才解除恢复锁;预算烧完则放弃(防 ini
        // 值异常时锁死)。恢复期每帧重喂 SetSelected(首帧排队、次帧布局落
        // 地),并配合主循环的强制重绘 —— 空闲门不再把窗口冻在切换前旧帧。
        if (g_app.pageRestore) {
            const bool targetVis =
                (g_app.page == 0 && pgNrVis) || (g_app.page == 1 && pgFgVis) ||
                (g_app.page == 2 && pgRtxVis) || (g_app.page == 3 && pgDiagVis);
            if (targetVis || --g_app.restoreFrames <= 0)
                g_app.pageRestore = false;
        }
    }
    y += 8 * s;

    // --- 全局行:保存/重置(性能日志开关已迁往"诊断"页)---
    dl->AddLine(ImVec2(wpos.x + marginX, wpos.y + y), ImVec2(wpos.x + wsize.x - marginX, wpos.y + y), IM_COL32(58, 62, 78, 255));
    y += 14 * s;

    y += 8 * s;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    if (ImGui::Button("保存设置", ImVec2(120 * s, 30 * s))) {
        // ini 单写者 = 本面板(bridge 侧写块已撤,2026-10-03):成败提示
        // 即最终落盘结果,无第二进程覆写。失败不再假报成功:ini 被占用/
        // 只读时状态栏红字说明,否则用户看到"已保存",重启全部回旧值
        // (dlssnr_ini.h 注释预言的场景)。
        if (WriteIniNow()) {
            WritePayload(); // logEnabled 等开关态随载荷同步
            snprintf(g_app.status, sizeof(g_app.status), "已保存");
        } else {
            snprintf(g_app.status, sizeof(g_app.status), "保存失败(文件被占用或不可写)");
        }
    }
    if (ImGui::IsItemHovered()) {
        ShowTip("调参即时生效;保存后作为默认值,下次加载滤镜自动应用。");
    }
    ImGui::SameLine(0, 14 * s);
    if (ImGui::Button("重置默认", ImVec2(120 * s, 30 * s))) {
        // Reset = push the factory-default payload itself; the plugin applies
        // it through the same Request*/Update path as any other edit (there
        // is no separate reset command in the protocol).
        const DlssnrParams beforeReset = g_app.params;
        g_app.params = DlssnrParams{};
        WritePayload();
        g_app.liveDirty = true; // reseek 消费挂在 liveDirty 节流分支里,不置位
                                // 上面 reseekDirty 永远无人消费(重置关 RTX
                                // 也不重建链)。多写一次 payload 幂等无害。
        // RTX Video 开关(mode/HDR)是创建时参数:重置真关掉了它们才触发
        // 链重建(与开关控件同款 reseek 语义;本来就没开时不付一次 seek)。
        if (beforeReset.rtxVsrMode != 0 || beforeReset.rtxHdrEnabled != 0) {
            g_app.reseekDirty = true;
        }
        // Optimized 档位不在 payload 里(插件不消费):随重置归 1 并写回
        // 代理 ini,与面板显示保持一致。写失败保持现值 + 状态栏说明
        // (与档位下拉同款语义:UI 不说谎)。
        if (WriteFgOptimizedIni(1)) {
            g_app.fgOptimized = 1;
            snprintf(g_app.status, sizeof(g_app.status), "已重置");
        } else {
            snprintf(g_app.status, sizeof(g_app.status),
                     "参数已重置;内核档位写入失败(代理配置缺失或被占用)");
        }
    }
    ImGui::SameLine(0, 16 * s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * s);
    if (g_app.status[0]) ImGui::TextUnformatted(g_app.status);
    y += 38 * s;

    // --- 处理用时(窗口最底部):分段行随帧出现/消失时,高度变化只影响
    // 底部收口,上方的页签、参数与按钮保持静止(不再快速跳动)。---
    dl->AddLine(ImVec2(wpos.x + marginX, wpos.y + y), ImVec2(wpos.x + wsize.x - marginX, wpos.y + y), IM_COL32(58, 62, 78, 255));
    y += 14 * s;
    ImGui::SetCursorScreenPos(ImVec2(wpos.x + marginX, wpos.y + y));
    const bool timingsOpen = ImGui::CollapsingHeader("处理用时", ImGuiTreeNodeFlags_DefaultOpen);
    if (timingsOpen && g_app.hasSegments) {
        // 分段 = 帧内执行顺序,fg/hdr 相对顺序随处理形态(管线全流程解耦,
        // 转换独立成段恒在末尾):
        //   默认:gpu(base)→ fg(postA:DLSSG SDR 域推理)→
        //         hdr(TrueHDR 链)→ conv(post:全部输出转换 + 回读)
        //   HDR 域插帧(fg_hdr_interp):gpu → vsr →
        //         hdr(TrueHDR 前置,真实帧一次)→ fg(HDR 域插帧)→ conv
        // 显示值 = segDisp*(EMA 平滑)。
        // 关闭段恒 0(OF 无消费者的 nvof、NR 关的 eval_cpu、RTX 关的
        // vsr/hdr),零值段由下方 <1e-3f 跳过,时间线自动收缩。conv 恒非 0
        // (格式契约必需)。
        const bool hdrFirst = g_app.params.fgHdrInterp != 0;
        const float total = g_app.segDispPack + g_app.segDispNvof + g_app.segDispEval +
                            g_app.segDispQueue + g_app.segDispGpu + g_app.segDispRtxVsr +
                            g_app.segDispFg + g_app.segDispRtxHdr + g_app.segDispConv +
                            g_app.segDispUnpack;
        struct Seg { float v; ImU32 c; const char *name; };
        const Seg segFg{ g_app.segDispFg,     IM_COL32(0, 150, 136, 255),   "fg(补帧链)" };
        const Seg segHdr{ g_app.segDispRtxHdr, IM_COL32(255, 152, 0, 255),  "hdr(RTX HDR)" };
        const Seg segConv{ g_app.segDispConv, IM_COL32(121, 85, 72, 255),   "conv(输出转换)" };
        const Seg segs[10]{
            { g_app.segDispPack,   IM_COL32(229, 57, 53, 255),   "pack(打包)" },
            { g_app.segDispNvof,   IM_COL32(156, 39, 176, 255),  "of(光流引擎)" },
            { g_app.segDispEval,   IM_COL32(63, 81, 181, 255),   "eval_cpu(NGX 调用)" },
            { g_app.segDispQueue,  IM_COL32(84, 110, 122, 255),  "queue(帧间排队)" },
            { g_app.segDispGpu,    IM_COL32(30, 136, 229, 255),  "gpu(NR 推理)" },
            { g_app.segDispRtxVsr, IM_COL32(67, 160, 71, 255),   "vsr(RTX 超分)" },
            hdrFirst ? segHdr : segFg,
            hdrFirst ? segFg : segHdr,
            segConv,
            { g_app.segDispUnpack, IM_COL32(0, 137, 123, 255),   "unpack(解包)" },
        };
        constexpr int kSegCount = 10;
        // 实际要画的段数(零值段跳过)。零值判定 = 显示精度下的 0:
        // <1e-3ms 渲染出来就是 "0.000 ms",按用户裁定视作 0ms 不画;
        // 除此之外无任何下限门(旧 total>0.5ms 会把真实存在但很小的
        // 处理时间整段吞掉)。BeginTable 的列数必须与之相等:imgui 对
        // 本帧未 TableSetupColumn 的列按 SizingStretchSame 默认权重 1.0
        // 补齐,而可见段权重和恒为 1.0 —— 空列恰好占掉一半宽度(of=0
        // 时长条右侧大片空白)。visibleSegs=0 时连表都不开(imgui 对
        // 0 列 BeginTable 断言)。
        int visibleSegs = 0;
        for (int i = 0; i < kSegCount; ++i)
            if (segs[i].v >= 1e-3f) ++visibleSegs;
        if (total > 0.0f && visibleSegs > 0) {
            ImGui::Spacing();
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5, 5));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

            if (ImGui::BeginTable("timeline", visibleSegs)) {
                for (int i = 0; i < kSegCount; ++i) {
                    if (segs[i].v < 1e-3f) continue;
                    char colId[8];
                    snprintf(colId, sizeof(colId), "%d", i);
                    ImGui::TableSetupColumn(colId,
                        ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder,
                        segs[i].v / total);
                }
                ImGui::TableNextRow();
                for (int i = 0; i < kSegCount; ++i) {
                    if (segs[i].v < 1e-3f) continue;
                    ImGui::TableNextColumn();
                    ImGui::PushID(i);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, segs[i].c);
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive, segs[i].c);
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, segs[i].c);
                    ImGui::PushStyleColor(ImGuiCol_Header, segs[i].c);
                    ImGui::Selectable("", false);
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered() || ImGui::IsItemClicked()) {
                        char tipContent[128];
                        snprintf(tipContent, sizeof(tipContent), "%s\n%.3f ms\n%d%%", segs[i].name,
                                 segs[i].v, static_cast<int>(segs[i].v / total * 100 + 0.5f));
                        ImGui::SetTooltip("%s", tipContent);
                    }
                    // 空间足够时居中显示百分比
                    char pctText[16];
                    snprintf(pctText, sizeof(pctText), "%d%%", static_cast<int>(segs[i].v / total * 100 + 0.5f));
                    const float textWidth = ImGui::CalcTextSize(pctText).x;
                    const float itemWidth = ImGui::GetItemRectSize().x;
                    if (itemWidth > textWidth + 4 * s) {
                        ImGui::SameLine(0, 0);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (itemWidth - textWidth) / 2);
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 0.5f * s);
                        ImGui::TextUnformatted(pctText);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar(4);
            ImGui::Spacing();

            // timings 列表:色点 ■ + 名称 + 右对齐时间
            if (ImGui::BeginTable("timings", 1, ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize);
                for (int si = 0; si < kSegCount; ++si) {
                    const Seg &seg = segs[si];
                    if (seg.v < 1e-3f) continue; // 零值段不列(of=0 的 nvof / 关缩放)
                    ImGui::PushID(si);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    char timeStr[32];
                    snprintf(timeStr, sizeof(timeStr), "%.3f ms", seg.v);
                    const float timeWidth = ImGui::CalcTextSize(timeStr).x;
                    const float wrapPos = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - timeWidth - 8 * s;
                    ImGui::Selectable("", false, 0, ImVec2(0, ImGui::GetTextLineHeight()));
                    ImGui::SameLine(0, 3 * s);
                    const ImVec4 segCol = ImGui::ColorConvertU32ToFloat4(seg.c);
                    // ■ 字形已在 fontUI 的 glyph range 里(Magpie COLOR_INDICATOR 同款)
                    ImGui::TextColored(segCol, "%s", vsdlssnr::fonts::COLOR_INDICATOR);
                    ImGui::SameLine(0, 3 * s);
                    ImGui::PushTextWrapPos(wrapPos);
                    ImGui::TextUnformatted(seg.name);
                    ImGui::PopTextWrapPos();
                    ImGui::SameLine(0, 0);
                    ImGui::SetCursorPosX(wrapPos + 8 * s);
                    // 时间数值用等宽数字字体(Magpie _fontMonoNumbers 用途)
                    if (g_fontMono) {
                        ImGui::PushFont(g_fontMono);
                        ImGui::TextUnformatted(timeStr);
                        ImGui::PopFont();
                    } else {
                        ImGui::TextUnformatted(timeStr);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
        }
    }

    // 高度收口:按内容实际底部调整窗口(WS_POPUP 尺寸 = client 尺寸)
    y = ImGui::GetCursorPosY() - wpos.y + 10 * s;
    const float needH = y + 8 * s;
    RECT wrc{};
    GetWindowRect(g_hwnd, &wrc);
    if (std::abs((wrc.bottom - wrc.top) - static_cast<int>(needH)) > 1) {
        SetWindowPos(g_hwnd, nullptr, 0, 0, wrc.right - wrc.left,
                     static_cast<int>(needH), SWP_NOZORDER | SWP_NOMOVE);
    }

    ImGui::End();
}
