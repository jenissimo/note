# reduce_themes.ps1 — regenerates src\platform\console\console_themes.h.
#
# The C64 and 16-bit MS-DOS builds cannot carry the theme registry, so they
# carry the core's built-in themes already reduced to their sixteen colours.
# The reduction is note_theme_reduce -- the same function every other target
# runs, compiled here for the host rather than reimplemented -- so the answer
# is what those machines would have computed if they could afford to.
#
# Run this after changing note_builtin_themes in src\core\note_theme.c, and
# commit the result: build-retro.bat and build-dos16.bat compile the generated
# header and never run this, so neither needs a host compiler.
#
#   powershell -ExecutionPolicy Bypass -File tools\reduce_themes.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root 'build\tools'
$hdr  = Join-Path $root 'src\platform\console\console_themes.h'

# The same probe list build.bat walks, in the same order: this needs a C
# compiler and does not care which one, but MSVC is the one a machine that
# builds note already has.
$pf   = [Environment]::GetEnvironmentVariable('ProgramFiles')
$pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
$vcvars = @(
    "$pf\Microsoft Visual Studio\2022\Enterprise"
    "$pf\Microsoft Visual Studio\2022\Professional"
    "$pf\Microsoft Visual Studio\2022\Community"
    "$pf\Microsoft Visual Studio\2022\BuildTools"
    "$pf86\Microsoft Visual Studio\2022\BuildTools"
    "$pf86\Microsoft Visual Studio\2019\Community"
    "$pf86\Microsoft Visual Studio\2019\BuildTools"
) | ForEach-Object { Join-Path $_ 'VC\Auxiliary\Build\vcvarsall.bat' } |
    Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $vcvars) { throw 'Visual Studio with the C++ toolset was not found.' }

New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'reduce_themes.exe'
$src = Join-Path $root 'tools\reduce_themes.c'

# /Fo names the object file rather than a directory: a directory has to end in
# a backslash, and a backslash in front of the closing quote is an escape as
# far as cmd is concerned, which eats the argument after it.
$obj = Join-Path $out 'reduce_themes.obj'
$cmd = "`"$vcvars`" x64 >nul && cl /nologo /W3 /Fo`"$obj`" /Fe`"$exe`" `"$src`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Compiling reduce_themes.c failed ($LASTEXITCODE)." }

& $exe | Set-Content -Path $hdr -Encoding ascii
if ($LASTEXITCODE -ne 0) { throw "reduce_themes.exe failed ($LASTEXITCODE)." }

$n = (Get-Content $hdr | Where-Object { $_ -match '^\s+\{ "' }).Count
Write-Host ("Wrote {0} ({1} rows, two palettes)" -f $hdr, $n)
