# Guided recording helper for the FIFO calibration build (AURORA_TAP_STREAM):
# decode the live serial log, print the activity since the previous
# checkpoint and record a new checkpoint at the latest sample.
#   .\checkpoint-fifo.ps1 -Name stand-triples -Log tap-fifo.log -Out tap-fifo [-Segments tap_segments.exe] [-Python python.exe]
param([Parameter(Mandatory = $true)][string]$Name,
      [Parameter(Mandatory = $true)][string]$Log,
      [Parameter(Mandatory = $true)][string]$Out,
      [string]$Segments = '',
      [string]$Python = 'python')
$py = $Python
& $py -I "$PSScriptRoot\decode_fifo.py" $Log $Out | Select-String '^all sessions|^rate windows|dropped'
$state = Join-Path $Out 'checkpoints.csv'
if (-not (Test-Path $state)) { 'name,from_ms,to_ms' | Set-Content $state }
$rows = @(Import-Csv $state)
$from = if ($rows.Count) { [uint32]$rows[-1].to_ms } else { 0 }
$last = Get-Content (Join-Path $Out 'all.csv') -Tail 1
$to = [uint32]($last.Split(',')[0])
"$Name,$from,$to" | Add-Content $state
"window $Name : device ms $from .. $to"
if ($Segments) { & $Segments (Join-Path $Out 'all.csv') 1500 $from $to }
else { & $py -I "$PSScriptRoot\onsets.py" (Join-Path $Out 'all.csv') $from $to 0.06 }
