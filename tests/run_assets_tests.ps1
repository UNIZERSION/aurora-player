param([Parameter(Mandatory=$true)][string]$ZigPath,[string]$BuildDir="$PSScriptRoot/../build-host")
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build=(Resolve-Path $BuildDir).Path
& $ZigPath c++ -O2 -Wall -Wextra -Werror -I "$root/main" "$PSScriptRoot/assets_screenshot_test.cpp" -o "$build/assets-screenshot-test.exe"
if($LASTEXITCODE -ne 0){throw 'Asset test compilation failed'}
& "$build/assets-screenshot-test.exe" "$root/main" "$build/screenshot-test.bmp"
if($LASTEXITCODE -ne 0){throw 'Asset/screenshot tests failed'}
