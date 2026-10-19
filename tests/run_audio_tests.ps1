param(
    [Parameter(Mandatory = $true)][string]$ZigPath,
    [string]$BuildDir = (Join-Path $env:TEMP 'tab5-audio-host-tests')
)
$ErrorActionPreference = 'Stop'
$testRoot = $PSScriptRoot
$projectRoot = Split-Path $testRoot -Parent
$zig = (Resolve-Path -LiteralPath $ZigPath).Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build = (Resolve-Path -LiteralPath $BuildDir).Path

function Invoke-Checked([string[]]$Arguments) {
    & $zig @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Zig build failed with exit code $LASTEXITCODE" }
}

$common = @('cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-I', (Join-Path $projectRoot 'main'))
$speedExe = Join-Path $build 'audio_speed_test.exe'
Invoke-Checked ($common + @((Join-Path $testRoot 'audio_speed_test.c'), '-o', $speedExe))
& $speedExe
if ($LASTEXITCODE -ne 0) { throw 'Turntable speed tests failed' }
$parserExe = Join-Path $build 'audio_file_test.exe'
Invoke-Checked ($common + @(
    (Join-Path $testRoot 'audio_file_test.c'),
    (Join-Path $projectRoot 'main/audio_file.c'), '-o', $parserExe
))
& $parserExe (Join-Path $testRoot 'fixtures/selftest.flac') (Join-Path $testRoot 'fixtures/selftest.aac')
if ($LASTEXITCODE -ne 0) { throw "Audio parser tests failed with exit code $LASTEXITCODE" }

$boundsExe = Join-Path $build 'audio_probe_bounds_test.exe'
Invoke-Checked ($common + @((Join-Path $testRoot 'audio_probe_bounds_test.c'), '-o', $boundsExe))
& $boundsExe
if ($LASTEXITCODE -ne 0) { throw "Audio probe bounds tests failed with exit code $LASTEXITCODE" }

$engineExe = Join-Path $build 'audio_engine_test.exe'
Invoke-Checked ($common + @(
    '-I', (Join-Path $testRoot 'host_audio'),
    '-I', (Join-Path $testRoot 'host_codec'),
    (Join-Path $testRoot 'audio_engine_test.c'),
    (Join-Path $testRoot 'host_audio/runtime.c'),
    (Join-Path $projectRoot 'main/audio_engine.c'),
    (Join-Path $projectRoot 'main/audio_fx.c'), (Join-Path $projectRoot 'main/audio_eq.c'),
    (Join-Path $projectRoot 'main/audio_extra_codec.c'),
    (Join-Path $testRoot 'host_codec/mock_codec.c'),
    (Join-Path $projectRoot 'main/audio_file.c'), '-o', $engineExe
))
& $engineExe (Join-Path $testRoot 'fixtures/sine-440hz.mp3') (Join-Path $testRoot 'fixtures/sine-440hz-mono24.wav')
if ($LASTEXITCODE -ne 0) { throw "Audio engine tests failed with exit code $LASTEXITCODE" }
$eqEngineExe = Join-Path $build 'audio_eq_engine_test.exe'
$eqFactoryObject = Join-Path $build 'eq_factory_engine_fixture.o'
Invoke-Checked @('c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', '-I', (Join-Path $projectRoot 'main'),
    '-c', (Join-Path $testRoot 'eq_factory_engine_fixture.cpp'), '-o', $eqFactoryObject)
Invoke-Checked ($common + @(
    '-I', (Join-Path $testRoot 'host_audio'), '-I', (Join-Path $testRoot 'host_codec'),
    (Join-Path $testRoot 'audio_eq_engine_test.c'), (Join-Path $testRoot 'host_audio/runtime.c'),
    (Join-Path $projectRoot 'main/audio_engine.c'), (Join-Path $projectRoot 'main/audio_fx.c'),
    (Join-Path $projectRoot 'main/audio_eq.c'), (Join-Path $projectRoot 'main/audio_extra_codec.c'),
    (Join-Path $testRoot 'host_codec/mock_codec.c'), (Join-Path $projectRoot 'main/audio_file.c'), $eqFactoryObject, '-o', $eqEngineExe
))
& $eqEngineExe
if ($LASTEXITCODE -ne 0) { throw 'Audio EQ engine routing/meter tests failed' }
$visualStereoExe = Join-Path $build 'audio_visual_stereo_test.exe'
Invoke-Checked ($common + @(
    '-I', (Join-Path $testRoot 'host_audio'),
    '-I', (Join-Path $testRoot 'host_codec'),
    (Join-Path $testRoot 'audio_visual_stereo_test.c'),
    (Join-Path $testRoot 'host_audio/runtime.c'),
    (Join-Path $projectRoot 'main/audio_engine.c'),
    (Join-Path $projectRoot 'main/audio_fx.c'), (Join-Path $projectRoot 'main/audio_eq.c'),
    (Join-Path $projectRoot 'main/audio_extra_codec.c'),
    (Join-Path $testRoot 'host_codec/mock_codec.c'),
    (Join-Path $projectRoot 'main/audio_file.c'), '-o', $visualStereoExe
))
& $visualStereoExe
if ($LASTEXITCODE -ne 0) { throw "Audio stereo visualizer tests failed with exit code $LASTEXITCODE" }
$fxExe = Join-Path $build 'audio_fx_test.exe'
Invoke-Checked ($common + @((Join-Path $testRoot 'audio_fx_test.c'), (Join-Path $projectRoot 'main/audio_fx.c'), (Join-Path $projectRoot 'main/audio_eq.c'), '-o', $fxExe))
& $fxExe
if ($LASTEXITCODE -ne 0) { throw "Audio FX tests failed with exit code $LASTEXITCODE" }
$fxQualityExe = Join-Path $build 'audio_fx_quality_test.exe'
Invoke-Checked ($common + @((Join-Path $testRoot 'audio_fx_quality_test.c'), (Join-Path $projectRoot 'main/audio_fx.c'), (Join-Path $projectRoot 'main/audio_eq.c'), '-o', $fxQualityExe))
& $fxQualityExe
if ($LASTEXITCODE -ne 0) { throw "Audio FX quality tests failed with exit code $LASTEXITCODE" }
$reverbExe = Join-Path $build 'audio_reverb_test.exe'
# This fixture includes the production DSP to inspect the float wet tail.
Invoke-Checked ($common + @((Join-Path $testRoot 'audio_reverb_test.c'), '-o', $reverbExe))
& $reverbExe
if ($LASTEXITCODE -ne 0) { throw "Audio reverb tests failed with exit code $LASTEXITCODE" }
$extraExe = Join-Path $build 'audio_extra_codec_test.exe'
Invoke-Checked ($common + @('-I', (Join-Path $testRoot 'host_audio'), '-I', (Join-Path $testRoot 'host_codec'),
    (Join-Path $testRoot 'audio_extra_codec_test.c'), (Join-Path $testRoot 'host_codec/mock_codec.c'),
    (Join-Path $projectRoot 'main/audio_extra_codec.c'), (Join-Path $projectRoot 'main/audio_file.c'), '-o', $extraExe))
& $extraExe
if ($LASTEXITCODE -ne 0) { throw "Extra codec adapter tests failed with exit code $LASTEXITCODE" }
$pauseExe = Join-Path $build 'audio_pause_barrier_test.exe'
Invoke-Checked ($common + @(
    '-I', (Join-Path $testRoot 'host_audio'),
    '-I', (Join-Path $testRoot 'host_codec'),
    (Join-Path $testRoot 'audio_pause_barrier_test.c'),
    (Join-Path $testRoot 'host_audio/runtime.c'),
    (Join-Path $projectRoot 'main/audio_engine.c'),
    (Join-Path $projectRoot 'main/audio_fx.c'), (Join-Path $projectRoot 'main/audio_eq.c'),
    (Join-Path $projectRoot 'main/audio_extra_codec.c'),
    (Join-Path $testRoot 'host_codec/mock_codec.c'),
    (Join-Path $projectRoot 'main/audio_file.c'), '-o', $pauseExe
))
& $pauseExe (Join-Path $testRoot 'fixtures/sine-440hz.mp3')
if ($LASTEXITCODE -ne 0) { throw "Audio pause barrier tests failed with exit code $LASTEXITCODE" }
Write-Host 'All audio suites passed. Host timing does not validate Tab5 I2S/SD hardware.'
