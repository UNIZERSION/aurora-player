param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host-stream")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$testOutput = Join-Path (Resolve-Path -LiteralPath $BuildDir).Path 'fixed_rate_stream_test.exe'
& $ZigPath c++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$PSScriptRoot/../main" "$PSScriptRoot/fixed_rate_stream_test.cpp" -o $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Fixed-rate stream test compilation failed' }
& $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Fixed-rate stream tests failed' }
