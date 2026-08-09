$ErrorActionPreference = "Stop"

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$gameDir = "E:\Games\Steam\steamapps\common\A Dance of Fire and Ice"
$modDir = Join-Path $gameDir "Mods\TimingShow"

Set-Location $projectDir
msbuild TimingShow.sln /p:Configuration=Release /p:Platform="Any CPU"
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

New-Item -ItemType Directory -Force -Path $modDir | Out-Null
Copy-Item -Path ".\TimingShow\bin\Release\*" -Destination $modDir -Recurse -Force

Start-Sleep -Seconds 1

Set-Location $gameDir
Start-Process ".\A Dance of Fire and Ice.exe"

$log = "$env:USERPROFILE\AppData\LocalLow\7th Beat Games\A Dance of Fire and Ice\Player.log"
while (!(Test-Path $log)) { Start-Sleep -Milliseconds 500 }

$job = Start-Job -ScriptBlock {
    param($logPath)
    Get-Content $logPath -Wait -Encoding UTF8
} -ArgumentList $log

while ($true) {
    $process = Get-Process -Name "A Dance of Fire and Ice" -ErrorAction SilentlyContinue
    if (-not $process) {
        Stop-Job $job
        Remove-Job $job
        break
    }
    Receive-Job $job
    Start-Sleep -Milliseconds 500
}