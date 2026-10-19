param([Parameter(Mandatory=$true)][string]$Zig,[Parameter(Mandatory=$true)][string]$LvglRoot,[string]$OutputDirectory,[string]$Python,[switch]$Rgb565)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(-not $OutputDirectory){$OutputDirectory=Join-Path $root 'build/preview'}; $build=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $build -Force | Out-Null
Copy-Item "$root/tests/lv_conf_host.h" "$build/lv_conf.h"
$asm='.section .rdata,"dr"' + "`n"
foreach($pair in @(@('ui_shell.pack','ui_shell_pack'),@('ui_dragon_thumb.rgb565','ui_dragon_thumb_rgb565'),@('aurora_controls.pack','aurora_controls_pack'),@('aurora_dragon.pack','aurora_dragon_pack'))) {
 $asset=($root+'/main/'+$pair[0]).Replace('\','/')
 $asm += '# '+(Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash+"`n"
 $asm += '.balign 4' + "`n" + '.global _binary_'+$pair[1]+'_start' + "`n" + '_binary_'+$pair[1]+'_start:' + "`n" + '.incbin "'+$asset+'"' + "`n" + '.global _binary_'+$pair[1]+'_end' + "`n" + '_binary_'+$pair[1]+'_end:' + "`n"
}
[IO.File]::WriteAllText("$build/assets.S",$asm)
function Q([string]$s){'"'+$s.Replace('\','/')+'"'}
$lv=(Resolve-Path $LvglRoot).Path
$argsList=@('-O1','-DLV_CONF_INCLUDE_SIMPLE','-I'+(Q $build),'-I'+(Q $lv),'-I'+(Q "$root/main"),'-x','c')
$argsList+=Get-ChildItem "$lv/src" -Recurse -Filter '*.c' | ForEach-Object {Q $_.FullName}
$argsList+=Get-ChildItem "$root/main" -Filter 'ui_montserrat_latin_*.c' | ForEach-Object {Q $_.FullName}
$argsList+=@(Q "$root/main/aurora_fonts.c")
$argsList+=@('-x','c++',(Q "$root/main/ui_fonts.cpp"),(Q "$root/main/player_ui.cpp"),(Q "$root/tests/aurora_ui_preview.cpp"),'-x','assembler',(Q "$build/assets.S"),'-o',(Q "$build/aurora-preview.exe"))
[IO.File]::WriteAllLines("$build/build.rsp",$argsList)
& $Zig c++ "@$build/build.rsp"
if($LASTEXITCODE -ne 0){throw 'Native UI compilation failed'}
if($Rgb565){ & "$build/aurora-preview.exe" $build '--rgb565' }
else { & "$build/aurora-preview.exe" $build }
if($LASTEXITCODE -ne 0){throw 'Native UI checks failed'}
if($Python){Get-ChildItem $build -Filter '*.ppm' | ForEach-Object { & $Python -c 'from PIL import Image;import sys;Image.open(sys.argv[1]).save(sys.argv[2])' $_.FullName ([IO.Path]::ChangeExtension($_.FullName,'.png')) }

}
