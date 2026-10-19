param([Parameter(Mandatory=$true)][string]$ZigPath,[string]$BuildDir="$PSScriptRoot/../build-host")
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$exe=Join-Path (Resolve-Path -LiteralPath $BuildDir).Path 'accessories_test.exe'
& $ZigPath cc -std=c11 -Wall -Wextra -Werror -I $PSScriptRoot "$PSScriptRoot/accessories_test.c" -o $exe
if($LASTEXITCODE -ne 0){throw 'Accessories test compilation failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Accessories tests failed'}
