<#
    setup_v86.ps1 — installs the v86 emulator and a FreeDOS guest into tools\v86.

        tools\setup_v86.ps1              install if missing
        tools\setup_v86.ps1 -Force       reinstall over whatever is there

    v86 is an x86 emulator compiled to WebAssembly.  It is here because it is
    the only emulator in this project that can be driven entirely from a
    script: it runs under Node with no window, no canvas and no desktop, so a
    DOS build can be booted, typed at and photographed on a machine somebody
    else is using.  DOSBox-X can do none of that -- see tools\run_retro.ps1,
    which has to bring a window to the front and photograph the screen.

    Three things are fetched, from three places, because no one place has all
    of them:

      * the emulator itself, from the npm tarball.  npm is the only channel
        that ships a versioned, prebuilt libv86 + v86.wasm pair; the GitHub
        releases carry sources and the project's own site carries whatever is
        current today.  The .mjs build is taken because Node loads it directly.

      * the BIOS pair, from the repository.  v86 needs a SeaBIOS and a VGA
        BIOS and does not put either in the npm package.  vgabios.bin earns a
        second job here: tools\v86_bench.mjs digs the 8x16 VGA font out of it
        to draw text-mode screenshots.

      * the FreeDOS boot floppy, from copy.sh.  This is the 720K image the
        v86 demo boots, and FreeDOS is redistributable, so it can be fetched
        without anybody agreeing to a licence.  It is a floppy, not a hard
        disk, which is what makes the bench simple: the build under test goes
        in a second floppy the bench generates, and no partition table, no
        MBR and no formatting step is involved anywhere.

    Nothing here is committed -- about 8 MB of prebuilt binaries and a guest
    OS image -- see .gitignore, the same arrangement as tools\gbdk-2020 and
    tools\watcom.

    No Windows 95 image is fetched and none ever will be: Windows is not
    redistributable.  If you have a licensed image of your own, the bench
    takes it from NOTE_V86_WIN95 or from tools\v86\images\win95.img; see the
    header of tools\run_v86.ps1.
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [string]$Version = '0.5.458'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'tools\v86'

if ((Test-Path (Join-Path $dest 'build\v86.wasm')) -and -not $Force) {
    Write-Host "v86 is already in $dest (use -Force to reinstall)."
    exit 0
}

# The GitHub and npm CDNs are unhappy with PowerShell's progress rendering on
# anything large; turning it off is the difference between seconds and minutes.
function Get-File([string]$url, [string]$path) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $oldProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
    } finally {
        $ProgressPreference = $oldProgress
    }
}

if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item -ItemType Directory -Force $dest | Out-Null

Write-Host "Downloading v86 $Version from npm..."
$tgz = Join-Path $env:TEMP "v86-$Version.tgz"
Get-File "https://registry.npmjs.org/v86/-/v86-$Version.tgz" $tgz

# Windows ships bsdtar as tar.exe and it reads .tgz, so there is no second
# download for an unpacker.  Every path in the tarball is under "package/",
# hence --strip-components=1.  Only four files are kept: the debug build and
# the fallback wasm are another 13 MB and nothing here uses them.
Write-Host 'Unpacking the emulator...'
& tar.exe -xf $tgz -C $dest --strip-components=1 `
    'package/build/libv86.mjs' 'package/build/v86.wasm' 'package/LICENSE' 'package/v86.d.ts'
if ($LASTEXITCODE -ne 0) { throw "tar failed to unpack $tgz" }
Remove-Item $tgz -Force

Write-Host 'Downloading the SeaBIOS and VGA BIOS...'
Get-File 'https://raw.githubusercontent.com/copy/v86/master/bios/seabios.bin' (Join-Path $dest 'bios\seabios.bin')
Get-File 'https://raw.githubusercontent.com/copy/v86/master/bios/vgabios.bin' (Join-Path $dest 'bios\vgabios.bin')

Write-Host 'Downloading the FreeDOS boot floppy...'
Get-File 'https://copy.sh/v86/images/freedos722.img' (Join-Path $dest 'images\freedos722.img')

Set-Content -Path (Join-Path $dest 'version.txt') -Value "v86 $Version" -Encoding ASCII

foreach ($f in @('build\v86.wasm', 'build\libv86.mjs', 'bios\seabios.bin', 'bios\vgabios.bin', 'images\freedos722.img')) {
    $p = Join-Path $dest $f
    if (-not (Test-Path $p)) { throw "Install looks wrong: no $f under $dest" }
    if ((Get-Item $p).Length -lt 1024) { throw "Install looks wrong: $f is $((Get-Item $p).Length) bytes, which is a download that failed politely." }
}

# Booting the guest is the only check worth making.  A tree that unpacked
# cleanly can still be a wasm that will not instantiate under this Node, or a
# floppy image that arrived as an HTML error page with a plausible size, and
# neither shows up until something tries to run.
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { throw 'v86 unpacked, but Node was not found on PATH; the bench cannot run without it.' }

Write-Host 'Booting FreeDOS once to verify...'
$bench = Join-Path $root 'tools\v86_bench.mjs'
& $node.Source $bench --self-test
if ($LASTEXITCODE -ne 0) { throw 'v86 unpacked but will not boot FreeDOS; the install is incomplete.' }

Write-Host "v86 $Version installed in $dest and verified."
