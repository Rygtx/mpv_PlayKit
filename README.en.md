[简体中文](README.MD) | English

![](Temp/mpv_PK.jpg)

# mpv_PlayKit — DLSSNR Edition

> A fork of [hooke007/mpv_PlayKit](https://github.com/hooke007/mpv_PlayKit): on top of the official mpv-lazy release configuration (upstream main⊕lite baseline), it integrates the **DLSSNR** AI quality enhancement ported from the [Magpie](https://github.com/SAOG0721/Magpie) experimental fork.

_Who cares about logic, architecture, edge cases, or code style — vibe coding all the way, nobody reads the code, ship it when it feels right._

## What is this

**vs_dlssnr** — ports Magpie's DLSSNR (NVIDIA DLSS SDK 310.9.0 AI video enhancement, NGX Feature 18) into mpv as a native VapourSynth API4 plugin:

- Pure D3D12 implementation; zero-guidance single-frame mode by default, optional **NVOF hardware optical-flow guidance** (levels 0–5, RTX Turing+; automatically falls back to zero guidance when unsupported)
- **YUV native**: processes YUV420P8/P10 directly and outputs the same format, zero pixel conversion on the CPU side (pure row copy); color matrix/range handled per source frame properties (_Matrix/_ColorRange, 709/601 + limited/full), defaults to 709 limited
- Same-resolution processing (not upscaling)
- Static NGX core + snippet direct linking + IAT hook, fully replicating the Magpie call chain
- Residual pipeline: internal inference resolution adjustable 25–100%, Catmull-Rom residual reconstruction back to source resolution
- All parameters take effect in real time (preset switching goes through hot rebuild), with a standalone ImGui tuning panel

### Supported video color formats

| Content | Handling |
|---|---|
| YUV 4:2:0 8-bit (YUV420P8, nv12/yuv420p) | ✅ Enhanced (same format output) |
| YUV 4:2:0 10-bit (YUV420P10, HEVC 10-bit, etc.) | ✅ Enhanced (same format output) |
| Color matrix BT.709 / BT.601 / XYZ (auto-detected per frame properties, defaults to 709) | ✅ |
| Color range limited / full (per frame properties) | ✅ |
| HDR (BT.2020 matrix or PQ/HLG transfer, per frame properties) | ⏭️ Passthrough (treating it as SDR would produce wrong colors) |
| 4:2:2 / 4:4:4 / 12-bit and above / RGB | ⏭️ Passthrough |

> "Passthrough" = normal playback without enhancement; bare streams with missing properties cannot determine color metadata and are handled as 709 limited (consistent with mainstream player defaults).

For general mpv tweaks from upstream (configuration guides, mpv-lazy usage, etc.), see the [upstream Wiki](https://github.com/hooke007/mpv_PlayKit/wiki) and the general mpv tutorial (Chinese).

## Download

[**🏷 Releases 🏷**](../../releases) provides the `mpv_PlayKit-dlssnr-v<version>-full.zip` complete package (full portable_config directory + plugins + panel + model).

## Package contents

| File | Description |
|---|---|
| `portable_config\` | Complete configuration directory (official release config, with DLSSNR keybinds and menus integrated) |
| `vs-plugins\vs_dlssnr.dll` | VapourSynth API4 plugin (loaded automatically by mpv-lazy) |
| `vs-plugins\dlssnr_panel.exe` | Standalone ImGui tuning panel (optional, launched automatically when the filter loads) |
| `vs-plugins\ngx\nvngx_dlssnr.dll` | DLSSNR model (must reside in the `ngx\` subdirectory; the plugin resolves it by this relative path; taken from the RenoDX project) |
| `vs-plugins\ngx\nvngx_dlssg.dll` | Official DLSS frame-generation runtime (NVIDIA-signed, official-chain carrier; selectable via "FG route") |
| `vs-plugins\ngx\version.dll` | DLSS frame-generation hook proxy 0.3.x ([dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) ≥0.3.0, RTX 30/20; self-signed; intercepts the `nvngx_dlssg.dll` load and swaps in its embedded runtime) |
| `vs-plugins\ngx\dlssg_sm86.ini` | Frame-generation proxy factory config (shipped as-is; the plugin no longer writes it) |
| `portable_config\vs\DLSSNR_NV.vpy` | Filter script (parameters in the table below) |

## Installation (existing mpv-lazy)

1. Back up your `portable_config\` (if you have personal modifications)
2. Replace the original directory with the package's `portable_config\` (or overwrite file by file)
3. Merge everything inside `vs-plugins\` into the `vs-plugins\` under the mpv directory
4. Restart mpv

> When hand-editing `conf`/`vpy` files under `portable_config\` (with Chinese comments), keep **UTF-8 encoding**; re-saving as ANSI/GBK in Notepad will cause mpv parsing errors.

This package does not include mpv.exe or the VapourSynth runtime; use the official [mpv-lazy](https://github.com/hooke007/mpv_PlayKit/releases) release (preferably the same version as or newer than the packaging baseline, currently mpv-lazy-20260510).

## Usage

- Toggle the filter with the **`*`** key or via right-click menu **VF filters > DLSSNR enhancement (RTX)**
- First-frame initialization takes about 1 second (model loading); this is normal
- While playing, double-click `vs-plugins\dlssnr_panel.exe` to tune parameters in real time (the panel is also silently launched when the filter loads; it lives in the tray, click to summon)
  - Parameter changes take effect in real time; **"Save settings"** writes to `vs-plugins\dlssnr_ui.ini` and applies automatically on next filter load; **"Reset defaults"** restores factory parameters
  - **ini takes precedence over vpy parameters**; delete `dlssnr_ui.ini` to restore script defaults
  - Optical-flow quality dropdown (0–5) and the "optical flow follows scaling" switch are live-adjustable; when optical flow is unavailable the panel shows "degraded to zero guidance"
  - **Diagnostics tab**: the single home for monitoring — the tuning tabs stay clean. Session facts (requested vs actual: optical flow degraded to zero guidance, effective FG route off / official NGX (0.3.x proxy takeover) / official NGX direct / duplicate-frames with the failure reason, panel multiplier above the session cap — all highlighted in red; **which backend "auto" actually picked is shown here, no log digging**) plus queueing details (slot-pool wait / NGX serialize wait / optical-flow gate skip·expired·reset counters) plus the **"debug view" dropdown** (diff ×20 grayscale: white = big change, flat gray = untouched; optical flow = direction → hue, brightness = speed, black = no motion data) and the **"write performance log" switch** (controls `dlssnr_timing.log`). No red on the page = the plugin is working
  - Processing-time timeline chart (gpu / optical flow / inference segments)
  - The panel exits automatically when the filter is turned off / mpv exits

## Parameters (DLSSNR_NV.vpy)

| Parameter | Range | Default | Description |
|---|---|---|---|
| `NR_Enabled` | True/False | True | NR master switch; False = skip the denoise inference and pass source frames through while **frame generation / optical flow keep working untouched**; with both NR and FG off the whole filter initializes nothing at zero cost; panel toggles apply immediately within an activated session |
| `Preset` | 0–3 | 0 | NR preset level (create-time parameter; switching triggers hot rebuild) |
| `Style` | 0–2 | 0 | Style level (0 default / 1 natural / 2 cinematic) |
| `Intensity` | 0–2 | 1.0 | Intensity |
| `Local_Tone` | 0–2 | 1.0 | Local tone strength |
| `Local_Structure` | 0–2 | 1.0 | Local structure strength |
| `Skin_Structure` | 0–2 | 0 | Skin structure strength (upstream beta3 removed the −1 = auto mode) |
| `Use_Auto_Mask` | True/False | True | Use auto mask |
| `Scaling_Enabled` | True/False | True | Residual pipeline master switch; False = fully disabled (no intermediate textures, process at source size) |
| `Input_Resolution` | 25–100 | 100 | NGX internal inference resolution as a percentage of source size; residual reconstruction restores source resolution |
| `Residual_Multiplier` | 1.0–2.0 | 1.0 | Residual multiplier, compensating detail together with internal resolution scaling |
| `Motion_Vector_Quality` | 0–5 | 0 | NVIDIA optical-flow guidance level (0 = zero guidance; 1–5 use hardware optical flow to reduce motion-scene temporal artifacts; higher is more accurate but slower) |
| `Nvof_Follow_Scaling` | True/False | False | Optical-flow input follows internal downsampling (requires Scaling_Enabled; greatly reduces optical-flow engine load at a slight motion-accuracy cost) |
| `Fg_Enabled` | True/False | False | DLSS frame generation (chained after denoise, output fps ×2–×4; auto mode requires `ngx\version.dll` (dlssg_for_sm86 ≥0.3.0), falls back to 1:1 on init failure) |
| `Fg_Multiplier` | 2–4 | 2 | Interpolation multiplier (output frame count/pacing is fixed per session; panel changes auto-trigger an in-place mpv reload via `input-ipc-server`; without IPC, off/down-grade falls back to in-session real-frame duplication and up-grade needs a manual seek; 24fps ×3 = 72fps) |
| `Fg_Route` | 0–1 | 0 | FG route (0 = auto, preloads the dlssg_for_sm86 0.3.x hook proxy — RTX 30/20 get DLSS-G delivered through it while still driving the official signed chain; 1 = pure official, no preload, straight to the official runtime/RTX 40/50, no fallback when rejected on 30/20. Process-level, mpv restart required after switching. **From v20 the 0–3 range collapses to two gears: legacy ini/vpy 1/2 map to auto, 3 to pure official**) |
| `H_Max` | integer | 0 | Output height cap (sources above it skip processing; 0 = unlimited) |

Tuning tips: 100% with scaling on ≈ scaling off (equivalent when multiplier = 1); lowering levels does not save much frame time (NGX fixed cost dominates) and mainly affects high-frequency detail — 50–75% is recommended for 1080p content, 25% only for extreme power-saving scenarios; for 4K sources pair with `Input_Resolution = 50`. 4K full-resolution (res=100%) inference is about 200ms/frame — a physical ceiling, not stuttering.

## Feature dependencies (what cannot be enabled standalone)

| Feature | Depends on | When unmet |
|---|---|---|
| Optical-flow guidance levels 1–5 | RTX Turing+ GPU | Auto-falls back to zero guidance; nothing else is blocked |
| Optical-flow follow scaling | `Scaling_Enabled = True`; **force-ignored while frame generation is active** (FG requires a source-sized motion field) | Switch has no effect; optical flow runs at source size |
| `Input_Resolution` / `Residual_Multiplier` | `Scaling_Enabled = True` | Meaningless (processing happens at source size when scaling is off) |
| Frame generation `Fg_Enabled` | ① **Motion_Vector_Quality ≥ 1** (zero guidance has no real motion field) ② a matching runtime under `ngx\` (official `nvngx_dlssg.dll` or proxy `version.dll`) | With zero guidance every interpolated frame degrades to a duplicated real frame (switching it on accomplishes nothing); missing/rejected runtime falls back to 1:1, denoise unaffected |
| FG multiplier/switch auto-reload | mpv.conf `input-ipc-server` (enabled by default in the shipped config) | Off/down-grade degrades to in-session real-frame duplication; up-grade needs a manual seek |
| `Fg_Route` pure-official gear | `nvngx_dlssg.dll` exists and the hardware is in the official support range (RTX 40/50) | On 30/20 the architecture gate rejects it (0xBAD0000B) with **no fallback** — FG simply turns off (`fg_detail` on the diagnostics page carries the reason) |
| NR only off (`NR_Enabled = False`) | None (frame generation / optical flow work independently) | With frame generation **also** off → the whole filter initializes nothing at zero cost and every other parameter is moot |
| DLSSNR filter as a whole | RTX GPU (Tensor Core) + `ngx\nvngx_dlssnr.dll` model + YUV420 8/10-bit SDR source | Any missing → automatic passthrough (normal playback, no enhancement) |
| Panel stats/status area | Filter loaded | The panel can run alone to tweak and save the ini (applied on next load); stats area stays blank |
| Menu / `*` key DLSSNR toggle | Bound in both `input_uosc.conf` (uosc menu) and `menu.conf` (mpv context menu) | Custom key bindings must be updated in both places |

## Performance reference (measured, RTX 3080)

- 1080p: plugin-internal ≈16.5ms/frame (≈60fps capability; after YUV nativization: pack 0.2 + NGX inference + GPU optical flow + unpack 0.3, zero per-frame pixel conversion on the CPU side)
- 4K (res=50%): gpu ≈11–13ms/frame, real-time capable; res=100% about 200ms/frame (physical cost of NGX 4K full resolution)
- First-frame init ~1s (165MB model load)
- Diagnostics/timing switches (environment variables):
  - `VSDLSSNR_TIMING=1` writes per-frame segment timings to `dlssnr_timing.log` (use this for performance measurement, not python end-to-end numbers)
  - `VSDLSSNR_DUMP=1` dumps key textures to files (numeric acceptance)
  - `VSDLSSNR_PROBE=1` per-frame probe lines (timing issue localization)
  - `VSDLSSNR_SKIP_EVAL=1` skips inference to measure pipeline cost
  - `VSDLSSNR_D3D12_DEBUG=1` enables the D3D12 debug layer
  - `VSDLSSNR_NGX_LOG=1` hooks NGX internal logs
  - `VSDLSSNR_NO_PANEL=1` disables automatic panel launch (headless/acceptance scenarios)

## GPU support matrix

The two AI features have different GPU requirements:

- **DLSSNR denoise**: an official NVIDIA NGX feature, requires Tensor Core; all RTX cards (Turing+) are within the officially supported range, non-RTX unavailable
- **DLSS frame generation**: a single official NGX chain (Magpie-style: the NVIDIA-signed `nvngx_dlssg.dll` through the shared NGX core + NVOF-generated motion vectors); on RTX 30/20 the [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) **0.3.x hook proxy** delivers DLSS-G (the version.dll installs its hooks at LoadLibrary: intercepts the `nvngx_dlssg.dll` load and swaps in its embedded runtime, kernels auto-picked per physical architecture, tier 1 bit-identical to official). The panel "FG route" has two gears: Auto (preload the 0.3.x proxy) / Pure official (no preload, straight to the official runtime) — process-level, mpv restart required after switching

| GPU | Architecture | DLSSNR denoise | DLSS frame gen |
|---|---|---|---|
| RTX 50 | Blackwell (SM120) | 🟡 Theoretical (official range, untested) | 🟡 Theoretical (official NGX branch, untested) |
| RTX 40 | Ada (SM89) | 🟡 Theoretical (official range, untested) | 🟡 Theoretical (official NGX branch, untested) |
| RTX 30 | Ampere (SM86) | ✅ **Verified** (RTX 3080) | ✅ **Verified** (RTX 3080, 0.3.x proxy takeover of the official chain) |
| RTX 20 | Turing (SM75) | 🟡 Theoretical (official range, untested) | 🟡 Theoretical (0.3.x proxy kernel family auto-picked per physical architecture; upstream 2080 Ti verified, not tested with this plugin) |
| GTX / non-RTX | — | ❌ Unavailable (no Tensor Core) | ❌ Unavailable |

Legend: ✅ verified · 🟡 theoretical (untested) · ❌ unavailable

> - NVOF hardware optical-flow guidance (RTX Turing+) is natively supported across the line; on unsupported or non-NVIDIA cards it automatically falls back to zero guidance without blocking the feature
> - The official chain requires `nvngx_dlssg.dll` (NVIDIA-signed, included in the release package) next to the model DLL; Auto mode preloads the 0.3.x proxy before init (its hooks land before the capability query — one clean pass, no retry), while the "Pure official" gear skips the preload (on 30/20 the architecture gate rejects with 0xBAD0000B, FG off, `fg_detail` on the diagnostics page carries the reason)
> - DLSS frame generation on RTX 30/20 is an unofficial path: it requires [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) ≥0.3.0 (self-signed). 0.3.x is a hook proxy (embedded runtime, bit-identical to official; the 0.2.4 direct-drive contract has been removed — a stale version.dll deployment yields FG off with `fg_detail` reporting 0xBAD0000B, please upgrade)
> - The verified baseline is the RTX 3080; other tiers are theoretical inferences — real-world test feedback is welcome

## Requirements

- Windows 10/11, **NVIDIA RTX GPU** (non-RTX not supported), recent drivers recommended
- mpv-lazy (ships with the VapourSynth R73 runtime)

## Branches

| Branch | Purpose |
|---|---|
| `dlssnr` | **Release branch**: upstream main⊕lite merged baseline + DLSSNR customization (config, plugin source, packaging scripts); everything described in the README lives here |
| `Dev` | Plugin development branch (historical line, features merged into `dlssnr`) |
| `main` | Clean mainline synced with upstream |

## Building from source

Dependencies land automatically (`fetch-deps.ps1` pulls from public sources: NGX headers + static libs from the NVIDIA/DLSS official repos, NVOF headers from the mbucchia/Optical-Flow-SDK mirror, VapourSynth R73 headers, ImGui 1.91.9b), no manual downloads:

```powershell
# 1. Fetch dependencies (requires pwsh + curl)
pwsh -File native\scripts\fetch-deps.ps1

# 2. Build (MSVC x64, /MT static CRT; requires Visual Studio + CMake 4.0+)
powershell -File native\scripts\build.ps1          # output: native\bin\{vs_dlssnr.dll, dlssnr_panel.exe}

# 3. Package (must be on the dlssnr branch; place the nvngx_dlssnr.dll model into native\vendor\ngx\ first)
pwsh -File native\scripts\package.ps1 [-Version 2026.09.08]   # output: native\dist\mpv_PlayKit-dlssnr-v<version>-full.zip (version defaults to today's date)
```

For the porting checklist, architecture notes, and pitfall log, see [docs/PORTING.md](docs/PORTING.md).

## Credits & license

- Upstream [hooke007/mpv_PlayKit](https://github.com/hooke007/mpv_PlayKit) and [mpv-lazy](https://github.com/hooke007/mpv_PlayKit/releases) — the configuration baseline of this fork
- [Magpie](https://github.com/SAOG0721/Magpie) (experimental fork, upstream [Blinue/Magpie](https://github.com/Blinue/Magpie)) — DLSSNR reference implementation
- **RenoDX** — source of the `nvngx_dlssnr.dll` model
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS), [VapourSynth](https://github.com/vapoursynth/vapoursynth), [Dear ImGui](https://github.com/ocornut/imgui)

License follows upstream, see [LICENSE.MD](LICENSE.MD).
