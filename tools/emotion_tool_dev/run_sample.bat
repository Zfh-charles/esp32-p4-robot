@echo off
setlocal
set "TOOL_DIR=%~dp0"
set "INPUT_DIR=C:\Users\0000\Documents\朱迪\mjpeg"
set "OUTPUT_DIR=C:\bake\emotion-roi-output"

where python >nul 2>nul
if %errorlevel%==0 (
  set "PYTHON_EXE=python"
) else (
  set "PYTHON_EXE=C:\Users\0000\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
)

"%PYTHON_EXE%" "%TOOL_DIR%emotion_roi_builder.py" --input "%INPUT_DIR%" --output "%OUTPUT_DIR%"
if errorlevel 1 exit /b %errorlevel%
echo.
echo Done: %OUTPUT_DIR%
endlocal
