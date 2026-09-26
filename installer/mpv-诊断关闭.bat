:: 说明（UTF8中文乱码可修改编码为ANSI或GB18030）
chcp 936

@echo off
setlocal enableextensions

echo ==========================================================
echo == 诊断关闭（恢复默认；无需管理员权限）
echo ==
echo == 删除 mpv.exe / umpv.exe / dlssnr_panel.exe 的崩溃转储配置,
echo == 之后闪退不再生成 dump（回到 Windows 默认行为）
echo ==
echo == 已生成的 dump 不受影响,位于:
echo ==   %LOCALAPPDATA%\CrashDumps\
echo == 确认反馈已发出后,可手动删除该目录释放空间
echo ==========================================================

reg delete "HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\mpv.exe" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\umpv.exe" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\dlssnr_panel.exe" /f >nul 2>&1

echo [校验] 恢复结果:
call :verify mpv.exe
call :verify umpv.exe
call :verify dlssnr_panel.exe

echo.
powershell -NoProfile -Command "$p=$env:LOCALAPPDATA+'\CrashDumps'; if (Test-Path $p) { $d=@(Get-ChildItem $p -Filter *.dmp -ErrorAction SilentlyContinue); if ($d.Count) { '现有 dump: {0} 个文件, 共 {1:N0} MB' -f $d.Count, (($d | Measure-Object Length -Sum).Sum/1MB) } else { '无 dump 文件' } } else { '无 dump 目录' }"

echo ==========================================================
echo == 已恢复默认
echo ==========================================================
pause
exit /b 0

:verify
reg query "HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\%~1" >nul 2>&1
if errorlevel 1 (
    echo   [OK] %~1 已关闭
) else (
    echo   [失败] %~1 仍存在,请截图反馈
)
exit /b 0
