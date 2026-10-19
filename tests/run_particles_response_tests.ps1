param([Parameter(Mandatory=$true)][string]$ZigPath,[string]$BuildDir="$PSScriptRoot/../build-host/particles")
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build=(Resolve-Path $BuildDir).Path
& $ZigPath c++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/main" "$PSScriptRoot/particles_response_test.cpp" -o "$build/particles_response_test.exe"
if($LASTEXITCODE -ne 0){throw 'Particle response test compilation failed'}
& "$build/particles_response_test.exe"
if($LASTEXITCODE -ne 0){throw 'Particle response tests failed'}
