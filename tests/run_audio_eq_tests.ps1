param([Parameter(Mandatory=$true)][string]$ZigPath,[string]$BuildDir=(Join-Path $PSScriptRoot '../../../work/eq1311'))
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$exe=Join-Path ([IO.Path]::GetFullPath($BuildDir)) 'audio_eq_test.exe'
& $ZigPath cc -std=c11 -O2 -Wall -Wextra -Werror (Join-Path $PSScriptRoot 'audio_eq_test.c') -o $exe
if($LASTEXITCODE -ne 0){throw 'EQ compilation failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'EQ tests failed'}
