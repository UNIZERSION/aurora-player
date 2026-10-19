param([Parameter(Mandatory=$true)][string]$ZigPath, [string]$BuildDir = "$PSScriptRoot/../build-host/folder-render")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build = (Resolve-Path -LiteralPath $BuildDir).Path

# The real controller must use the tested screen-tree placement. Otherwise the
# fixture alone would silently keep passing after a return to layer_top.
$controller = [IO.File]::ReadAllText("$root/main/main.cpp")
if ($controller -notmatch 'dialog\s*=\s*lv_obj_create\(lv_screen_active\(\)\)') {
    throw 'Opaque production dialogs must use the active-screen tree for cover culling.'
}
Copy-Item "$PSScriptRoot/lv_conf_host.h" "$build/lv_conf.h"
function Q([string]$value) { '"' + $value.Replace('\', '/') + '"' }
$assets = '.section .rdata,"dr"' + "`n"
foreach ($pair in @(@('ui_shell.pack','ui_shell_pack'),@('ui_dragon_thumb.rgb565','ui_dragon_thumb_rgb565'),@('aurora_controls.pack','aurora_controls_pack'),@('aurora_dragon.pack','aurora_dragon_pack'))) {
    $path = "$root/main/$($pair[0])".Replace('\','/')
    $symbol = '_binary_' + $pair[1]
    $assets += ".balign 4`n.global ${symbol}_start`n${symbol}_start:`n.incbin " + (Q $path) + "`n.global ${symbol}_end`n${symbol}_end:`n"
}
[IO.File]::WriteAllText("$build/assets.S", $assets)
$lv = "$root/managed_components/lvgl__lvgl"
if (!(Test-Path "$lv/src")) { throw 'Restore project managed components before running the production LVGL test.' }
$argsList = @('-O2', '-DLV_CONF_INCLUDE_SIMPLE', '-I'+(Q $build), '-I'+(Q $lv), '-I'+(Q "$root/main"), '-x', 'c')
$argsList += Get-ChildItem "$lv/src" -Recurse -Filter '*.c' | ForEach-Object { Q $_.FullName }
$argsList += Get-ChildItem "$root/main" -Filter 'ui_montserrat_latin_*.c' | ForEach-Object { Q $_.FullName }
$argsList += @(Q "$root/main/aurora_fonts.c")
$argsList += @('-x','c++',(Q "$root/main/ui_fonts.cpp"),(Q "$root/main/player_ui.cpp"),(Q "$PSScriptRoot/folder_render_test.cpp"),'-x','assembler',(Q "$build/assets.S"),'-o',(Q "$build/folder-render-test.exe"))
[IO.File]::WriteAllLines("$build/build.rsp",$argsList)
& $ZigPath c++ "@$build/build.rsp"
if ($LASTEXITCODE -ne 0) { throw 'Folder renderer test compilation failed' }
& "$build/folder-render-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Folder renderer tests failed' }
