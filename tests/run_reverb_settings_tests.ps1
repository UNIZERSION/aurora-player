param([Parameter(Mandatory=$true)][string]$ZigPath,[string]$BuildDir=(Join-Path $env:TEMP 'aurora-reverb-settings-tests'))
$ErrorActionPreference='Stop'
$project=(Resolve-Path "$PSScriptRoot/..").Path
$zig=(Resolve-Path -LiteralPath $ZigPath).Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build=(Resolve-Path -LiteralPath $BuildDir).Path
function Compile([string[]]$Arguments){ & $zig @Arguments; if($LASTEXITCODE -ne 0){throw 'Native reverb test compilation failed'} }
function Run([string]$Exe,[string[]]$Arguments=@()){ & $Exe @Arguments; if($LASTEXITCODE -ne 0){throw "Reverb test failed: $Exe"} }
$common=@('cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',"$project/main")
Compile ($common+@("$PSScriptRoot/audio_reverb_settings_test.c",'-o',"$build/settings.exe"))
Run "$build/settings.exe"
Compile ($common+@("$PSScriptRoot/audio_reverb_band_test.c",'-o',"$build/band.exe"))
Run "$build/band.exe"
Compile ($common+@("$PSScriptRoot/audio_reverb_test.c",'-o',"$build/legacy.exe"))
Run "$build/legacy.exe"
Compile ($common+@('-c',"$project/main/audio_fx.c",'-o',"$build/fx.o"))
Compile @('c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I',"$project/main","$PSScriptRoot/reverb_store_test.cpp","$project/main/reverb_store.cpp","$project/main/player_store.cpp","$build/fx.o",'-o',"$build/store.exe")
Run "$build/store.exe" @((Join-Path $build ([guid]::NewGuid().ToString('N'))))
Compile @('c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I',"$project/main",'-c',"$PSScriptRoot/eq_factory_engine_fixture.cpp",'-o',"$build/factory.o")
Compile ($common+@('-I',"$PSScriptRoot/host_audio",'-I',"$PSScriptRoot/host_codec","$PSScriptRoot/audio_reverb_engine_test.c","$PSScriptRoot/host_audio/runtime.c","$project/main/audio_engine.c","$project/main/audio_fx.c","$project/main/audio_eq.c","$project/main/audio_extra_codec.c","$PSScriptRoot/host_codec/mock_codec.c","$project/main/audio_file.c","$build/factory.o",'-o',"$build/engine.exe"))
Run "$build/engine.exe"
Write-Host 'Reverb DSP, settings persistence and shared engine output passed. Hardware timing/listening is not simulated.'
