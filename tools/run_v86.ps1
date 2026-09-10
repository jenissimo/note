<#
    run_v86.ps1 — runs a build inside a real operating system under v86.

        tools\run_v86.ps1 dos                          boot FreeDOS, run the DOS build
        tools\run_v86.ps1 dos -Capture shot.png        ...and photograph it
        tools\run_v86.ps1 dos -Keys "hello{ENTER}"     ...after typing into it
        tools\run_v86.ps1 win95 -Capture shot.png      boot a Windows 95 image you supply

    This is the third of the three run scripts and the odd one out: run_gb.ps1
    and run_retro.ps1 start an emulator, bring its window to the front and
    photograph the screen, which means they cannot be run on a machine
    somebody is using.  v86 is an x86 emulator compiled to WebAssembly, and it
    runs under Node with no window at all, so this one opens nothing, steals
    no focus and writes the PNG itself.  Run it while you work.

    -Keys uses the same spelling as tools\run_retro.ps1: plain text goes
    through as typed, and names in braces stand for keys with no printable
    form -- {ESC} {ENTER} {TAB} {BKSP} {F1}..{F10} {UP} {DOWN} {LEFT} {RIGHT}
    {HOME} {END} {PGUP} {PGDN} {INS} {DEL}.

    -Capture writes a PNG of the guest's screen.  In text mode it is drawn
    from the characters and colours the VGA emulation produced, using the
    font the guest itself uploaded -- 640x400 of real pixels, not a
    photograph of a window with a title bar on it.  In graphics mode it is the
    emulator's own frame buffer.  -Text writes the same screen as 25 lines of
    plain text, which is what to diff in a test.

    -Seconds is how long the guest runs after the keys have been typed, before
    the screenshot.  -StartSeconds is the wait between launching the program
    and typing at it, and it is a real setting rather than a detail: keys sent
    while a program is still loading go into the BIOS's sixteen-byte buffer,
    and most full-screen editors flush that on startup.  Four seconds is
    enough for the DJGPP build loading CWSDPMI off a floppy.

    HOW THE BUILD GETS IN

    The payload directory is turned into a 1.44 MB FAT12 floppy image and
    handed to the guest as a second drive: B: under FreeDOS, A: behind a hard
    disk.  Nothing in the guest has to cooperate -- no drivers, no partition
    table, no formatting.  The whole directory goes on, not just the
    executable, because a DOS build is rarely one file: the DJGPP editor needs
    CWSDPMI.EXE beside it or it exits saying "no DPMI" and nothing else.

    WINDOWS 95

    No Windows image is downloaded and none will be: Windows is not
    redistributable and that is not this script's call to make.  If you have a
    licensed image of your own -- a hard disk image of an installed system,
    raw, not a VHD, VMDK or qcow2 -- point NOTE_V86_WIN95 at it, or drop it at
    tools\v86\images\win95.img, and `run_v86.ps1 win95` will boot it.  The
    plumbing either side of the image is tested and works: a supplied hard
    disk image is attached, SeaBIOS boots from it, the payload floppy shows up
    as A:, and the screenshot path handles graphics modes as well as text.
    Whether v86 will carry Windows 95 itself is the one part nobody here has
    tested, for want of an image.  v86's own project lists Windows 95 among
    the systems it runs and does not list it among the systems it runs well;
    expect a boot measured in minutes rather than seconds, expect to raise
    -BootSeconds a long way past its default, and expect the screenshot to
    come out of the graphics path rather than the text one.  If it will not
    boot at all, 86Box and PCem emulate period hardware properly and are the
    tools for that job -- at the cost of a window on somebody's desktop.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet('dos', 'win95')][string]$Target = 'dos',
    [string]$Payload,
    [string]$Run,
    [string]$File,
    [string]$Keys,
    [string]$Capture,
    [string]$Text,
    [string]$Image,
    [int]$Seconds = 4,
    [int]$StartSeconds = 4,
    [int]$BootSeconds = 0
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$v86  = Join-Path $root 'tools\v86'

if (-not (Test-Path (Join-Path $v86 'build\v86.wasm'))) {
    throw "v86 is not installed. Run tools\setup_v86.ps1 first."
}
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { throw 'Node was not found on PATH; v86 is a JavaScript library and cannot run without it.' }

# The 16-bit DOS target is the one this bench exists for, so it wins when it
# has been built; the DJGPP build is the fallback while it has not.
if (-not $Payload) {
    foreach ($candidate in @('build\dos16', 'build\dos')) {
        $p = Join-Path $root $candidate
        if ((Test-Path $p) -and (Get-ChildItem $p -Filter '*.exe' -ErrorAction SilentlyContinue)) {
            $Payload = $p
            break
        }
    }
    if (-not $Payload) { throw 'No DOS build found in build\dos16 or build\dos -- run build-retro.bat first.' }
}
if (-not (Test-Path $Payload)) { throw "No payload at $Payload" }

# DOS sees the floppy's 8.3 names, so the command is the file's own name in
# upper case.  note-dos.exe and note16.exe are both plausible; whichever .exe
# is there is the one to run.
if (-not $Run) {
    $exe = Get-ChildItem $Payload -Filter '*.exe' |
           Where-Object { $_.Name -notmatch '^CWSDPMI' } |
           Select-Object -First 1
    if (-not $exe) { throw "No .exe in $Payload to run." }
    $Run = $exe.Name.ToUpper()
}
if ($File) { $Run = "$Run $File" }

# FreeDOS reaches a prompt in under a second and the wait ends the moment it
# does, so the number is only a give-up point.  Windows 95 has no prompt to
# wait for and the wait runs to the end, so the number is the boot time.
if (-not $BootSeconds) { $BootSeconds = if ($Target -eq 'win95') { 180 } else { 60 } }

$args = @(
    (Join-Path $root 'tools\v86_bench.mjs')
    '--guest', $Target
    '--payload', $Payload
    '--run', $Run
    '--seconds', $Seconds
    '--start-seconds', $StartSeconds
    '--boot-seconds', $BootSeconds
)

if ($Target -eq 'win95') {
    # The image is the user's to supply; see the header.
    if (-not $Image) { $Image = [Environment]::GetEnvironmentVariable('NOTE_V86_WIN95') }
    if (-not $Image) { $Image = Join-Path $v86 'images\win95.img' }
    if (-not (Test-Path $Image)) {
        throw @"
No Windows 95 disk image at $Image.
Windows is not redistributable, so setup_v86.ps1 does not fetch one. Supply a
raw hard disk image of an installed Windows 95 -- not a VHD, VMDK or qcow2 --
and either set NOTE_V86_WIN95 to its path or put it at tools\v86\images\win95.img.
"@
    }
    $args += @('--image', $Image)
}

if ($Keys)    { $args += @('--keys', $Keys) }
if ($Capture) { $args += @('--capture', $Capture) }
if ($Text)    { $args += @('--text', $Text) }

& $node.Source @args
if ($LASTEXITCODE -ne 0) { throw "The guest run failed (exit $LASTEXITCODE)." }
