param(
  [Parameter(Mandatory=$true)][string]$ZigPath,
  [string]$AuroraProject = (Join-Path $PSScriptRoot '..\..')
)
$ErrorActionPreference='Stop'
$picoProject=Split-Path $PSScriptRoot -Parent
$zig=(Resolve-Path -LiteralPath $ZigPath).Path
$aurora=(Resolve-Path -LiteralPath $AuroraProject).Path
foreach($variant in @('legacy','native-enum')) {
  $defines=@()
  if($variant -eq 'native-enum'){$defines=@('-DMOCK_NATIVE_AUDIO_ENUM=1')}
  foreach($test in @('runtime','audio_wire')) {
    $exe=Join-Path $PSScriptRoot "$test-$variant.exe"
    & $zig c++ -std=c++17 -O2 -Wall -Wextra -Werror @defines -I (Join-Path $PSScriptRoot 'mocks') -I (Join-Path $picoProject 'src') -I (Join-Path $aurora 'main') -I (Join-Path $aurora 'tests/host_audio') (Join-Path $PSScriptRoot "${test}_test.cpp") -o $exe
    if($LASTEXITCODE -ne 0){throw "Pico $test $variant test build failed"}
    & $exe
    if($LASTEXITCODE -ne 0){throw "Pico $test $variant tests failed"}
    if($test -eq 'runtime') {
      & $exe race
      if($LASTEXITCODE -ne 0){throw "Pico $variant in-flight session regression failed"}
    }
  }
}
Write-Host 'No serial port opened. Hardware RF/DMA timing is not simulated.'

$mediaExe=Join-Path $PSScriptRoot 'media_state.exe'
& $zig c++ -std=c++17 -O2 -Wall -Wextra -Werror (Join-Path $PSScriptRoot 'media_state_test.cpp') -o $mediaExe
if($LASTEXITCODE -ne 0){throw 'Pico media state test build failed'}
& $mediaExe
if($LASTEXITCODE -ne 0){throw 'Pico media state tests failed'}
