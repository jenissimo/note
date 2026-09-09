# Makes a UPX-compressed copy of note.exe beside it, if UPX is installed.
#
# The copy is never the shipped binary, and that is deliberate.  Packing costs
# nothing in size terms to try and gains about a third, but a packed
# executable trips antivirus heuristics on sight, and it is decompressed into
# private dirty pages at load, so the image stops being shared between
# processes and the start is slower.  It exists for the curiosity of seeing
# note fit in 64 KB, not as the thing to hand someone.
#
# One hard constraint learned the hard way: the image must NOT be linked with
# /FILEALIGN:16.  UPX rewrites such a file into something Windows refuses to
# load, with no error from UPX itself.
#
#   powershell -ExecutionPolicy Bypass -File tools\pack_exe.ps1 <build dir>

param([string]$OutDir)

$ErrorActionPreference = "SilentlyContinue"

$exe = Join-Path $OutDir "note.exe"
if (-not (Test-Path $exe)) { exit 0 }

$upx = (Get-Command upx -ErrorAction SilentlyContinue).Source
if (-not $upx) {
    # winget installs it under a versioned directory and only puts it on the
    # PATH of shells started afterwards.
    $upx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" `
                -Recurse -Filter upx.exe -ErrorAction SilentlyContinue |
            Select-Object -First 1).FullName
}
if (-not $upx) { exit 0 }

$min = Join-Path $OutDir "note.min.exe"
Copy-Item $exe $min -Force
& $upx -q --ultra-brute --force $min 2>&1 | Out-Null

if (-not (Test-Path $min) -or (Get-Item $min).Length -ge (Get-Item $exe).Length) {
    Remove-Item $min -Force -ErrorAction SilentlyContinue
    exit 0
}

"  packed {0}  ({1:N0} bytes, {2:P0} of {3:N0})" -f `
    $min, (Get-Item $min).Length,
    ((Get-Item $min).Length / (Get-Item $exe).Length), (Get-Item $exe).Length
