@echo off
REM Short-path build to avoid Windows "command line too long" error
REM Usage: tools\build-flash.bat [COM port, default COM6]

set PORT=%1
if "%PORT%"=="" set PORT=COM6

set PROJ=C:\Users\0000\Documents\esp32-p4-i2cpolling\esp32-p4-i2cpolling

REM Map short drive letter (skip if already mapped)
subst X: %PROJ% 2>nul

call C:\esp_alm\v5.4.1\esp-idf\export.bat
cd /d X:\

echo Building from X:\ (short path, no ccache) ...
idf.py -DCCACHE_ENABLE=0 -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON build
if errorlevel 1 exit /b 1

echo Freeing %PORT% from stale idf_monitor ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJ%\tools\free-com-port.ps1" -Port %PORT%

echo Flashing to %PORT% ...
idf.py -p %PORT% flash monitor
