param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$testOutput = Join-Path (Resolve-Path -LiteralPath $BuildDir).Path 'power_off_test.exe'
& $ZigPath cc -std=c11 -Wall -Wextra -Werror -I $PSScriptRoot "$PSScriptRoot/power_off_test.c" -o $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Power-off test compilation failed' }
& $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Power-off tests failed' }
