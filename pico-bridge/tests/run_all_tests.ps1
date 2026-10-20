# Runs every Pico Bluetooth host test with Zig 0.13.0 (zig c++) and Python 3.
#   powershell -NoProfile -ExecutionPolicy Bypass -File pico-bridge\tests\run_all_tests.ps1
# Zig and Python are taken from PATH unless -ZigPath / -PythonPath are given. The Aurora
# headers come from this repository (-AuroraProject defaults to the repository root).
param([string]$ZigPath = '', [string]$PythonPath = '', [string]$AuroraProject = (Join-Path $PSScriptRoot '..\..'))
$ErrorActionPreference='Stop'
$picoRoot=Split-Path $PSScriptRoot -Parent
if(-not $ZigPath){$ZigPath=(Get-Command zig).Source}
if(-not $PythonPath){$PythonPath=(Get-Command python).Source}
& "$PSScriptRoot/run_audit_tests.ps1" -ZigPath $ZigPath -AuroraProject $AuroraProject
& $PythonPath "$PSScriptRoot/run_library_media.py" $ZigPath
if($LASTEXITCODE -ne 0){throw 'Pico actual-library regression failed'}
foreach($test in @('stream_health','airpods_protocol','airpods_transport')) {
  & $ZigPath c++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=undefined -I $PSScriptRoot -I "$picoRoot/src" "$PSScriptRoot/${test}_test.cpp" -o "$PSScriptRoot/$test.exe"
  if($LASTEXITCODE -ne 0){throw "$test compilation failed"}
  & "$PSScriptRoot/$test.exe"
  if($LASTEXITCODE -ne 0){throw "$test regression failed"}
}
Write-Output 'All host tests passed. No USB port opened; radio and native NVS migration remain untested.'
