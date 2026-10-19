param([Parameter(Mandatory=$true)][string]$Zig,[Parameter(Mandatory=$true)][string]$Python,[Parameter(Mandatory=$true)][string]$LvglRoot,[string]$BuildDirectory,[string]$Sample)
$ErrorActionPreference='Stop'
$project=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(-not $BuildDirectory){$BuildDirectory=Join-Path $project 'build-host/artwork-decode'}
$build=[IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Path $build -Force | Out-Null
Copy-Item "$PSScriptRoot/lv_conf_host.h" "$build/lv_conf.h"
$conf=[IO.File]::ReadAllText("$build/lv_conf.h").Replace('#define LV_USE_TJPGD 1','#define LV_USE_TJPGD 0')
[IO.File]::WriteAllText("$build/lv_conf.h",$conf)
& $Python -X utf8 "$PSScriptRoot/generate-artwork-decoder-fixtures.py" "$build/fixtures"
if($LASTEXITCODE -ne 0){throw 'Fixture generation failed'}
function Q([string]$s){'"'+$s.Replace('\','/')+'"'}
$lv=(Resolve-Path $LvglRoot).Path
$argsList=@('-O1','-DLV_CONF_INCLUDE_SIMPLE','-I'+(Q $build),'-I'+(Q $lv),'-I'+(Q "$project/main"),'-x','c')
$argsList+=Get-ChildItem "$lv/src" -Recurse -Filter '*.c' | ForEach-Object {Q $_.FullName}
$argsList+=@(Q "$project/main/vendor/tjpgd/tjpgd.c")
$argsList+=@('-x','c++',(Q "$PSScriptRoot/artwork_decode_test.cpp"),'-o',(Q "$build/artwork-decode.exe"))
[IO.File]::WriteAllLines("$build/build.rsp",$argsList)
& $Zig c++ "@$build/build.rsp"
if($LASTEXITCODE -ne 0){throw 'Artwork decoder compilation failed'}
if($Sample){& "$build/artwork-decode.exe" "$build/fixtures" $Sample}else{& "$build/artwork-decode.exe" "$build/fixtures"}
if($LASTEXITCODE -ne 0){throw 'Artwork decoder tests failed'}
& $Python -X utf8 "$PSScriptRoot/verify-artwork-pixels.py" "$build/fixtures" $Sample
if($LASTEXITCODE -ne 0){throw 'Artwork pixel oracle failed'}
