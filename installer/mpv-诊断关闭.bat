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
echo == 本插件进程的诊断 dump 位于 %LOCALAPPDATA%\CrashDumps\,
echo == 关闭后可选择一并删除（只删这三个进程的 dump,
echo == 其他应用的转储不受影响）
echo ==========================================================

reg delete "HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\mpv.exe" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\umpv.exe" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\dlssnr_panel.exe" /f >nul 2>&1

echo [校验] 恢复结果:
call :verify mpv.exe
call :verify umpv.exe
call :verify dlssnr_panel.exe

echo.
echo [诊断 dump] 检索本插件进程的转储文件:
powershell -NoProfile -Command "$p=$env:LOCALAPPDATA+'\CrashDumps'; if (Test-Path $p) { $d=@(Get-ChildItem $p -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(mpv|umpv|dlssnr_panel)\.exe\..*\.dmp$' }); if ($d.Count) { $d | ForEach-Object { '  {0}  ({1:N1} MB)' -f $_.Name, ($_.Length/1MB) }; '共 {0} 个文件, {1:N0} MB' -f $d.Count, (($d | Measure-Object Length -Sum).Sum/1MB) } else { '  无' } } else { '  无 dump 目录' }"

choice /C YN /M "是否删除上述诊断 dump 文件"
if errorlevel 2 goto :keep

powershell -NoProfile -Command "$p=$env:LOCALAPPDATA+'\CrashDumps'; if (Test-Path $p) { $d=@(Get-ChildItem $p -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(mpv|umpv|dlssnr_panel)\.exe\..*\.dmp$' }); $n=$d.Count; $s=($d | Measure-Object Length -Sum).Sum; $d | Remove-Item -Force -ErrorAction SilentlyContinue; $left=@(Get-ChildItem $p -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(mpv|umpv|dlssnr_panel)\.exe\..*\.dmp$' }); if ($left.Count -eq 0) { '已删除 {0} 个文件, 释放 {1:N0} MB' -f $n, ($s/1MB) } else { '{0} 个文件被占用未删除, 已释放其余 {1:N0} MB' -f $left.Count, (($s-($left | Measure-Object Length -Sum).Sum)/1MB) } }"
goto :done

:keep
echo   已保留 dump 文件（可稍后手动删除 %LOCALAPPDATA%\CrashDumps\）

:done
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
