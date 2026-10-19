param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$testOutput = Join-Path (Resolve-Path -LiteralPath $BuildDir).Path 'display_present_test.exe'
& $ZigPath cc -std=c11 -Wall -Wextra -Werror -I $PSScriptRoot "$PSScriptRoot/display_present_test.c" -o $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Display presentation test compilation failed' }
& $testOutput
if ($LASTEXITCODE -ne 0) { throw 'Display presentation tests failed' }
foreach ($failure in @('ppa', 'publish', 'queue', 'wake', 'first-frame-timeout', 'first-frame-ppa', 'first-frame-backlight', 'startup-expander', 'startup-feature', 'startup-expander-all', 'startup-feature-all', 'startup-i2c-all', 'startup-reset')) {
    & $testOutput $failure
    if ($LASTEXITCODE -ne 0) { throw "Display $failure failure tests failed" }
}
