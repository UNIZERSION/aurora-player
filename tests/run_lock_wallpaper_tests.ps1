param([Parameter(Mandatory=$true)][string]$Zig,[Parameter(Mandatory=$true)][string]$Python,[Parameter(Mandatory=$true)][string]$LvglRoot,[Parameter(Mandatory=$true)][string]$BuildDirectory)
$ErrorActionPreference='Stop'
$project=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build=[IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Path $build -Force | Out-Null
$conf=[IO.File]::ReadAllText("$PSScriptRoot/lv_conf_host.h").Replace('#define LV_USE_TJPGD 1','#define LV_USE_TJPGD 0')
[IO.File]::WriteAllText("$build/lv_conf.h",$conf)
& $Python -X utf8 "$PSScriptRoot/generate-lock-wallpaper-fixtures.py" "$build/fixtures"
if($LASTEXITCODE -ne 0){throw 'Wallpaper fixture generation failed'}
function Q([string]$s){'"'+$s.Replace('\','/')+'"'}
$lv=(Resolve-Path $LvglRoot).Path
$argsList=@('-O1','-DLV_CONF_INCLUDE_SIMPLE','-I'+(Q $build),'-I'+(Q $lv),'-I'+(Q "$project/main"),'-x','c')
$argsList+=Get-ChildItem "$lv/src" -Recurse -Filter '*.c' | ForEach-Object {Q $_.FullName}
$argsList+=@(Q "$project/main/vendor/tjpgd/tjpgd.c")
$argsList+=@('-x','c++',(Q "$PSScriptRoot/lock_wallpaper_test.cpp"),'-o',(Q "$build/lock-wallpaper.exe"))
[IO.File]::WriteAllLines("$build/build.rsp",$argsList)
& $Zig c++ "@$build/build.rsp"
if($LASTEXITCODE -ne 0){throw 'Wallpaper test compilation failed'}
& "$build/lock-wallpaper.exe" "$build/fixtures"
if($LASTEXITCODE -ne 0){throw 'Wallpaper tests failed'}
