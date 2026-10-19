param(
    [Parameter(Mandatory = $true)][string]$Zig,
    [Parameter(Mandatory = $true)][string]$LvglRoot,
    [string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $project 'build-host/artwork-stop' }
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
$build = (Resolve-Path $BuildDirectory).Path
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'lv_conf_host.h') -Destination (Join-Path $build 'lv_conf.h')
$exe = Join-Path $build 'artwork-stop-test.exe'
& $Zig c++ -std=c++17 -O1 -Wall -Wextra -Werror -DLV_CONF_INCLUDE_SIMPLE `
    -I $build -I $LvglRoot -I (Join-Path $project 'main') -I (Join-Path $PSScriptRoot 'host_artwork') `
    (Join-Path $project 'main/player_artwork.cpp') (Join-Path $PSScriptRoot 'artwork_stop_test.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Artwork worker host compilation failed.' }
& $exe (Join-Path $build 'artwork-stop-input.tmp')
if ($LASTEXITCODE -ne 0) { throw 'Artwork worker shutdown regression failed.' }
