@echo off
setlocal enabledelayedexpansion

:: Get the directory where this batch file is located
set "BAT_DIR=%~dp0"
:: Remove trailing backslash
if "%BAT_DIR:~-1%"=="\" set "BAT_DIR=%BAT_DIR:~0,-1%"

:: Check if a link was provided
if "%~1"=="" (
    echo Usage: ytdl ^<link^>
    echo Example: ytdl https://www.youtube.com/watch?v=xxxxxxxxxxx
    exit /b 1
)

:: Check if yt-dlp.exe exists
if not exist "%BAT_DIR%\yt-dlp.exe" (
    echo Error: Cannot find "%BAT_DIR%\yt-dlp.exe"
    echo Please place yt-dlp.exe in the same directory
    exit /b 1
)

:: Check if yt-dlp.conf exists (optional - warning only)
if not exist "%BAT_DIR%\yt-dlp.conf" (
    echo Warning: Config file "%BAT_DIR%\yt-dlp.conf" not found, using default settings
)

:: Start download
echo Starting download: %~1
echo Using config: %BAT_DIR%\yt-dlp.conf
echo.

"%BAT_DIR%\yt-dlp.exe" --config-location "%BAT_DIR%\yt-dlp.conf" %*

:: Check result
if errorlevel 1 (
    echo.
    echo Download failed. Please check your link or network connection.
    exit /b 1
) else (
    echo.
    echo Download completed!
    exit /b 0
)