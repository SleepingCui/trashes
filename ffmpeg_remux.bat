@echo off
setlocal enabledelayedexpansion

if "%~1"=="" (
    echo Error: Input file path is required.
    echo Usage: %~n0 ^<input_file^> [output_file]
    exit /b 1
)

set "INPUT_FILE=%~1"
set "OUTPUT_FILE=%~2"

if "%OUTPUT_FILE%"=="" (
    for %%A in ("!INPUT_FILE!") do (
        set "BASE_NAME=%%~nA"
        set "INPUT_DIR=%%~dpA"
    )
    set "OUTPUT_FILE=!INPUT_DIR!!BASE_NAME!_remuxed.mp4"
)

echo Input:  "!INPUT_FILE!"
echo Output: "!OUTPUT_FILE!"

ffmpeg -i "!INPUT_FILE!" -map 0 -c copy "!OUTPUT_FILE!"

if errorlevel 1 (
    exit /b 1
) else (
    echo Done! 
    echo Saved to: "!OUTPUT_FILE!"
)

endlocal