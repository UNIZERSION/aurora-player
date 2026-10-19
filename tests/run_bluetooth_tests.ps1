param(
    [Parameter(Mandatory = $true)][string]$ZigPath,
    [string]$BuildDir = (Join-Path $env:TEMP 'tab5-bluetooth-tests')
)
$ErrorActionPreference = 'Stop'
$zig = (Resolve-Path -LiteralPath $ZigPath).Path
$project = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$exe = Join-Path $build 'bluetooth_test.exe'
& $zig c++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $project 'main') -I (Join-Path $PSScriptRoot 'host_audio') (Join-Path $PSScriptRoot 'bluetooth_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw "Bluetooth host build failed: $LASTEXITCODE" }
& $exe
if ($LASTEXITCODE -ne 0) { throw "Bluetooth tests failed: $LASTEXITCODE" }
$shutdownExe = Join-Path $build 'bluetooth_shutdown_test.exe'
& $zig c++ -std=c++17 -O2 -Wall -Wextra -Werror -I (Join-Path $project 'main') -I (Join-Path $PSScriptRoot 'host_bluetooth') -I (Join-Path $PSScriptRoot 'host_audio') (Join-Path $PSScriptRoot 'bluetooth_shutdown_test.cpp') -o $shutdownExe
if ($LASTEXITCODE -ne 0) { throw "Bluetooth shutdown host build failed: $LASTEXITCODE" }
& $shutdownExe
if ($LASTEXITCODE -ne 0) { throw "Bluetooth shutdown tests failed: $LASTEXITCODE" }
Write-Host 'Host tests do not validate radio pairing, UART wake pulses or I2S clocks.'
