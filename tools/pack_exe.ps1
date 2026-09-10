# Makes a UPX-compressed copy of note.exe beside it, if UPX is installed.
#
# The copy is never the shipped binary, and that is deliberate.  Packing costs
# nothing in size terms to try and gains about a third, but a packed
# executable trips antivirus heuristics on sight, and it is decompressed into
# private dirty pages at load, so the image stops being shared between
# processes and the start is slower.  It exists for the curiosity of seeing
# note fit in 64 KB, not as the thing to hand someone.
#
# What it packs is the shipped note.exe itself, relocations and all.  It used
# to pack a second link of the same objects made with /FIXED, which drops the
# relocation table and with it ASLR: worth about a kilobyte compressed, which
# mattered while 64 KB was the number being chased.  It is not the number any
# more -- the file now carries a 16-bit MS-DOS editor in its stub and is
# measured against 128 KB -- and a program that opens documents it did not
# write should not be the one giving up ASLR to save a kilobyte.  So the
# /FIXED link is gone and this packs the real thing.
#
# The icon does still cost the packed file: it no longer carries a 256x256
# entry, and see tools\make_icon.py for why that was the entry to cut.  The
# DOS stub costs its full size too, since UPX preserves it byte for byte and
# does not compress it.
#
# THE DEFINITION PACKS, AND WHY --compress-resources=0 IS HERE
#
# This file used to claim the packs in .rsrc passed through UPX untouched.
# They do not, and the belief was load-bearing: the MS-DOS half of note.exe
# reads its definitions out of the file it is sitting in, and cannot run a UPX
# stub to get at them.  Measured, on the shipped note.exe against its packed
# copy: .rsrc falls from 20,480 raw bytes to 4,608, only the icon and the
# version block survive verbatim, and not one byte of either pack -- not even
# the first eight -- appears anywhere in the packed file.  The resource
# directory still lists them at their original RVAs, which now land inside
# UPX1; FindResource works at run time only because the stub has expanded that
# into memory first, which is exactly the trick a real-mode program cannot
# play.
#
# --compress-resources=0 keeps them, and is the flag that matters here.
# --keep-resource= is the one that looks like it should, and is silently a
# no-op for these -- it produced a file byte-identical to a plain run, blobs
# still gone.  Do not reach for it.
#
# It is not free any more.  While the packs were LZMS they were incompressible
# and UPX gained nothing by swallowing them, so keeping them cost exactly
# nothing; note's own LZSS leaves redundancy UPX can find, so the flag now
# costs roughly the difference.  That is the trade taken deliberately: an
# artifact whose DOS half silently cannot read its own definitions is the
# failure this whole format exists to remove, and the packed copy is held to
# the same standard as the shipped one rather than being the exception.
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
& $upx -q --ultra-brute --force --compress-resources=0 $min 2>&1 | Out-Null

if (-not (Test-Path $min) -or (Get-Item $min).Length -ge (Get-Item $exe).Length) {
    Remove-Item $min -Force -ErrorAction SilentlyContinue
    exit 0
}

"  packed {0}  ({1:N0} bytes, {2:P0} of {3:N0})" -f `
    $min, (Get-Item $min).Length,
    ((Get-Item $min).Length / (Get-Item $exe).Length), (Get-Item $exe).Length
