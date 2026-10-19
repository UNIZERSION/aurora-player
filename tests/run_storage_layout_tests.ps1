param([Parameter(Mandatory=$true)][string]$ZigPath,[string]$BuildDir=(Join-Path $env:TEMP 'aurora-storage-layout-tests'))
$ErrorActionPreference='Stop'
$project=(Resolve-Path "$PSScriptRoot/..").Path
$zig=(Resolve-Path -LiteralPath $ZigPath).Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build=(Resolve-Path -LiteralPath $BuildDir).Path
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $build 'zig-global'
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $build 'zig-local'
function Compile([string[]]$Arguments){ & $zig @Arguments; if($LASTEXITCODE -ne 0){throw 'Storage migration test compilation failed'} }
Compile @('cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',"$project/main",'-c',"$project/main/audio_fx.c",'-o',"$build/fx.o")
Compile @('cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',"$project/main",'-c',"$project/main/audio_eq.c",'-o',"$build/eq.o")
Compile @('c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I',"$project/main","$PSScriptRoot/storage_layout_test.cpp","$project/main/storage_layout.cpp","$project/main/player_store.cpp","$project/main/eq_store.cpp","$project/main/reverb_store.cpp","$project/main/favorites_store.cpp","$build/fx.o","$build/eq.o",'-o',"$build/storage.exe")
& "$build/storage.exe" (Join-Path $build ([guid]::NewGuid().ToString('N')))
if($LASTEXITCODE -ne 0){throw 'Storage migration tests failed'}
