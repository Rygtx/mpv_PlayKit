# Build vs_dlssnr.dll: MSVC x64 + /MT via cmake NMake generator
# Usage: powershell -File native\scripts\build.ps1 [-Clean]
param([switch]$Clean)

$ErrorActionPreference = "Stop"
$native = Split-Path -Parent $PSScriptRoot
# VS 安装位置自动发现(vswhere 随 VS/Build Tools 一起安装);可用 VSDLSSNR_VSROOT 覆盖
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsroot = if ($env:VSDLSSNR_VSROOT) {
    $env:VSDLSSNR_VSROOT
} elseif (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
} else {
    $null
}
if (-not $vsroot) { throw "未找到 Visual Studio 安装;设 VSDLSSNR_VSROOT 指向 VS 安装目录,或安装带 C++ 工具链的 VS" }
$vcvars = Join-Path $vsroot "VC\Auxiliary\Build\vcvarsall.bat"

if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found: $vcvars" }

$buildDir = Join-Path $native "build"
if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir -Confirm:$false
}
New-Item -ItemType Directory -Force $buildDir | Out-Null

# Run through cmd so vcvars environment sticks for cmake+nmake.
# Paths travel via environment variables: baking them into the batch text
# would hit Out-File -Encoding ascii, which mangles non-ASCII paths.
$env:VSDLSSNR_VCVARS = $vcvars
$env:VSDLSSNR_BUILD_DIR = $buildDir
$cmd = @"
call "%VSDLSSNR_VCVARS%" x64 >nul
cd /d "%VSDLSSNR_BUILD_DIR%"
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
nmake || exit /b 1
"@
$cmd | Out-File -Encoding ascii (Join-Path $env:TEMP "vsdlssnr_build.bat")
& cmd /c (Join-Path $env:TEMP "vsdlssnr_build.bat")
exit $LASTEXITCODE
