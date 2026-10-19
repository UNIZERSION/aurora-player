# Runs every Aurora host (PC) test with Zig 0.13.0 and writes a summary.
#   tests\run_all_tests.ps1 [-Zig zig.exe] [-Python python.exe] [-Only tap_gesture,core]
# Needs managed_components (run `idf.py reconfigure` or a build once) and, for
# the exfat suite, a configured firmware build in build\. Python needs Pillow.
param(
    [string]$Project = "$PSScriptRoot\..",
    [string]$OutDir = "$PSScriptRoot\..\build-host-all",
    [string]$Zig = "",
    [string]$Python = "",
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Continue'
$zig = if ($Zig) { (Resolve-Path $Zig).Path } else { (Get-Command zig -ErrorAction Stop).Source }
$python = if ($Python) { $Python } else { (Get-Command python -ErrorAction Stop).Source }
$project = (Resolve-Path $Project).Path
$tests = Join-Path $project 'tests'
$lvgl = Join-Path $project 'managed_components\lvgl__lvgl'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = (Resolve-Path $OutDir).Path
$logs = Join-Path $out 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null

function B([string]$name) { $d = Join-Path $out $name; New-Item -ItemType Directory -Force $d | Out-Null; $d }

# name -> argument list for a child powershell / python process
$suite = [ordered]@{}
foreach ($n in 'accessories','assets','audio_eq','audio','bluetooth','core','display','eq_store','folder_render','keyboard_hal','keyboard_ui',
               'library_scan_memory','lock_ui','motion_hal','particles_response','power','queue_cache','reverb_settings',
               'screen_lock','storage_layout','tap_gesture','fixed_rate_stream','transport_policy','visual_scroll') {
    $script = if ($n -eq 'screen_lock') { 'run_screen_lock_tests.ps1' } else { "run_${n}_tests.ps1" }
    $suite[$n] = @('ps', $script, '-ZigPath', $zig, '-BuildDir', (B $n))
}
$suite['exfat'] = @('ps', 'run_exfat_test.ps1', '-ZigPath', $zig, '-FirmwareBuildDir', (Join-Path $project 'build'), '-BuildDir', (B 'exfat'))
$suite['artwork_stop'] = @('ps', 'run_artwork_stop_test.ps1', '-Zig', $zig, '-LvglRoot', $lvgl, '-BuildDirectory', (B 'artwork_stop'))
$suite['font'] = @('ps', 'run_font_tests.ps1', '-Zig', $zig, '-LvglRoot', $lvgl, '-BuildDir', (B 'font'))
$suite['artwork_decode'] = @('ps', 'run_artwork_decode_tests.ps1', '-Zig', $zig, '-Python', $python, '-LvglRoot', $lvgl, '-BuildDirectory', (B 'artwork_decode'))
$suite['lock_wallpaper'] = @('ps', 'run_lock_wallpaper_tests.ps1', '-Zig', $zig, '-Python', $python, '-LvglRoot', $lvgl, '-BuildDirectory', (B 'lock_wallpaper'))
foreach ($n in 'artwork_controller','audio_eq_boot_probe','charging_status','eq_controller','folder_navigation',
               'keyboard_controller','shutdown_controller','tap_controller') {
    $suite["py_$n"] = @('py', "${n}_test.py", '--zig', $zig, '--build', (B "py_$n"))
}
$suite['py_eq_dialog_lvgl'] = @('py', 'run_eq_dialog_lvgl_test.py', '--zig', $zig, '--build', (B 'py_eq_dialog_lvgl'))
$suite['py_fx_skin_lvgl'] = @('py', 'run_eq_dialog_lvgl_test.py', '--zig', $zig, '--build', (B 'py_fx_skin_lvgl'), '--fixture', 'fx_skin_lvgl_test.cpp')
$suite['py_search_keyboard_lvgl'] = @('py', 'run_search_keyboard_lvgl_test.py', '--zig', $zig, '--build', (B 'py_search_keyboard_lvgl'))
$suite['py_eq_factory_response'] = @('py', 'eq_factory_response_test.py', '--output', (Join-Path (B 'py_eq_factory_response') 'out.json'))

$results = @()
Push-Location $project
foreach ($name in $suite.Keys) {
    if ($Only.Count -and $name -notin $Only) { continue }
    $spec = $suite[$name]
    $log = Join-Path $logs "$name.log"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if ($spec[0] -eq 'ps') {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests $spec[1]) @($spec[2..($spec.Count-1)]) *> $log
    } else {
        & $python -X utf8 (Join-Path $tests $spec[1]) @($spec[2..($spec.Count-1)]) *> $log
    }
    $code = $LASTEXITCODE
    $sw.Stop()
    $status = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
    $results += [pscustomobject]@{ Test = $name; Status = $status; Exit = $code; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1) }
    "{0,-26} {1} ({2}s)" -f $name, $status, [math]::Round($sw.Elapsed.TotalSeconds, 1)
}
Pop-Location
$results | Export-Csv (Join-Path $out 'summary.csv') -NoTypeInformation -Encoding UTF8
"PASS: {0}  FAIL: {1}" -f @($results | Where-Object Status -eq 'PASS').Count, @($results | Where-Object Status -eq 'FAIL').Count
