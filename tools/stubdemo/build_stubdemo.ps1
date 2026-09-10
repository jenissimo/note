# THROWAWAY PROOF -- not part of note.  Delete tools\stubdemo\ at will.
#
# Builds three things and then checks them:
#
#   stub.exe     16-bit real-mode MZ, Watcom, small model
#   ted16.exe    a crude 16-bit editor, built only to be measured
#   stubtest.exe a Win32 PE linked with note's own link flags plus
#                /STUB:stub.exe, so we can see what the stub does there
#
#   powershell -ExecutionPolicy Bypass -File tools\stubdemo\build_stubdemo.ps1
#
# Output lands in build\stubdemo\, which .gitignore already covers.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$here = $PSScriptRoot
$out  = Join-Path $root 'build\stubdemo'
New-Item -ItemType Directory -Force $out | Out-Null

# --- Watcom -----------------------------------------------------------------
# setup_watcom.ps1 installs but deliberately sets nothing, so every consumer
# says what it needs.  For a DOS build that is these three.
$watcom = Join-Path $root 'tools\watcom'
if (-not (Test-Path (Join-Path $watcom 'binnt64\wcl.exe'))) {
    throw "Open Watcom is not installed; run tools\setup_watcom.ps1 first."
}
$env:WATCOM  = $watcom
$env:INCLUDE = Join-Path $watcom 'h'
$env:PATH    = (Join-Path $watcom 'binnt64') + ';' + $env:PATH
$wcl = Join-Path $watcom 'binnt64\wcl.exe'

# -0 is 8086 code, -bcl=dos builds and links a DOS executable in one step,
# -ms is the small model (one 64 KB code segment, one 64 KB data segment),
# -fm= asks for the map file the size questions are answered from.
Push-Location $out
try {
    # The -f options are quoted because PowerShell splits an unquoted native
    # argument on '=' and wcl then sees a bare '.exe' it cannot open.
    & $wcl -q -0 -bcl=dos -ms -os '-fe=stub.exe'  '-fm=stub.map'  (Join-Path $here 'stub.c')
    if ($LASTEXITCODE -ne 0) { throw 'stub.c did not build' }
    & $wcl -q -0 -bcl=dos -ms -os '-fe=ted16.exe' '-fm=ted16.map' (Join-Path $here 'ted16.c')
    if ($LASTEXITCODE -ne 0) { throw 'ted16.c did not build' }
} finally {
    Pop-Location
}

# --- the 0x3C problem -------------------------------------------------------
# link.exe writes e_lfanew, the offset of the PE header, at byte 0x3C of the
# stub it embeds.  Watcom's MZ header is three paragraphs -- 48 bytes -- so
# byte 0x3C is not header at all, it is the stub's first instructions, and
# the linker overwrites four of them.  That is what LNK4060 is complaining
# about: "the stub file lacks a full MS-DOS header".  The stub still gets
# embedded and the PE still runs, but the DOS half is quietly broken.
#
# The fix is to rebuild the header the way a "full" DOS header is laid out:
# the sixteen defined fields, then reserved space through 0x3F -- which is
# where e_lfanew belongs and where nothing else reads -- then the relocation
# table at 0x40.  link.exe decides whether a header is full by looking at
# e_lfarlc, so moving the relocations is what silences LNK4060; padding alone
# saves the code but leaves the warning.  The image itself only moves, so
# nothing in it needs patching; e_cparhdr, e_lfarlc and the two fields that
# record the file length in 512-byte pages do.
function Expand-MzHeader($src, $dst) {
    $b = [System.IO.File]::ReadAllBytes($src)
    $hdr = [BitConverter]::ToUInt16($b, 0x08) * 16
    $nrel = [BitConverter]::ToUInt16($b, 0x06)
    $rel = [BitConverter]::ToUInt16($b, 0x18)
    if ($rel -ge 0x40) { Copy-Item $src $dst -Force; return }

    $new = 0x40 + $nrel * 4
    if ($new % 16) { $new += 16 - ($new % 16) }
    $n = New-Object byte[] ($b.Length - $hdr + $new)
    [Array]::Copy($b, 0, $n, 0, 0x20)                       # the defined fields
    [Array]::Copy($b, $rel, $n, 0x40, $nrel * 4)            # relocations, moved
    [Array]::Copy($b, $hdr, $n, $new, $b.Length - $hdr)     # the load image
    [Array]::Copy([BitConverter]::GetBytes([uint16]($new / 16)), 0, $n, 0x08, 2)  # e_cparhdr
    [Array]::Copy([BitConverter]::GetBytes([uint16]0x40), 0, $n, 0x18, 2)         # e_lfarlc
    [Array]::Copy([BitConverter]::GetBytes([uint16]($n.Length % 512)), 0, $n, 0x02, 2)          # e_cblp
    [Array]::Copy([BitConverter]::GetBytes([uint16][math]::Ceiling($n.Length / 512.0)), 0, $n, 0x04, 2)  # e_cp
    [System.IO.File]::WriteAllBytes($dst, $n)
}
Expand-MzHeader (Join-Path $out 'stub.exe') (Join-Path $out 'stub64.exe')

# --- MSVC, found the way build.bat finds it ---------------------------------
$vcvars = $null
foreach ($vs in @(
    "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise",
    "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional",
    "$env:ProgramFiles\Microsoft Visual Studio\2022\Community",
    "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools")) {
    $p = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    if (Test-Path $p) { $vcvars = $p; break }
}
if (-not $vcvars) { throw 'Visual Studio with the C++ toolset was not found.' }

# cl and link need vcvarsall's environment, and vcvarsall is a batch file, so
# this goes through cmd rather than being run directly.  The link flags are
# build.bat's, verbatim, with /STUB: added -- the point of the exercise is
# that they are not adjusted for it.
$bat = Join-Path $out '_build.bat'
@"
@echo off
call "$vcvars" x86 >nul || exit /b 1
cd /d "$out"
cl /nologo /c /O1 /Os /GS- /Gs1000000 /W4 "$here\win.c" /Fo"$out\win.obj" || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /ENTRY:noteEntry /NODEFAULTLIB ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO /STUB:"$out\stub.exe" ^
     /OUT:"$out\stubtest.exe" "$out\win.obj" kernel32.lib || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /ENTRY:noteEntry /NODEFAULTLIB ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO /STUB:"$out\stub64.exe" ^
     /OUT:"$out\stubtest64.exe" "$out\win.obj" kernel32.lib || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /ENTRY:noteEntry /NODEFAULTLIB ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
     /OUT:"$out\nostub.exe" "$out\win.obj" kernel32.lib || exit /b 1
"@ | Set-Content $bat -Encoding ASCII
& cmd.exe /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw 'MSVC build failed' }
Remove-Item $bat -Force

# --- what came out ----------------------------------------------------------
'' ; 'sizes'
foreach ($f in 'stub.exe','stub64.exe','ted16.exe','nostub.exe','stubtest.exe','stubtest64.exe') {
    '  {0,-16} {1,8:N0} bytes' -f $f, (Get-Item (Join-Path $out $f)).Length
}

# Is the marker string from stub.c actually inside the PE, is the linker's own
# "cannot be run in DOS mode" text gone, and did the stub's code survive?
function Check($pe, $stub) {
    $b = [System.IO.File]::ReadAllBytes((Join-Path $out $pe))
    $s = [System.IO.File]::ReadAllBytes((Join-Path $out $stub))
    $lfanew = [BitConverter]::ToInt32($b, 0x3C)
    $ascii = [System.Text.Encoding]::ASCII.GetString($b, 0, $lfanew)
    $diff = @(0..($s.Length - 1) | Where-Object { $b[$_] -ne $s[$_] })
    ''
    "$pe (stub $stub)"
    '  e_lfanew        0x{0:X} -- {0} bytes of stub, {1} of padding' -f $lfanew, ($lfanew - $s.Length)
    '  marker present  {0}' -f ($ascii -match 'NOTEPADTURBO-WATCOM-STUB')
    '  default text    {0}' -f ($ascii -match 'cannot be run in DOS mode')
    '  bytes changed   {0} of {1}{2}' -f $diff.Count, $s.Length,
        $(if ($diff.Count) { ' at ' + (($diff | ForEach-Object { '0x{0:X}' -f $_ }) -join ',') } else { '' })
}
Check 'stubtest.exe'   'stub.exe'
Check 'stubtest64.exe' 'stub64.exe'

# Both are ExitProcess(0) with no window, so running them costs nothing and
# tells us Windows still loads an image with a foreign stub in front of it.
''
foreach ($f in 'stubtest.exe','stubtest64.exe') {
    & (Join-Path $out $f)
    '{0} exit code {1}' -f $f, $LASTEXITCODE
}

# --- what 16-bit code costs -------------------------------------------------
# The segment table is the honest answer: _TEXT is code, CONST/CONST2/_DATA
# are initialised data, and the difference between the two programs is what
# an editor's worth of C plus the stdio and malloc it drags in actually
# weighs in a small-model DOS build.
foreach ($m in 'stub.map','ted16.map') {
    ''
    "$m"
    Get-Content (Join-Path $out $m) |
        Where-Object { $_ -match '^(BEGTEXT|_TEXT|CONST|CONST2|_DATA|Memory size|Stack size)' } |
        ForEach-Object { '  ' + $_.Trim() }
}

# --- does UPX keep the stub? ------------------------------------------------
# pack_exe.ps1 finds UPX on PATH or under the winget package directory; the
# same two probes, because the answer is only interesting for the UPX this
# project would actually use.
$upx = (Get-Command upx -ErrorAction SilentlyContinue).Source
if (-not $upx) {
    $upx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" `
                -Recurse -Filter upx.exe -ErrorAction SilentlyContinue |
            Select-Object -First 1).FullName
}
''
if (-not $upx) {
    'UPX not installed; skipping the packing question.'
} else {
    '{0}' -f (& $upx --version | Select-Object -First 1)
    Copy-Item (Join-Path $out 'stubtest64.exe') (Join-Path $out 'packed.exe') -Force
    & $upx -q --ultra-brute --force (Join-Path $out 'packed.exe') 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "UPX refused the file (exit $LASTEXITCODE)" }
    $b = [System.IO.File]::ReadAllBytes((Join-Path $out 'packed.exe'))
    $lfanew = [BitConverter]::ToInt32($b, 0x3C)
    $ascii = [System.Text.Encoding]::ASCII.GetString($b, 0, $lfanew)
    'packed.exe       {0:N0} bytes' -f $b.Length
    '  e_lfanew        0x{0:X}' -f $lfanew
    '  marker present  {0}' -f ($ascii -match 'NOTEPADTURBO-WATCOM-STUB')
    '  default text    {0}' -f ($ascii -match 'cannot be run in DOS mode')
    '  stub region     ' + (($b[0x40..0x7F] | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
    & (Join-Path $out 'packed.exe')
    '  exit code       {0}' -f $LASTEXITCODE
}
