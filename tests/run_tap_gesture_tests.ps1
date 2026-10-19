param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$testOutput = Join-Path (Resolve-Path -LiteralPath $BuildDir).Path 'tap_gesture_test.exe'
& $ZigPath c++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$PSScriptRoot/../main" "$PSScriptRoot/tap_gesture_test.cpp" "$PSScriptRoot/../main/tap_gesture.cpp" -o $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Tap gesture test compilation failed' }
& $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Tap gesture tests failed' }
