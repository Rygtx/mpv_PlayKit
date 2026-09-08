# 面板 UI 探针工具集(独立进程场景:枚举窗口/截图/尺寸轮询/DPI/命中测试)。
# 不是断言脚本,是交互排查工具 —— 面板是 ImGui 自绘 UI,没有控件树可查,
# 只能靠 Win32 枚举 + PrintWindow 截图 + 命中测试。全部按窗口类名
# vs_dlssnr_panel_app 定位,不依赖安装路径。
#
# 用法(任一;面板未运行时需设 VSDLSSNR_TEST_ROOT 指向部署根):
#   powershell -File testkit\panel_ui_tools.ps1 -Action capture  [-OutFile panel_ui.png]
#   powershell -File testkit\panel_ui_tools.ps1 -Action size
#   powershell -File testkit\panel_ui_tools.ps1 -Action poll     [-Seconds 5]
#   powershell -File testkit\panel_ui_tools.ps1 -Action dpi
#   powershell -File testkit\panel_ui_tools.ps1 -Action hittest
#   powershell -File testkit\panel_ui_tools.ps1 -Action windows
param(
    [Parameter(Mandatory = $true)][ValidateSet("capture", "size", "poll", "dpi", "hittest", "windows")]
    [string]$Action,
    [string]$OutFile = "panel_ui.png",
    [int]$Seconds = 5
)

$ErrorActionPreference = "Stop"

# --- 部署根解析:VSDLSSNR_TEST_ROOT 优先,否则假设面板已在运行(按进程定位) ---
$panelProc = Get-Process dlssnr_panel -ErrorAction SilentlyContinue
if (-not $panelProc) {
    $root = $env:VSDLSSNR_TEST_ROOT
    if (-not $root) { throw 'panel not running and VSDLSSNR_TEST_ROOT not set' }
    $panelExe = Join-Path $root 'vs-plugins\dlssnr_panel.exe'
    Start-Process $panelExe
    Start-Sleep 3
    $panelProc = Get-Process dlssnr_panel -ErrorAction SilentlyContinue
}

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class PanelUI {
  public delegate bool EnumCb(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

$script:hwnd = [IntPtr]::Zero
$cb = [PanelUI+EnumCb]{ param($h, $l)
  $wpid = 0
  [PanelUI]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
  if ($wpid -eq $panelProc.Id) {
    $c = New-Object System.Text.StringBuilder 128
    [PanelUI]::GetClassNameW($h, $c, 128) | Out-Null
    if ($c.ToString() -eq "vs_dlssnr_panel_app") { $script:hwnd = $h; return $false }
  }
  return $true
}
[PanelUI]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:hwnd -eq [IntPtr]::Zero) { throw "panel window not found (pid $($panelProc.Id))" }

switch ($Action) {
  "windows" {
    # 枚举面板进程全部顶层窗口(可见性/类名/标题)
    $found = @()
    $cb2 = [PanelUI+EnumCb]{ param($h, $l)
      $wpid = 0
      [PanelUI]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
      if ($wpid -eq $panelProc.Id) {
        $t = New-Object System.Text.StringBuilder 128
        $c = New-Object System.Text.StringBuilder 128
        [PanelUI]::GetWindowTextW($h, $t, 128) | Out-Null
        [PanelUI]::GetClassNameW($h, $c, 128) | Out-Null
        $found += "hwnd=$h vis=$([PanelUI]::IsWindowVisible($h)) class='$($c)' title='$($t)'"
      }
      return $true
    }
    [PanelUI]::EnumWindows($cb2, [IntPtr]::Zero) | Out-Null
    $found
  }
  "size" {
    $r = New-Object PanelUI+RECT
    [PanelUI]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
    "window $($r.R - $r.L) x $($r.B - $r.T) at $($r.L),$($r.T)"
  }
  "poll" {
    # 尺寸随时间轮询(观察动画/延迟 resize;面板 idle 时脏检查 ~2fps)
    for ($i = 0; $i -lt ($Seconds * 10); ++$i) {
      $r = New-Object PanelUI+RECT
      [PanelUI]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
      "t=$([math]::Round($i * 0.1, 1))s  $($r.R - $r.L) x $($r.B - $r.T)"
      Start-Sleep -Milliseconds 100
    }
  }
  "dpi" {
    $r = New-Object PanelUI+RECT
    [PanelUI]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
    $dpi = [PanelUI]::GetDpiForWindow($script:hwnd)
    "window: $($r.R - $r.L) x $($r.B - $r.T), GetDpiForWindow: $dpi (per-monitor-v2 set in code)"
  }
  "hittest" {
    # WM_NCHITTEST 探针:区分客户区(1=CLIENT)与标题栏(2=CAPTION)
    $r = New-Object PanelUI+RECT
    [PanelUI]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
    foreach ($pt in @(@(50, 20), @(230, 20), @(415, 19), @(370, 19), @(230, 200), @(230, 330))) {
      $lp = [IntPtr]((($pt[1]) -shl 16) -bor (($pt[0]) -band 0xFFFF))
      $ht = [PanelUI]::SendMessageW($script:hwnd, 0x0084, [IntPtr]::Zero, $lp)
      "client($($pt[0]),$($pt[1])) -> HT=$ht (1=CLIENT 2=CAPTION)"
    }
  }
  "capture" {
    # PrintWindow + PW_RENDERFULLCONTENT:ImGui D3D11 自绘内容必须用此标志
    [PanelUI]::ShowWindow($script:hwnd, 5) | Out-Null
    Start-Sleep 2
    $r = New-Object PanelUI+RECT
    [PanelUI]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h = $r.B - $r.T
    "window ${w}x$h at $($r.L),$($r.T)"
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [PanelUI]::PrintWindow($script:hwnd, $dc, 2) | Out-Null
    $g.ReleaseHdc($dc)
    $g.Dispose()
    $bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
    "saved $OutFile"
  }
}
