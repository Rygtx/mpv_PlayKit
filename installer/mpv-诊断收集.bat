:: 说明（UTF8中文乱码可修改编码为ANSI或GB18030）
chcp 936

@echo off
setlocal enableextensions enabledelayedexpansion

:: 更改当前工作目录到整合包根目录（脚本随包分发,不依赖任何外部路径配置）
cd /D %~dp0\..

set "OUT=诊断收集"
set "R=%OUT%\report.txt"
set "DUMPSRC=%LOCALAPPDATA%\CrashDumps"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\dumps" mkdir "%OUT%\dumps"
if not exist "%OUT%\wer" mkdir "%OUT%\wer"

echo ==========================================================
echo == 诊断收集（随时可运行,做完即退出,不常驻）
echo ==
echo == 前提:先运行过一次 installer\mpv-诊断设置.bat（开启崩溃转储）
echo ==
echo == 收集内容（有则收,无则跳过,自动发现）:
echo ==   系统硬件/驱动/刷新率/dxdiag/nvidia-smi 全量
echo ==   mpv 与 VapourSynth 版本、插件与配置清单与哈希
echo ==   全部日志原件（report 内为尾部速览）、事件查看器崩溃记录、WER 报告元数据
echo ==   崩溃 dump → %OUT%\dumps\
echo == 最后自动打包为 zip,把该 zip 发回即可
echo ==========================================================

echo [1/6] 收集系统硬件与驱动...
> "%R%" echo ===== mpv_PlayKit 诊断收集 %date% %time% =====
>> "%R%" echo.

>> "%R%" echo ===== Windows 版本 / 区域 =====
>> "%R%" reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v ProductName 2>nul
>> "%R%" reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v DisplayVersion 2>nul
>> "%R%" reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuild 2>nul
>> "%R%" reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v UBR 2>nul
>> "%R%" reg query "HKLM\SYSTEM\CurrentControlSet\Control\Nls\CodePage" /v ACP 2>nul
>> "%R%" echo.

>> "%R%" echo ===== CPU / 内存 / 电源计划 =====
>> "%R%" powershell -NoProfile -Command "Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors | Format-List; '内存(GB): ' + [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,1)"
>> "%R%" powercfg /getactivescheme
>> "%R%" echo.

>> "%R%" echo ===== 显卡与驱动（含当前输出分辨率/刷新率） =====
>> "%R%" powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate | Format-List"
if exist "%SystemRoot%\System32\nvidia-smi.exe" (
    >> "%R%" "%SystemRoot%\System32\nvidia-smi.exe" --query-gpu=name,driver_version --format=csv
) else (
    >> "%R%" echo nvidia-smi 不存在（驱动未装或非 N 卡）
)
>> "%R%" echo.

>> "%R%" echo ===== NVIDIA 安装目录清单 =====
if exist "%ProgramFiles%\NVIDIA Corporation" (
    >> "%R%" dir "%ProgramFiles%\NVIDIA Corporation" /b
) else (
    >> "%R%" echo 目录不存在
)
>> "%R%" echo.

echo [2/6] 生成 dxdiag 报告（约10至30秒,请稍候）...
del "%OUT%\dxdiag.txt" 2>nul
powershell -NoProfile -Command "Start-Process dxdiag -ArgumentList '/whql:off','/t','%OUT%\dxdiag.txt' -Wait; for($i=0;$i -lt 30 -and -not (Test-Path '%OUT%\dxdiag.txt');$i++){Start-Sleep 2}"
echo        生成 nvidia-smi 全量信息...
if exist "%SystemRoot%\System32\nvidia-smi.exe" (
    "%SystemRoot%\System32\nvidia-smi.exe" -q > "%OUT%\nvidia-smi-q.txt" 2>&1
)

echo [3/6] 收集 mpv 与插件版本...
>> "%R%" echo ===== mpv 版本 =====
if exist "mpv.com" (
    >> "%R%" mpv.com --version 2>&1
) else (
    >> "%R%" echo mpv.com 不存在
)
>> "%R%" echo.

>> "%R%" echo ===== Python / VapourSynth =====
if exist "python.exe" (
    >> "%R%" python.exe -c "import sys; print(sys.version)" 2>&1
    >> "%R%" python.exe -c "import vapoursynth; c=vapoursynth.core; print('VapourSynth', vapoursynth.__version__, '| core', c.version()); print('threads', c.num_threads())" 2>&1
) else (
    >> "%R%" echo python.exe 不存在
)
>> "%R%" echo.

>> "%R%" echo ===== 关键文件版本信息 =====
>> "%R%" powershell -NoProfile -Command "Get-Item 'vs-plugins\vs_dlssnr.dll','vs-plugins\dlssnr_panel.exe','mpv.exe','python.exe' -ErrorAction SilentlyContinue | ForEach-Object { '{0}  {1}' -f $_.Name, $_.VersionInfo.FileVersion }"
>> "%R%" echo.

>> "%R%" echo ===== 关键文件哈希 MD5 =====
>> "%R%" powershell -NoProfile -Command "Get-FileHash -Algorithm MD5 -ErrorAction SilentlyContinue -Path 'vs-plugins\vs_dlssnr.dll','vs-plugins\dlssnr_panel.exe','vs-plugins\ngx\nvngx_dlssnr.dll','vs-plugins\ngx\nvngx_vsr.dll','vs-plugins\ngx\nvngx_truehdr.dll','vs-plugins\ngx\nvngx_dlssg.dll','vs-plugins\ngx\version.dll' | Format-List Path,Hash"
>> "%R%" echo.

echo [4/6] 收集插件与配置清单...
>> "%R%" echo ===== 根目录清单（日期/大小可暴露文件版本错位） =====
>> "%R%" dir /O-D
>> "%R%" echo.
>> "%R%" echo ===== vs-plugins 清单 =====
>> "%R%" dir vs-plugins
>> "%R%" echo.
>> "%R%" echo ===== vs-plugins\ngx 清单 =====
>> "%R%" dir vs-plugins\ngx
>> "%R%" echo.
>> "%R%" echo ===== portable_config 全树清单 =====
>> "%R%" dir portable_config /s /b 2>nul
>> "%R%" echo.

echo [5/6] 收集日志尾部...
for %%F in ("*.log" "vs-plugins\*.log" "vs-plugins\ngx\*.log" "portable_config\*.log") do (
    >> "%R%" echo ----- 日志 %%~F 尾部 200 行 -----
    >> "%R%" powershell -NoProfile -Command "Get-Content -LiteralPath '%%~F' -Tail 200 -Encoding UTF8 -ErrorAction SilentlyContinue"
    >> "%R%" echo.
)

>> "%R%" echo ===== 事件查看器崩溃记录（Application,mpv/dlssnr 相关,最多 8 条） =====
>> "%R%" powershell -NoProfile -Command "try { Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000,1001,1002} -MaxEvents 300 -ErrorAction Stop | Where-Object { $_.Message -match 'mpv|dlssnr' } | Select-Object -First 8 | Format-List TimeCreated,Message } catch { '无相关记录' }"
>> "%R%" echo.

>> "%R%" echo ===== 事件查看器显卡驱动记录（System,4101/nvlddmkm/dxgkrnl,最多 8 条） =====
>> "%R%" powershell -NoProfile -Command "try { Get-WinEvent -FilterHashtable @{LogName='System'} -MaxEvents 500 -ErrorAction Stop | Where-Object { $_.Id -eq 4101 -or $_.ProviderName -match 'nvlddmkm|dxgkrnl' } | Select-Object -First 8 | Format-List TimeCreated,Id,ProviderName,Message } catch { '无相关记录' }"
>> "%R%" echo.

echo [6/6] 复制配置原件与崩溃 dump...
for %%F in ("*.log" "vs-plugins\*.log" "vs-plugins\ngx\*.log" "portable_config\*.log" "mpv_*.txt") do (
    copy /Y "%%~F" "%OUT%\" >nul 2>nul
)
copy /Y "vs-plugins\dlssnr_ui.ini" "%OUT%\" >nul 2>nul
copy /Y "vs-plugins\ngx\dlssg_sm86.ini" "%OUT%\" >nul 2>nul
copy /Y "umpv.conf" "%OUT%\" >nul 2>nul
copy /Y "portable_config\mpv.conf" "%OUT%\" >nul 2>nul
copy /Y "portable_config\profiles.conf" "%OUT%\" >nul 2>nul
copy /Y "portable_config\script-opts.conf" "%OUT%\" >nul 2>nul
xcopy "portable_config\vs" "%OUT%\portable_config_vs\" /e /i /y >nul 2>nul

if exist "%DUMPSRC%" (
    for %%F in ("%DUMPSRC%\mpv.exe.*.dmp" "%DUMPSRC%\umpv.exe.*.dmp" "%DUMPSRC%\dlssnr_panel.exe.*.dmp") do (
        copy /Y "%%~F" "%OUT%\dumps\" >nul 2>nul && echo   已收到 dump: %%~nxF
    )
)
dir /b "%OUT%\dumps\*.dmp" 2>nul | findstr . >nul || echo   （dumps 为空:尚未闪退,或未运行过诊断设置.bat）

for %%D in ("%SystemRoot%\..\ProgramData\Microsoft\Windows\WER\ReportArchive" "%SystemRoot%\..\ProgramData\Microsoft\Windows\WER\ReportQueue") do (
    for /d %%E in ("%%~D\AppCrash_mpv*" "%%~D\AppCrash_umpv*" "%%~D\AppCrash_dlssnr*") do (
        copy /Y "%%~E\Report.wer" "%OUT%\wer\%%~nxE.wer" >nul 2>nul
    )
)

echo 正在打包...
powershell -NoProfile -Command "try { Compress-Archive -Path '%OUT%\*' -DestinationPath ('%OUT%-{0:yyyyMMdd-HHmm}.zip' -f (Get-Date)) -Force -ErrorAction Stop; '打包完成' } catch { '打包失败,请手动压缩文件夹' }"

echo ==========================================================
echo == 收集完成
echo == 请把 "%OUT%-日期.zip" 发回（若不存在则压缩 "%OUT%" 文件夹）
echo == 内容:report.txt / dxdiag.txt / nvidia-smi-q.txt / dumps\ /
echo ==       wer\ / 全部日志原件 / 配置与滤镜脚本原件
echo ==========================================================
pause
