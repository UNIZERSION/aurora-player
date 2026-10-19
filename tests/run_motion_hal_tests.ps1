param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host-motion")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
& $ZigPath cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-variable -I $PSScriptRoot "$PSScriptRoot/test_tab5_motion_hal.c" -o "$BuildDir/motion-hal.exe"
if ($LASTEXITCODE -ne 0) { throw 'Motion HAL build failed' }
& "$BuildDir/motion-hal.exe"
if ($LASTEXITCODE -ne 0) { throw 'Motion HAL test failed' }
