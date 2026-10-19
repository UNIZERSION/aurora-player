param(
    [Parameter(Mandatory = $true)][string]$ZigPath,
    [Parameter(Mandatory = $true)][string]$FirmwareBuildDir,
    [string]$BuildDir = (Join-Path $env:TEMP 'tab5-exfat-host-test')
)
$ErrorActionPreference = 'Stop'
$zig = (Resolve-Path -LiteralPath $ZigPath).Path
$firmwareBuild = (Resolve-Path -LiteralPath $FirmwareBuildDir).Path
$fatfs = Join-Path $firmwareBuild 'tab5_components/fatfs/src'
$config = Join-Path $firmwareBuild 'config'
foreach ($required in @((Join-Path $fatfs 'ff.c'), (Join-Path $fatfs 'ffunicode.c'), (Join-Path $config 'sdkconfig.h'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing $required. Configure/build the firmware first." }
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$exe = Join-Path $build 'exfat_memory_test.exe'
$arguments = @(
    'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-DportTICK_PERIOD_MS=1',
    '-include', (Join-Path $PSScriptRoot 'host_audio/posix_compat.h'),
    '-I', $config, '-I', (Join-Path $PSScriptRoot 'host_audio'), '-I', $fatfs,
    (Join-Path $PSScriptRoot 'exfat_memory_test.c'),
    (Join-Path $fatfs 'ff.c'), (Join-Path $fatfs 'ffunicode.c'), '-o', $exe
)
& $zig @arguments
if ($LASTEXITCODE -ne 0) { throw "exFAT host build failed with exit code $LASTEXITCODE" }
& $exe
if ($LASTEXITCODE -ne 0) { throw "exFAT test failed with exit code $LASTEXITCODE" }
$streamExe = Join-Path $build 'audio_stream_fatfs_test.exe'
$streamArguments = @(
    'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-DportTICK_PERIOD_MS=1',
    '-include', (Join-Path $PSScriptRoot 'host_audio/posix_compat.h'),
    '-I', $config, '-I', (Join-Path $PSScriptRoot 'host_audio'), '-I', $fatfs,
    (Join-Path $PSScriptRoot 'audio_stream_fatfs_test.c'),
    '-I', (Join-Path $PSScriptRoot 'host_codec'),
    (Join-Path $PSScriptRoot 'host_audio/runtime.c'),
    (Join-Path $PSScriptRoot 'host_codec/mock_codec.c'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'main/audio_fx.c'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'main/audio_eq.c'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'main/audio_extra_codec.c'),
    (Join-Path (Split-Path $PSScriptRoot -Parent) 'main/audio_file.c'),
    (Join-Path $fatfs 'ff.c'), (Join-Path $fatfs 'ffunicode.c'), '-o', $streamExe
)
& $zig @streamArguments
if ($LASTEXITCODE -ne 0) { throw "Audio stream FatFs build failed with exit code $LASTEXITCODE" }
& $streamExe
if ($LASTEXITCODE -ne 0) { throw "Audio stream FatFs test failed with exit code $LASTEXITCODE" }
Write-Host 'Test used only a private RAM disk. Physical SD and ESP VFS remain hardware checks.'
