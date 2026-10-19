param([Parameter(Mandatory=$true)][string]$ZigPath,[Parameter(Mandatory=$true)][string]$BuildDir)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build=(Resolve-Path $BuildDir).Path
& $ZigPath cc -std=c11 -O1 -Wall -Wextra -Werror -I "$root/main" -c "$root/main/audio_eq.c" -o "$build/audio_eq.o"
if($LASTEXITCODE -ne 0){throw 'EQ DSP compilation failed'}
& $ZigPath c++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$root/main" "$root/tests/eq_store_test.cpp" "$root/main/player_store.cpp" "$build/audio_eq.o" -o "$build/eq_store_test.exe"
if($LASTEXITCODE -ne 0){throw 'EQ store test compilation failed'}
& "$build/eq_store_test.exe" (Join-Path $build ([Guid]::NewGuid().ToString('N')))
if($LASTEXITCODE -ne 0){throw 'EQ store test failed'}
