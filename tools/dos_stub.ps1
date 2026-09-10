# Prepares the MS-DOS half of note.exe, and checks it survived the link.
#
#   tools\dos_stub.ps1 build\dos16\note16.exe build\win32\note.stub.exe
#   tools\dos_stub.ps1 -Verify build\win32\note.exe build\win32\note.stub.exe
#
# Every PE begins with an MZ header describing a real-mode program -- the DOS
# stub, normally the one that prints "This program cannot be run in DOS mode".
# link.exe takes /STUB: and puts a program of one's choosing there instead, so
# the same file can be a Win32 editor to Windows and a 16-bit editor to DOS.
# What goes in is build-dos16.bat's note16.exe; what comes out is the same
# program with a header link.exe can write into.
#
# THE 0x3C PROBLEM
#
# link.exe writes e_lfanew, the offset of the PE header, at byte 0x3C of the
# stub.  Watcom's MZ header is three paragraphs -- 48 bytes -- so byte 0x3C is
# not header at all, it is the stub's first instructions, and the linker
# overwrites four of them.  That is what LNK4060 means by "the stub file lacks
# a full MS-DOS header".  The stub is still embedded and the PE still runs, so
# nothing looks wrong; only the DOS half is quietly broken.  This is the whole
# reason the script exists, and the reason for -Verify.
#
# The fix is to rebuild the header the way a full DOS header is laid out: the
# sixteen defined fields, then reserved space through 0x3F -- which is where
# e_lfanew belongs and where nothing else reads -- then the relocation table
# at 0x40.  link.exe decides whether a header is full by looking at e_lfarlc,
# so moving the relocations is what silences LNK4060; padding alone saves the
# code but leaves the warning.  The load image only moves, so nothing in it
# needs patching; e_cparhdr, e_lfarlc and the two fields that record the file
# length in 512-byte pages do.  It costs 32 bytes.

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)][string]$Path,
    [Parameter(Position = 1, Mandatory = $true)][string]$Stub,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'

# Where e_lfanew lives, and how wide it is.  Named because both halves of this
# script care: one leaves room for it, the other has to exclude it from a
# byte-for-byte comparison, since it is the one field the linker legitimately
# writes into the stub it was given.
$MZ_LFANEW = 0x3C
$MZ_LFANEW_LEN = 4

function Expand-MzHeader($src, $dst) {
    $b = [System.IO.File]::ReadAllBytes($src)
    if ($b[0] -ne 0x4D -or $b[1] -ne 0x5A) { throw "$src is not an MZ executable." }

    $hdr  = [BitConverter]::ToUInt16($b, 0x08) * 16   # e_cparhdr, in bytes
    $nrel = [BitConverter]::ToUInt16($b, 0x06)        # e_crlc
    $rel  = [BitConverter]::ToUInt16($b, 0x18)        # e_lfarlc

    # Already a full header -- some linkers emit one -- so there is nothing to
    # move and copying it through keeps the caller's contract simple.
    if ($rel -ge 0x40) { Copy-Item $src $dst -Force; return }

    $new = 0x40 + $nrel * 4
    if ($new % 16) { $new += 16 - ($new % 16) }       # DOS loads on paragraphs
    $n = New-Object byte[] ($b.Length - $hdr + $new)
    [Array]::Copy($b, 0, $n, 0, 0x20)                       # the defined fields
    [Array]::Copy($b, $rel, $n, 0x40, $nrel * 4)            # relocations, moved
    [Array]::Copy($b, $hdr, $n, $new, $b.Length - $hdr)     # the load image
    [Array]::Copy([BitConverter]::GetBytes([uint16]($new / 16)), 0, $n, 0x08, 2)  # e_cparhdr
    [Array]::Copy([BitConverter]::GetBytes([uint16]0x40), 0, $n, 0x18, 2)         # e_lfarlc
    [Array]::Copy([BitConverter]::GetBytes([uint16]($n.Length % 512)), 0, $n, 0x02, 2)                    # e_cblp
    [Array]::Copy([BitConverter]::GetBytes([uint16][math]::Ceiling($n.Length / 512.0)), 0, $n, 0x04, 2)   # e_cp
    [System.IO.File]::WriteAllBytes($dst, $n)
}

if (-not $Verify) {
    if (-not (Test-Path $Path)) { throw "No DOS build at $Path." }
    Expand-MzHeader $Path $Stub
    "  DOS stub  {0}  ({1:N0} bytes, from {2:N0})" -f `
        $Stub, (Get-Item $Stub).Length, (Get-Item $Path).Length
    exit 0
}

# -Verify: the failure this guards against is silent.  A stub whose first
# instructions were overwritten still lets Windows load the PE, still lets
# link.exe finish, and only shows itself when someone runs the file on DOS.
# So the check is byte-for-byte against what was handed to /STUB:, and it is
# an error, not a warning: a build that quietly ships a broken DOS half is
# worse than one that stops.
$pe = [System.IO.File]::ReadAllBytes($Path)
$s  = [System.IO.File]::ReadAllBytes($Stub)
$lfanew = [BitConverter]::ToInt32($pe, $MZ_LFANEW)

if ($lfanew -lt $s.Length) {
    throw ("e_lfanew is 0x{0:X}, inside a stub of {1:N0} bytes -- the PE header overlaps the DOS program." -f $lfanew, $s.Length)
}
if ($pe[$lfanew] -ne 0x50 -or $pe[$lfanew + 1] -ne 0x45) {
    throw ("e_lfanew (0x{0:X}) does not point at a PE signature." -f $lfanew)
}

$diff = @(0..($s.Length - 1) |
          Where-Object { $_ -lt $MZ_LFANEW -or $_ -ge ($MZ_LFANEW + $MZ_LFANEW_LEN) } |
          Where-Object { $pe[$_] -ne $s[$_] })
if ($diff.Count) {
    throw ("the linker changed {0} byte(s) of the stub, at {1} -- the DOS half is corrupt." -f
           $diff.Count, (($diff | Select-Object -First 8 | ForEach-Object { '0x{0:X}' -f $_ }) -join ','))
}

"  DOS half  intact ({0:N0} bytes verbatim, PE at 0x{1:X})" -f $s.Length, $lfanew
