@echo off
setlocal
set "INPUT=C:\bake\mjpeg_ai"
set "OUTPUT=C:\bake\mjpeg_ai_dialogue_v2"
python "%~dp0dialogue_emotion_builder.py" --input "%INPUT%" --output "%OUTPUT%"
if errorlevel 1 (
  echo Build failed.
  pause
  exit /b 1
)
echo Build complete: %OUTPUT%
pause
