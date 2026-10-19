param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host-lock")
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force $BuildDir | Out-Null
$buildRoot = (Resolve-Path $BuildDir).Path
& $ZigPath c++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$projectRoot/tests/screen_lock_stubs" -I "$projectRoot/main" "$projectRoot/tests/screen_lock_test.cpp" "$projectRoot/main/screen_lock.cpp" "$projectRoot/main/screen_lock_storage.cpp" -o "$buildRoot/screen_lock_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Screen lock test compilation failed' }
& "$buildRoot/screen_lock_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Screen lock tests failed' }
& $ZigPath c++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$projectRoot/tests/screen_lock_stubs" -I "$projectRoot/main" "$projectRoot/tests/screen_lock_controller_test.cpp" "$projectRoot/main/screen_lock.cpp" "$projectRoot/main/screen_lock_storage.cpp" -o "$buildRoot/screen_lock_controller_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Screen lock controller test compilation failed' }
& "$buildRoot/screen_lock_controller_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Screen lock controller tests failed' }
