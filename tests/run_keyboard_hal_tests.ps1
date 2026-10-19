param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host-keyboard")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$hal = "$PSScriptRoot/../components/tab5_hal"
& $ZigPath cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-variable -I $PSScriptRoot -I $hal -I "$hal/include" "$PSScriptRoot/test_tab5_keyboard_hal.c" "$hal/tab5_keyboard_protocol.c" -o "$BuildDir/keyboard-hal.exe"
if ($LASTEXITCODE -ne 0) { throw 'Keyboard HAL build failed' }
& "$BuildDir/keyboard-hal.exe"
if ($LASTEXITCODE -ne 0) { throw 'Keyboard HAL test failed' }
