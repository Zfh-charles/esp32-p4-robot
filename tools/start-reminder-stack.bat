@echo off

REM 一键开 4 个窗口：推理 API + 定时 push + ngrok + mcp_pipe

REM 用法: tools\start-reminder-stack.bat



set ROOT=%~dp0

set API_DIR=%ROOT%inference-api-demo

set MCP_DIR=%ROOT%mcp-calculator



echo Checking ngrok version ...

ngrok version

echo.



start "Reminder-API" cmd /k "cd /d %API_DIR% && run-local-no-seed.bat"

timeout /t 2 /nobreak >nul



start "Periodic-Push" cmd /k "cd /d %API_DIR% && run-periodic-push.bat"

timeout /t 1 /nobreak >nul



start "ngrok" cmd /k "ngrok http 8765"

timeout /t 1 /nobreak >nul



start "mcp-pipe" cmd /k "cd /d %MCP_DIR% && python mcp_pipe.py backend_alert.py"



echo.

echo Started 4 windows: API, periodic push, ngrok, mcp_pipe

echo ngrok URL must match sdkconfig REMINDER_POLL_DEFAULT_URL

echo Device idle - first TTS within ~30s after each push

