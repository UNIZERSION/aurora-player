param(
    [ValidateSet('build', 'flash', 'monitor', 'menuconfig')]
    [string]$Action = 'build',
    [string]$Port = ''
)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
if (-not $env:IDF_PATH -or -not (Test-Path -LiteralPath "$env:IDF_PATH/tools/idf.py")) {
    throw 'Open an ESP-IDF 5.5.2 PowerShell terminal first (or run ESP-IDF export.ps1).'
}
if ($Action -eq 'flash' -and -not $Port) {
    throw 'For flashing explicitly specify the Tab5 USB port, e.g. -Action flash -Port COM7.'
}
$idfArguments = @('-C', $projectPath)
if ($Port) { $idfArguments += @('-p', $Port) }
if ($Action -eq 'flash') { $idfArguments += @('build', 'flash') }
else { $idfArguments += $Action }
& python "$env:IDF_PATH/tools/idf.py" @idfArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
