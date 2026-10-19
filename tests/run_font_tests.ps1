param(
    [Parameter(Mandatory = $true)][string]$Zig,
    [Parameter(Mandatory = $true)][string]$LvglRoot,
    [string]$BuildDir
)
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$lvglSource = (Resolve-Path $LvglRoot).Path
$zigExe = (Resolve-Path $Zig).Path
if (-not $BuildDir) { $BuildDir = Join-Path $project 'build/font-tests' }
$buildDirectory = [IO.Path]::GetFullPath($BuildDir)
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'lv_conf_host.h') -Destination (Join-Path $buildDirectory 'lv_conf.h')
function Quote-Argument([string]$value) { return '"' + $value.Replace('\', '/') + '"' }
$compileArgs = @(
    '-O1', '-DLV_CONF_INCLUDE_SIMPLE',
    '-I' + (Quote-Argument $buildDirectory),
    '-I' + (Quote-Argument $lvglSource),
    '-I' + (Quote-Argument (Join-Path $project 'main')),
    '-x', 'c'
)
$compileArgs += Get-ChildItem -LiteralPath (Join-Path $lvglSource 'src') -Recurse -Filter '*.c' | ForEach-Object { Quote-Argument $_.FullName }
foreach ($size in @(14,16,18,20,24,32)) {
    $compileArgs += Quote-Argument (Join-Path $project "main/ui_montserrat_latin_$size.c")
}
$compileArgs += @('-x', 'c++',
    (Quote-Argument (Join-Path $project 'main/ui_fonts.cpp')),
    (Quote-Argument (Join-Path $PSScriptRoot 'font_latin_test.cpp')),
    '-o', (Quote-Argument (Join-Path $buildDirectory 'font-tests.exe')))
$responsePath = Join-Path $buildDirectory 'build.rsp'
[IO.File]::WriteAllLines($responsePath, $compileArgs)
& $zigExe c++ "@$responsePath"
if ($LASTEXITCODE -ne 0) { throw 'Font test compilation failed.' }
& (Join-Path $buildDirectory 'font-tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Font glyph regression failed.' }
