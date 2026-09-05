#pragma once
// Shared IPC layout between dlssnr_panel.exe (writer) and vs_dlssnr.dll
// (reader). Two named file mappings, both 512 bytes:
//   "vs_dlssnr_panel_params" - panel pushes parameter edits (seq-gated)
//   "vs_dlssnr_stats"        - plugin pushes perf stats (60-frame cadence)

#include <cstdint>
#include <cstring>
#include <windows.h>

namespace vsdlssnr {

constexpr wchar_t PARAMS_MAPPING[] = L"vs_dlssnr_panel_params";
constexpr wchar_t STATS_MAPPING[] = L"vs_dlssnr_stats";
constexpr uint32_t PAYLOAD_SIZE = 512;
constexpr uint32_t PAYLOAD_MAGIC = 0x314C5344u; // "DSSL1"

// Plugin-side stats publisher. Mapping + writable view are created once and
// kept for the process lifetime (the kernel object dies with the plugin
// process; readers map read-only per refresh). Replacing the mapping handle
// per write would leak one handle per publish.
inline bool PublishStatsJson(const char *json) noexcept {
    static HANDLE mapping = nullptr;
    static char *view = nullptr;
    if (!view) {
        if (!mapping) {
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, PAYLOAD_SIZE, STATS_MAPPING);
            if (!mapping) return false;
        }
        view = static_cast<char *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, PAYLOAD_SIZE));
        if (!view) return false;
    }
    size_t n = strlen(json);
    if (n > PAYLOAD_SIZE - 1) n = PAYLOAD_SIZE - 1;
    memcpy(view, json, n);
    view[n] = '\0';
    return true;
}

#pragma pack(push, 8)
struct PanelPayload {
    uint32_t magic;              // PAYLOAD_MAGIC
    uint32_t seq;                // panel increments per write; 0 = empty
    uint32_t generation;         // panel process instance (GetTickCount at start)
    int32_t preset;              // 0-3
    int32_t style;               // 0-2
    float intensity;             // 0-2
    float localTone;             // 0-2
    float localStructure;        // 0-2
    float skinStructure;         // -1-2
    int32_t useAutoMask;         // 0/1
    int32_t uiCorrection;        // 0/1
    int32_t inputResolution;     // 25-100
    float residualMultiplier;    // 1-2
    int32_t scalingEnabled;      // 0 = ignore inputResolution (treat as 100)
    int32_t saveRequest;         // panel "保存设置" press (applied once per seq)
    int32_t resetRequest;        // panel "重置默认" press
    int32_t logEnabled;          // perf log toggle state
    uint32_t reserved[5];
};
#pragma pack(pop)

static_assert(sizeof(PanelPayload) <= PAYLOAD_SIZE, "payload must fit the mapping");

} // namespace vsdlssnr
