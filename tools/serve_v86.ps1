<#
    serve_v86.ps1 — the same guests as run_v86.ps1, in a browser you drive.

        tools\serve_v86.ps1 win95        boot Windows 95, you at the keyboard
        tools\serve_v86.ps1 dos          boot FreeDOS instead
        tools\serve_v86.ps1 win95 -Payload build\win32

    run_v86.ps1 proves things without a human: it runs v86 under Node, types
    at the guest and writes a PNG.  This is for the other case -- looking at
    the thing, clicking around in it, opening a menu to see whether it is
    wrong.  v86 is a browser emulator to begin with, so this serves a page and
    prints a URL; nothing is installed and nothing is opened for you.

    The build under test arrives the same way it does under Node: the payload
    directory becomes a FAT12 floppy.  Under Windows 95 it is A:, because the
    hard disk takes the boot; under FreeDOS it is B:, behind the boot floppy.

    Windows 95 needs an image you supply -- raw, not VHD or VMDK -- at
    tools\v86\images\win95.img or wherever NOTE_V86_WIN95 points.  It is
    loaded in pieces over HTTP range requests rather than all at once, which
    is why this serves the file itself instead of using any static server.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('dos', 'win95')]
    [string]$Target = 'win95',

    [string]$Payload,
    [int]$Port = 8086
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    throw "Node is not on PATH; the emulator runs under it."
}
if (-not (Test-Path (Join-Path $here 'v86\build\libv86.mjs'))) {
    throw "v86 is not installed -- run tools\setup_v86.ps1 first."
}

$argv = @((Join-Path $here 'serve_v86.mjs'), $Target, '-Port', $Port)
if ($Payload) { $argv += @('-Payload', (Resolve-Path $Payload).Path) }

& node @argv
