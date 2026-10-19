param([Parameter(Mandatory=$true)][string]$ZigPath,[string]$BuildDir="$PSScriptRoot/../build-host/lock-ui")
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$build=(Resolve-Path -LiteralPath $BuildDir).Path
Copy-Item "$PSScriptRoot/lv_conf_host.h" "$build/lv_conf.h"
function Q([string]$value) { '"'+$value.Replace('\','/')+'"' }
$assets='.section .rdata,"dr"'+"`n"
foreach($pair in @(@('ui_shell.pack','ui_shell_pack'),@('ui_dragon_thumb.rgb565','ui_dragon_thumb_rgb565'),@('aurora_controls.pack','aurora_controls_pack'),@('aurora_dragon.pack','aurora_dragon_pack'))){
 $path="$root/main/$($pair[0])".Replace('\','/');$symbol='_binary_'+$pair[1]
 $assets+=".balign 4`n.global ${symbol}_start`n${symbol}_start:`n.incbin "+(Q $path)+"`n.global ${symbol}_end`n${symbol}_end:`n"
}
[IO.File]::WriteAllText("$build/assets.S",$assets)
$lv="$root/managed_components/lvgl__lvgl"
$argsList=@('-O1','-DLV_CONF_INCLUDE_SIMPLE','-I'+(Q $build),'-I'+(Q $lv),'-I'+(Q "$root/main"),'-x','c')
$argsList+=Get-ChildItem "$lv/src" -Recurse -Filter '*.c' | ForEach-Object {Q $_.FullName}
$argsList+=Get-ChildItem "$root/main" -Filter 'ui_montserrat_latin_*.c' | ForEach-Object {Q $_.FullName}
$argsList+=@(Q "$root/main/aurora_fonts.c")
$argsList+=@('-x','c++',(Q "$root/main/ui_fonts.cpp"),(Q "$root/main/player_ui.cpp"),(Q "$PSScriptRoot/lock_ui_test.cpp"),'-x','assembler',(Q "$build/assets.S"),'-o',(Q "$build/lock-ui-test.exe"))
[IO.File]::WriteAllLines("$build/build.rsp",$argsList)
& $ZigPath c++ "@$build/build.rsp"
if($LASTEXITCODE -ne 0){throw 'Lock UI test compilation failed'}
& "$build/lock-ui-test.exe" $build
if($LASTEXITCODE -ne 0){throw 'Lock UI tests failed'}
