param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host-transport")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
& $ZigPath c++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$PSScriptRoot/../main" -I "$PSScriptRoot/host_audio" "$PSScriptRoot/transport_policy_test.cpp" -o "$BuildDir/transport-policy.exe"
if ($LASTEXITCODE -ne 0) { throw 'Transport policy build failed' }
& "$BuildDir/transport-policy.exe"
if ($LASTEXITCODE -ne 0) { throw 'Transport policy test failed' }
