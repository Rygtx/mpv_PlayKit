:: 说明（UTF8中文乱码可修改编码为ANSI或GB18030）
chcp 936

@echo off
setlocal enableextensions

echo ==========================================================
echo == 诊断设置（一次性；无需管理员权限）
echo ==
echo == 开启 Windows 崩溃转储（WER LocalDumps，写入 HKCU 仅当前用户生效）：
echo ==   mpv.exe / umpv.exe / dlssnr_panel.exe 闪退时，系统错误报告
echo ==   服务（系统自带、常驻）自动在下方目录生成完整 dump：
echo ==     %LOCALAPPDATA%\CrashDumps\
echo ==   本脚本不启动任何常驻进程，写完注册表即退出
echo ==   dump 最多保留 10 份，超出自动滚动删除旧文件
echo ==========================================================

set "WERKEY=HKCU\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps"

reg add "%WERKEY%\mpv.exe"           /v DumpType  /t REG_DWORD /d 2 /f >nul
reg add "%WERKEY%\mpv.exe"           /v DumpCount /t REG_DWORD /d 10 /f >nul
reg add "%WERKEY%\umpv.exe"          /v DumpType  /t REG_DWORD /d 2 /f >nul
reg add "%WERKEY%\umpv.exe"          /v DumpCount /t REG_DWORD /d 10 /f >nul
reg add "%WERKEY%\dlssnr_panel.exe"  /v DumpType  /t REG_DWORD /d 2 /f >nul
reg add "%WERKEY%\dlssnr_panel.exe"  /v DumpCount /t REG_DWORD /d 10 /f >nul

echo [校验] 写入结果：
call :verify mpv.exe
call :verify umpv.exe
call :verify dlssnr_panel.exe

echo ==========================================================
echo == 设置完成
echo == 下一步：
echo ==   1. 正常使用，直到复现一次闪退
echo ==   2. 运行 installer\mpv-诊断收集.bat
echo ==   3. 把生成的"诊断收集"文件夹整体压缩后发回
echo == 注：若闪退后 CrashDumps 目录仍无 dump（系统策略关闭了
echo ==     WER 的少见情况），发事件查看器记录即可，收集脚本已包含
echo ==========================================================
pause
exit /b 0

:verify
reg query "%WERKEY%\%~1" /v DumpType >nul 2>&1
if errorlevel 1 (
    echo   [失败] %~1 写入失败，请截图本窗口反馈
) else (
    echo   [OK] %~1
)
exit /b 0
