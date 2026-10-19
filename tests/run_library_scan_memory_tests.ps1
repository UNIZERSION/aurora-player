param([Parameter(Mandatory=$true)][string]$ZigPath,[string]$BuildDir="$PSScriptRoot/../build-host/scan-memory")
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build=(Resolve-Path -LiteralPath $BuildDir).Path
& $ZigPath cc -O1 -std=c11 -I "$root/main" -c "$root/main/audio_file.c" -o "$build/audio_file.o"
if($LASTEXITCODE -ne 0){throw 'Audio parser compilation failed'}
& $ZigPath c++ -O1 -std=c++17 -Wall -Wextra -Werror -DAURORA_SCAN_MEMORY_TEST -I "$root/main" "$PSScriptRoot/library_scan_memory_test.cpp" "$root/main/music_library.cpp" "$root/main/player_store.cpp" "$root/main/favorites_store.cpp" "$build/audio_file.o" -o "$build/scan-memory.exe"
if($LASTEXITCODE -ne 0){throw 'Library memory test compilation failed'}
& "$build/scan-memory.exe" (Join-Path $build ([Guid]::NewGuid().ToString('N')))
if($LASTEXITCODE -ne 0){throw 'Library scan memory tests failed'}
