<#
    setup_gb.ps1 — installs the Game Boy toolchain into tools\gbdk-2020.

        tools\setup_gb.ps1              install if missing
        tools\setup_gb.ps1 -Force       reinstall over whatever is there

    GBDK-2020 ships as one self-contained zip: SDCC (retargeted to the sm83
    core the Game Boy's CPU actually is), the assembler and linker, the GB
    libraries, and the tools that turn a linked image into a cartridge.  It is
    fourteen megabytes of prebuilt binaries, so it is downloaded rather than
    committed -- see .gitignore.

    Do not point this at a copy of GBDK taken from another project's tree
    unless you check it first: a partial copy can be missing sdcc's own `cc1`
    backend, in which case every compile fails with "cannot execute 'cc1'"
    while lcc.exe itself looks perfectly healthy.
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [string]$Version = '4.4.0'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'tools\gbdk-2020'

if ((Test-Path (Join-Path $dest 'bin\lcc.exe')) -and -not $Force) {
    Write-Host "GBDK-2020 is already in $dest (use -Force to reinstall)."
    exit 0
}

$url = "https://github.com/gbdk-2020/gbdk-2020/releases/download/$Version/gbdk-win64.zip"
$zip = Join-Path $env:TEMP "gbdk-$Version-win64.zip"

Write-Host "Downloading GBDK-2020 $Version..."
# The GitHub CDN is unhappy with PowerShell's progress rendering on large
# files; turning it off is the difference between seconds and minutes.
$oldProgress = $ProgressPreference
$ProgressPreference = 'SilentlyContinue'
try {
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
} finally {
    $ProgressPreference = $oldProgress
}

if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }

# The archive contains a single top-level "gbdk" directory, which is unwrapped
# so the tree is tools\gbdk-2020\bin rather than tools\gbdk-2020\gbdk\bin.
$staging = Join-Path $env:TEMP "gbdk-$Version-staging"
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
Expand-Archive -Path $zip -DestinationPath $staging -Force

$inner = Join-Path $staging 'gbdk'
if (Test-Path $inner) {
    Move-Item $inner $dest
} else {
    Move-Item $staging $dest
}
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
Remove-Item $zip -Force

$lcc = Join-Path $dest 'bin\lcc.exe'
if (-not (Test-Path $lcc)) { throw "Install looks wrong: no bin\lcc.exe under $dest" }

# Compiling something is the only check worth making: a toolchain that
# unpacked cleanly can still be missing the backend lcc drives.
$probe = Join-Path $env:TEMP 'gbdk-probe'
New-Item -ItemType Directory -Force $probe | Out-Null
@'
#include <gb/gb.h>
void main(void) { while (1) vsync(); }
'@ | Set-Content (Join-Path $probe 'probe.c') -Encoding ASCII

Push-Location $probe
try {
    & $lcc -o probe.gb probe.c 2>&1 | Out-String | Write-Verbose
    if (-not (Test-Path (Join-Path $probe 'probe.gb'))) {
        throw 'GBDK unpacked but will not compile a hello-world; the install is incomplete.'
    }
} finally {
    Pop-Location
    Remove-Item $probe -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "GBDK-2020 $Version installed in $dest and verified."
