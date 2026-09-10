<#
    setup_watcom.ps1 — installs the Open Watcom toolchain into tools\watcom.

        tools\setup_watcom.ps1              install if missing
        tools\setup_watcom.ps1 -Force       reinstall over whatever is there

    Open Watcom is here for one reason: it is the only maintained compiler
    that still emits 16-bit real-mode code, which is what a DOS build and a
    custom MZ stub need.  MSVC has not been able to do that since 1993.

    The v2 fork publishes two kinds of release asset.  The installers --
    open-watcom-2_0-c-win-x64.exe and friends -- are 122 MB of InstallShield
    that wants a target directory and a start menu; the snapshot tarball is
    the same build unpacked, and it can be dropped in a directory and used.
    The tarball is what this takes, so nothing is installed machine-wide and
    removing the toolchain is `rm -r tools\watcom`.

    Only part of the tarball is unpacked.  It carries every host Watcom
    builds for -- Linux, OS/2, RDOS, DOS itself, the 16-bit Windows hosts,
    plus 1500 sample files -- and none of that runs here.  What is kept is
    the Win64-hosted compilers and linker, the headers, the 16- and 32-bit
    libraries, and binw\wlink.lnk, which is where wlink reads the definition
    of `system dos` from.  That last file is easy to miss and its absence
    shows up as a link that fails with "system dos not found" while the
    compiler itself looks perfectly healthy.

    The toolchain needs WATCOM, PATH and INCLUDE set.  This script sets them
    for its own verify step only; it does not touch the machine or the user
    environment.  Anything that builds with Watcom sets them itself -- see
    tools\stubdemo\build_stubdemo.ps1 for the three lines it takes.

    -Archive points at an ow-snapshot.tar.xz already on disk and skips the
    download.  GitHub's release CDN throttles hard after a couple of pulls of
    the same 145 MB file, so reinstalling without it can take the better part
    of an hour for bytes that are already sitting in a temp directory.
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [string]$Release = 'Current-build',
    [string]$Archive
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'tools\watcom'

if ((Test-Path (Join-Path $dest 'binnt64\wcl.exe')) -and -not $Force) {
    Write-Host "Open Watcom is already in $dest (use -Force to reinstall)."
    exit 0
}

$url = "https://github.com/open-watcom/open-watcom-v2/releases/download/$Release/ow-snapshot.tar.xz"
$tar = Join-Path $env:TEMP "ow-snapshot-$Release.tar.xz"
$keep = $false

if ($Archive) {
    if (-not (Test-Path $Archive)) { throw "No such archive: $Archive" }
    $tar = (Resolve-Path $Archive).Path
    $keep = $true
    Write-Host "Using the archive already at $tar."
} else {
    Write-Host "Downloading Open Watcom ($Release, about 145 MB)..."
    # The GitHub CDN is unhappy with PowerShell's progress rendering on large
    # files; turning it off is the difference between minutes and a quarter hour.
    $oldProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $url -OutFile $tar -UseBasicParsing
    } finally {
        $ProgressPreference = $oldProgress
    }
}

if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item -ItemType Directory -Force $dest | Out-Null

# Windows ships bsdtar as tar.exe and it reads .tar.xz, so there is no second
# download for an unpacker.  Every path in the archive is prefixed with "./",
# hence --strip-components=1 and the patterns without it.
Write-Host 'Unpacking the Win64 host, the headers and the libraries...'
& tar.exe -xf $tar -C $dest --strip-components=1 `
    './binnt64' './h' './lib286' './lib386' './binw/wlink.lnk'
if ($LASTEXITCODE -ne 0) { throw "tar failed to unpack $tar" }

if (-not $keep) { Remove-Item $tar -Force }

$wcl = Join-Path $dest 'binnt64\wcl.exe'
if (-not (Test-Path $wcl)) { throw "Install looks wrong: no binnt64\wcl.exe under $dest" }

# Compiling something is the only check worth making, and for this toolchain
# the thing worth compiling is exactly what it was installed for: a 16-bit
# real-mode MZ image.  A tree that unpacked cleanly can still be missing
# wlink.lnk or the small-model libraries, and neither shows up until a link.
$probe = Join-Path $env:TEMP 'watcom-probe'
if (Test-Path $probe) { Remove-Item $probe -Recurse -Force }
New-Item -ItemType Directory -Force $probe | Out-Null
@'
#include <dos.h>
static char m[] = "probe$";
void main(void) {
    union REGS r; struct SREGS s;
    segread(&s); s.ds = FP_SEG(m);
    r.h.ah = 9; r.x.dx = FP_OFF(m); int86x(0x21, &r, &r, &s);
    r.h.ah = 0x4C; r.h.al = 0; int86(0x21, &r, &r);
}
'@ | Set-Content (Join-Path $probe 'probe.c') -Encoding ASCII

$env:WATCOM = $dest
$env:INCLUDE = Join-Path $dest 'h'
$env:PATH = (Join-Path $dest 'binnt64') + ';' + $env:PATH

Push-Location $probe
try {
    # The -f options are quoted because PowerShell splits an unquoted native
    # argument on '=' and wcl then sees a bare '.exe' it cannot open.
    & $wcl -q -0 -bcl=dos -ms '-fe=probe.exe' probe.c 2>&1 | Out-String | Write-Verbose
    $exe = Join-Path $probe 'probe.exe'
    if (-not (Test-Path $exe)) {
        throw 'Watcom unpacked but will not build a real-mode hello-world; the install is incomplete.'
    }
    $sig = [System.IO.File]::ReadAllBytes($exe)[0..1]
    if ($sig[0] -ne 0x4D -or $sig[1] -ne 0x5A) {
        throw 'Watcom linked something, but it is not an MZ image; check -bcl=dos and binw\wlink.lnk.'
    }
    $size = (Get-Item $exe).Length
} finally {
    Pop-Location
    Remove-Item $probe -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Open Watcom installed in $dest and verified (MZ probe: $size bytes)."
