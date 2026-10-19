param(
    [Parameter(Mandatory = $true)] [ValidatePattern('^COM[0-9]+$')] [string]$Port,
    [string]$Python = 'python',
    [ValidateRange(115200, 921600)] [int]$Baud = 460800
)
$ErrorActionPreference = 'Stop'
$firmwarePath = [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) 'firmware'))
$manifest = Get-Content -LiteralPath (Join-Path $firmwarePath 'build-info.json') -Raw | ConvertFrom-Json
if ($manifest.chip -ne 'esp32p4') { throw 'This package is not for ESP32-P4.' }
$flashArguments = @('--chip', 'esp32p4', '--port', $Port, '--baud', $Baud,
    '--before', 'default_reset', '--after', 'watchdog_reset', '--no-stub', 'write_flash',
    '--flash_mode', [string]$manifest.flash_settings.flash_mode,
    '--flash_size', [string]$manifest.flash_settings.flash_size,
    '--flash_freq', [string]$manifest.flash_settings.flash_freq)
foreach ($entry in $manifest.files) {
    $binaryPath = [IO.Path]::GetFullPath((Join-Path $firmwarePath $entry.file))
    if (-not $binaryPath.StartsWith($firmwarePath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Invalid binary path in firmware manifest.'
    }
    if ((Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash -ne $entry.sha256) {
        throw "Firmware checksum mismatch: $($entry.file)"
    }
    $flashArguments += @([string]$entry.offset, $binaryPath)
}
& $Python -m esptool version
if ($LASTEXITCODE -ne 0) { throw 'Install esptool first: python -m pip install esptool==4.12.0' }
Write-Host "Writing verified M5Stack Tab5 firmware on $Port. Keep USB connected."
# Split-image flashing preserves gaps between partitions. No erase_flash/--force.
# esptool validates chip ID and image revision requirements before writing.
& $Python -m esptool @flashArguments
if ($LASTEXITCODE -ne 0) { throw 'Flashing failed. Read the esptool error above; no force override was used.' }
Write-Host 'Firmware written and verified. The Tab5 is restarting.'
