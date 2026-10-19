param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host")
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force $BuildDir | Out-Null
$buildRoot = (Resolve-Path $BuildDir).Path
& $ZigPath cc -std=c11 -Wall -Wextra -I "$projectRoot/main" -c "$projectRoot/main/audio_file.c" -o "$buildRoot/audio_file.o"
if ($LASTEXITCODE -ne 0) { throw 'Audio parser compilation failed' }
& $ZigPath c++ -std=c++17 -Wall -Wextra -I "$projectRoot/main" "$projectRoot/tests/core_test.cpp" "$projectRoot/main/player_store.cpp" "$projectRoot/main/favorites_store.cpp" "$projectRoot/main/music_library.cpp" "$buildRoot/audio_file.o" -o "$buildRoot/core_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Core test compilation failed' }
$fixtureDir = Join-Path $buildRoot ([Guid]::NewGuid().ToString('N'))
& "$buildRoot/core_test.exe" $fixtureDir
if ($LASTEXITCODE -ne 0) { throw 'Core tests failed' }
& $ZigPath c++ -std=c++17 -Wall -Wextra -Werror -I "$projectRoot/main" "$projectRoot/tests/favorites_test.cpp" "$projectRoot/main/player_store.cpp" "$projectRoot/main/favorites_store.cpp" "$projectRoot/main/music_library.cpp" "$buildRoot/audio_file.o" -o "$buildRoot/favorites_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Favorites test compilation failed' }
& "$buildRoot/favorites_test.exe" (Join-Path $buildRoot ([Guid]::NewGuid().ToString('N')))
if ($LASTEXITCODE -ne 0) { throw 'Favorites tests failed' }
