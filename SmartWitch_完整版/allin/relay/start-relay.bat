@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "NODE="
if exist "%ProgramFiles%\nodejs\node.exe" set "NODE=%ProgramFiles%\nodejs\node.exe"
if not defined NODE if exist "%LOCALAPPDATA%\Programs\nodejs\node.exe" set "NODE=%LOCALAPPDATA%\Programs\nodejs\node.exe"
if not defined NODE if exist "%ProgramFiles(x86)%\nodejs\node.exe" set "NODE=%ProgramFiles(x86)%\nodejs\node.exe"
if not defined NODE (
  for %%I in (node.exe) do if not defined NODE set "NODE=%%~$PATH:I"
)
if not defined NODE if exist "C:\Users\34109\.workbuddy\binaries\node\versions\22.22.2-6\node.exe" set "NODE=C:\Users\34109\.workbuddy\binaries\node\versions\22.22.2-6\node.exe"

if not defined NODE (
  echo.
  echo [ERROR] Node.js not found.
  echo         Install it from https://nodejs.org/ and run this file again.
  echo.
  pause
  exit /b 1
)

echo Using Node: %NODE%
"%NODE%" "%~dp0server.js"
echo.
echo Relay stopped.
pause
