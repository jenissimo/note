<#
    qemu95.ps1 -- a Windows 95 guest under QEMU that a script can drive.

        tools\qemu95.ps1 setup                     find QEMU, prepare the disk
        tools\qemu95.ps1 run                       boot it, headless
        tools\qemu95.ps1 shot desk.png             photograph the guest
        tools\qemu95.ps1 push                      put build\win32\note.exe in
        tools\qemu95.ps1 type "A:\NOTE.EXE"        type at the guest
        tools\qemu95.ps1 keys ctrl-esc r           press key combinations
        tools\qemu95.ps1 click 512 384             click where the guest sees it
        tools\qemu95.ps1 move 512 384              put the pointer there
        tools\qemu95.ps1 save desktop              snapshot the running state
        tools\qemu95.ps1 load desktop              go back to it
        tools\qemu95.ps1 stop                      shut the guest down

    WHY THIS EXISTS ALONGSIDE run_v86.ps1

    tools\run_v86.ps1 boots DOS under v86 and is the right tool for the DOS
    build: it runs inside Node, opens nothing, and is fast.  It cannot do the
    two things a Win32 build needs.  The first is a screenshot of a graphical
    Windows 95 that arrives without anybody watching; the second is a
    debugger.  QEMU has both -- a QMP socket that answers screendump, and a
    gdbstub -- so this script is the Win32 half of the same idea.

    The two share the disk image.  Windows is not redistributable and nothing
    here downloads it; the raw image tools\run_v86.ps1 documents is the one
    `setup` converts, and NOTE_V86_WIN95 is honoured the same way.

    EVERYTHING GOES THROUGH QMP

    QEMU has two control sockets and only one of them is scriptable.  The
    human monitor echoes each character back with cursor-movement escapes as
    it is typed, so a reply is the command interleaved with its own echo, and
    a long Windows path comes back unrecognisable -- screendump to an absolute
    path fails there for no reason a script can diagnose.  QMP is line-based
    JSON with no echo, so that is the only socket this script opens.  The few
    commands QMP never got -- savevm, loadvm -- go through QMP's
    human-monitor-command, which returns the monitor's output without the
    line editing.

    THE PAYLOAD IS A FLOPPY

    A 1.44 MB FAT12 image built by tools\v86_bench.mjs's make_floppy, the same
    function run_v86.ps1's payload floppy comes from -- one generator, so a
    build that reaches DOS and a build that reaches Windows 95 were laid out
    by the same code.  `push` swaps it in through QMP's
    blockdev-change-medium while the guest runs, so a rebuild costs a second
    rather than a boot.  It is attached read-only, which is not caution about
    the guest: savevm refuses to snapshot a VM that has a writable device
    which cannot hold snapshots, and a raw floppy is exactly that.  Read-only
    makes it invisible to savevm and the snapshots work.

    Win95 notices the swap because QEMU raises the floppy controller's disk
    change line, which is the same signal a real drive gives; the guest
    re-reads the FAT rather than trusting its cache.  Open the file from a new
    A:\ listing after a push, not from a window left open across it.

    SNAPSHOTS ARE THE POINT

    Windows 95 takes two to three minutes to reach a desktop.  `save desktop`
    stores the live machine inside the qcow2 and `load desktop` restores it in
    about a second, so the iteration loop is push, load, type, shot -- and a
    boot is something that happened once.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('setup', 'run', 'stop', 'status', 'shot', 'push', 'demo', 'keys', 'type', 'click', 'move', 'save', 'load', 'snapshots', 'monitor', 'qmp')]
    [string]$Command = 'status',

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$Rest,

    [string]$Image,
    [string]$Payload,

    # The file `demo` opens.  A real source file rather than a toy one:
    # the point of the demo is to see highlighting, wrapping and the line
    # index doing their jobs on code somebody actually wrote.  It is
    # copied to the floppy as DEMO.C because FAT12 here is 8.3 only.
    [string]$Source,
    [int]$Memory = 64,
    # Pentium II, not Pentium, and the difference is CMOV. Every modern
    # compiler targeting "i686" emits CMOVcc, so a build made today faults on
    # its first conditional move on a real Pentium -- Windows 95 catches it as
    # "executed an invalid instruction" and blames the program. Default to a
    # CPU that has it, and pass -Cpu pentium deliberately when the question
    # being asked is whether the build still runs on a 1995 machine.
    [string]$Cpu = 'pentium2',
    [switch]$Force,
    [switch]$Vnc,

    # A real QEMU window, for a person rather than a script.  The guest
    # gets an ordinary PS/2 mouse this way and it works normally: what
    # cannot be scripted is *absolute* pointing, which needs a USB tablet,
    # and Windows 95 has no USB.  A human moving a relative mouse around a
    # window is exactly what the guest expects.  Click in the window to
    # let QEMU grab the pointer and Ctrl+Alt+G to get it back.
    [switch]$Gui,

    # Which front end -Gui asks for.  Windows builds normally carry both
    # gtk and sdl; gtk is the one with a menu bar for swapping media by
    # hand.  Named here so a build missing it can be worked around
    # without editing the script.
    [string]$Display = 'gtk'
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$work  = Join-Path $root 'tools\qemu95'
$disk  = Join-Path $work 'win95.qcow2'
$flop  = Join-Path $work 'payload.img'
$state = Join-Path $work 'state.json'

# QEMU wants forward slashes in a filename it parses out of a command string,
# and takes them everywhere else, so normalising once is simpler than deciding
# per call site.
function To-QemuPath([string]$p) { return ($p -replace '\\', '/') }

<# --------------------------------------------------------------- finding QEMU #>

# PATH first, then the places the Windows installer and the portable builds
# put it.  Only if all of those miss is anything downloaded, and the download
# is the last resort rather than the plan: a machine with QEMU already on it
# should never grow a second copy.
function Find-Qemu {
    $onPath = Get-Command qemu-system-i386 -ErrorAction SilentlyContinue
    if ($onPath) { return (Split-Path -Parent $onPath.Source) }

    $dirs = @(
        "$env:ProgramFiles\qemu"
        "${env:ProgramFiles(x86)}\qemu"
        "$env:LOCALAPPDATA\qemu"
        'C:\qemu'
        'C:\Program Files\qemu'
        (Join-Path $work 'qemu')
    )
    foreach ($d in $dirs) {
        if ($d -and (Test-Path (Join-Path $d 'qemu-system-i386.exe'))) { return $d }
    }
    return $null
}

function Get-Qemu {
    $d = Find-Qemu
    if (-not $d) { throw "QEMU was not found. Run tools\qemu95.ps1 setup first." }
    return $d
}

<# ------------------------------------------------------------------ QMP client #>

# One QMP exchange per connection.  QEMU accepts repeated connections on the
# socket and the capability handshake costs a round trip on a loopback socket,
# so a connection per command is not worth the state a persistent one would
# need to carry between subcommand invocations of this script.
function Invoke-Qmp([string]$json, [int]$TimeoutMs = 60000) {
    $st = Get-State
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.Connect('127.0.0.1', $st.qmp)
    } catch {
        throw "Nothing is listening on the QMP port $($st.qmp). Is the guest running? Try tools\qemu95.ps1 run."
    }
    try {
        $stream = $client.GetStream()
        $stream.ReadTimeout = $TimeoutMs
        $reader = New-Object System.IO.StreamReader($stream)
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.AutoFlush = $true

        $null = $reader.ReadLine()                      # the greeting
        $writer.WriteLine('{"execute":"qmp_capabilities"}')
        $null = $reader.ReadLine()

        $writer.WriteLine($json)

        # Asynchronous events -- a guest reset, a device tray opening -- share
        # the socket with replies, so the loop skips anything that is not one.
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { throw 'QEMU closed the QMP connection without replying.' }
            if ($line -match '"return"' -or $line -match '"error"') {
                $obj = $line | ConvertFrom-Json
                if ($obj.PSObject.Properties.Name -contains 'error') {
                    throw "QMP refused the command: $($obj.error.class): $($obj.error.desc)"
                }
                return $obj.return
            }
        }
    } finally {
        $client.Close()
    }
}

# savevm and loadvm never got QMP commands of their own.  This is the
# supported way to reach them, and unlike a raw monitor socket the reply comes
# back clean.
function Invoke-Monitor([string]$cmd, [int]$TimeoutMs = 120000) {
    $payload = @{
        execute   = 'human-monitor-command'
        arguments = @{ 'command-line' = $cmd }
    } | ConvertTo-Json -Compress -Depth 5
    return (Invoke-Qmp $payload $TimeoutMs)
}

<# ----------------------------------------------------------------- state file #>

function Get-State {
    if (-not (Test-Path $state)) { throw "No running guest recorded. Run tools\qemu95.ps1 run first." }
    return (Get-Content $state -Raw | ConvertFrom-Json)
}

function Test-Running {
    if (-not (Test-Path $state)) { return $false }
    $st = Get-Content $state -Raw | ConvertFrom-Json
    $p = Get-Process -Id $st.pid -ErrorAction SilentlyContinue
    if (-not $p) { return $false }
    # A recycled PID belonging to something else is not our guest.
    if ($p.ProcessName -notlike 'qemu-system*') { return $false }
    return $true
}

# Ask the OS for a port nobody holds rather than picking a number and hoping.
function Get-FreePort {
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    $port = $l.LocalEndpoint.Port
    $l.Stop()
    return $port
}

<# -------------------------------------------------------------- payload floppy #>

# make_floppy lives in tools\v86_bench.mjs and is exported for exactly this.
# The shim is generated rather than committed because it is four lines of glue
# whose only job is to turn an export into a file on disk.
function Build-Floppy([string]$payloadDir, [string]$out) {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { throw 'Node was not found on PATH; the floppy generator in tools\v86_bench.mjs needs it.' }

    $files = @(Get-ChildItem $payloadDir -File | Where-Object { $_.Length -gt 0 })
    if (-not $files) { throw "Nothing to put on the floppy in $payloadDir" }

    $shim = Join-Path $work 'mkfloppy.mjs'
    $bench = To-QemuPath (Join-Path $root 'tools\v86_bench.mjs')
    @"
import fs from "node:fs";
import { make_floppy } from "file:///$bench";
const [out, ...files] = process.argv.slice(2);
fs.writeFileSync(out, make_floppy(files));
"@ | Set-Content -Path $shim -Encoding UTF8

    $argv = @($shim, $out) + ($files | ForEach-Object { $_.FullName })
    & $node.Source @argv
    if ($LASTEXITCODE -ne 0) { throw 'The floppy generator failed.' }

    Write-Host "Floppy: $(($files | ForEach-Object { $_.Name }) -join ' ') -> $out"
}

<# ------------------------------------------------------------------- keyboard #>

# QMP send-key speaks QKeyCode names, which are neither scan codes nor the
# characters they produce, so a printable character needs a name and possibly
# a shift.  Only the ASCII a US layout reaches is here; that is what a path,
# a filename and a command line are made of.
$script:KeyNames = @{
    'a'='a';'b'='b';'c'='c';'d'='d';'e'='e';'f'='f';'g'='g';'h'='h';'i'='i';'j'='j'
    'k'='k';'l'='l';'m'='m';'n'='n';'o'='o';'p'='p';'q'='q';'r'='r';'s'='s';'t'='t'
    'u'='u';'v'='v';'w'='w';'x'='x';'y'='y';'z'='z'
    '0'='0';'1'='1';'2'='2';'3'='3';'4'='4';'5'='5';'6'='6';'7'='7';'8'='8';'9'='9'
    ' '='spc';'-'='minus';'='='equal';'['='bracket_left';']'='bracket_right'
    '\'='backslash';';'='semicolon';"'"='apostrophe';'`'='grave_accent'
    ','='comma';'.'='dot';'/'='slash'
}
$script:ShiftedKeys = @{
    '!'='1';'@'='2';'#'='3';'$'='4';'%'='5';'^'='6';'&'='7';'*'='8';'('='9';')'='0'
    '_'='minus';'+'='equal';'{'='bracket_left';'}'='bracket_right';'|'='backslash'
    ':'='semicolon';'"'='apostrophe';'~'='grave_accent';'<'='comma';'>'='dot';'?'='slash'
}

function Send-Keys([string[]]$combos) {
    foreach ($combo in $combos) {
        # "ctrl-esc" and "alt-f4" are QEMU's own spelling for a chord and the
        # one sendkey takes, so it is kept rather than invented over.
        $parts = $combo.Split('-') | Where-Object { $_ -ne '' }
        $keys = @()
        foreach ($p in $parts) { $keys += @{ type = 'qcode'; data = $p } }
        $payload = @{
            execute   = 'send-key'
            arguments = @{ keys = $keys; 'hold-time' = 50 }
        } | ConvertTo-Json -Compress -Depth 6
        $null = Invoke-Qmp $payload
        Start-Sleep -Milliseconds 60
    }
}

function Send-Text([string]$text) {
    foreach ($ch in $text.ToCharArray()) {
        $s = [string]$ch
        $lower = $s.ToLower()
        $keys = @()
        if ($script:ShiftedKeys.ContainsKey($s)) {
            $keys = @(@{ type='qcode'; data='shift' }, @{ type='qcode'; data=$script:ShiftedKeys[$s] })
        } elseif ($s -cmatch '^[A-Z]$') {
            $keys = @(@{ type='qcode'; data='shift' }, @{ type='qcode'; data=$lower })
        } elseif ($script:KeyNames.ContainsKey($lower)) {
            $keys = @(@{ type='qcode'; data=$script:KeyNames[$lower] })
        } else {
            throw "No key for the character '$s'; send it with `keys` if it has a QEMU name."
        }
        $payload = @{
            execute   = 'send-key'
            arguments = @{ keys = $keys; 'hold-time' = 40 }
        } | ConvertTo-Json -Compress -Depth 6
        $null = Invoke-Qmp $payload
        # Windows 95 reads the 8042 slowly enough that a burst with no gap
        # loses characters; this is the smallest gap that did not.
        Start-Sleep -Milliseconds 45
    }
}

<# ------------------------------------------------------------------ subcommands #>

function Cmd-Setup {
    New-Item -ItemType Directory -Force $work | Out-Null

    $qdir = Find-Qemu
    if ($qdir) {
        $ver = (& (Join-Path $qdir 'qemu-system-i386.exe') --version | Select-Object -First 1)
        Write-Host "FOUND  QEMU already installed: $qdir"
        Write-Host "       $ver"
    } else {
        Write-Host 'QEMU was not found on PATH or in any of the usual install directories.'
        Write-Host 'Install it and re-run setup. The official Windows builds are at:'
        Write-Host '    https://qemu.weilnetz.de/w64/'
        Write-Host "Or unpack a portable build into $(Join-Path $work 'qemu') and re-run."
        throw 'No QEMU.'
    }

    # The raw image is the user's own, shared with run_v86.ps1, and is never
    # written to here -- the qcow2 is a separate copy so a snapshot chain and
    # a guest that scribbles on itself cannot reach it.
    if (-not $Image) { $Image = [Environment]::GetEnvironmentVariable('NOTE_V86_WIN95') }
    if (-not $Image) { $Image = Join-Path $root 'tools\v86\images\win95.img' }

    if ((Test-Path $disk) -and -not $Force) {
        Write-Host "FOUND  Bootable disk already prepared: $disk"
    } else {
        if (-not (Test-Path $Image)) {
            throw @"
No Windows 95 disk image at $Image.
Windows is not redistributable, so nothing here fetches one -- see the header
of tools\run_v86.ps1. Supply a raw hard disk image of an installed Windows 95
and either set NOTE_V86_WIN95 to its path or put it at
tools\v86\images\win95.img, then re-run setup.
"@
        }
        Write-Host "FOUND  Raw Windows 95 image: $Image ($([int]((Get-Item $Image).Length / 1MB)) MB)"
        Write-Host "       Converting to qcow2 (qcow2 is what savevm needs to store snapshots in)..."
        & (Join-Path $qdir 'qemu-img.exe') convert -f raw -O qcow2 $Image $disk
        if ($LASTEXITCODE -ne 0) { throw 'qemu-img convert failed.' }
        Write-Host "MADE   $disk"
    }

    # An empty payload floppy so the drive exists at boot and push only has to
    # change the medium, never add the device.
    if (-not (Test-Path $flop)) {
        $stub = Join-Path $work 'stub'
        New-Item -ItemType Directory -Force $stub | Out-Null
        Set-Content -Path (Join-Path $stub 'README.TXT') -Value 'Payload floppy. Replaced by tools\qemu95.ps1 push.' -Encoding ASCII
        Build-Floppy $stub $flop
    }

    Write-Host ''
    Write-Host 'Ready. Next: tools\qemu95.ps1 run'
}

function Cmd-Run {
    if (Test-Running) {
        $st = Get-State
        Write-Host "Already running (pid $($st.pid), QMP $($st.qmp)). Use stop first, or load a snapshot."
        return
    }
    if (-not (Test-Path $disk)) { throw "No prepared disk at $disk. Run tools\qemu95.ps1 setup first." }
    $qdir = Get-Qemu

    $qmpPort = Get-FreePort
    $monPort = Get-FreePort
    $vncPort = Get-FreePort

    $qargs = @(
        # A plain 440FX PC is the machine Windows 95 was written for; anything
        # with more than a couple of hundred megabytes of RAM trips its memory
        # manager. See -Cpu above for why the processor is not a Pentium.
        '-M', 'pc'
        '-cpu', $Cpu
        '-m', "$Memory"
        '-drive', "file=$(To-QemuPath $disk),format=qcow2,if=ide,index=0,media=disk"
        # Read-only for savevm's sake, not the guest's -- see the header.
        #
        # if=none plus an explicit -device is the long way round to attach one
        # floppy, and it is here for the id. `if=floppy` gives the drive a
        # BlockBackend called floppy0 but leaves the qdev anonymous, and
        # blockdev-change-medium's supported argument is `id`, which wants a
        # qdev id; without one the only way in is the deprecated `device`
        # argument, and push would break the day that is removed.
        '-drive', "file=$(To-QemuPath $flop),format=raw,if=none,id=fdd0,readonly=on"
        '-device', 'floppy,drive=fdd0,id=floppy0,unit=0'
        '-boot', 'order=c'
        # The image has an NE2000 configured. Booting without one puts a
        # "your network adapter is not working properly" dialog over the
        # desktop that a screenshot then has to be read around.
        '-netdev', 'user,id=net0'
        '-device', 'ne2k_isa,netdev=net0'
        '-rtc', 'base=localtime'
        '-qmp', "tcp:127.0.0.1:$qmpPort,server,nowait"
        # The human monitor is exposed for a person who wants to poke at the
        # running guest by hand. This script never uses it; see the header.
        '-monitor', "tcp:127.0.0.1:$monPort,server,nowait"
    )

    if ($Gui) {
        # Whichever front end this build was compiled with.  screendump keeps
        # working, so `shot` and the rest of the script are unaffected by a
        # window being open -- and none of this reaches the migration stream,
        # so a snapshot taken headless still loads here and the other way
        # round.
        $qargs += @('-display', $Display)
    } elseif ($Vnc) {
        # -vnc still renders, so screendump works either way; this is only for
        # a human who wants to watch. QEMU numbers displays from 5900.
        $qargs += @('-vnc', "127.0.0.1:$($vncPort - 5900)")
    } else {
        $qargs += @('-display', 'none')
    }

    # The "w" binary is the one that does not attach a console window; the
    # plain one flashes a terminal on every launch.
    $exe = Join-Path $qdir 'qemu-system-i386w.exe'
    if (-not (Test-Path $exe)) { $exe = Join-Path $qdir 'qemu-system-i386.exe' }
    if ($Gui) {
        # The windowless build swallows its own error output, so a front end
        # this QEMU was not compiled with would fail silently and look like a
        # guest that simply never appeared.
        $plain = Join-Path $qdir 'qemu-system-i386.exe'
        if (Test-Path $plain) { $exe = $plain }
    }

    $proc = Start-Process -FilePath $exe -ArgumentList $qargs -PassThru

    @{
        pid     = $proc.Id
        qmp     = $qmpPort
        monitor = $monPort
        vnc     = $(if ($Vnc) { $vncPort } else { 0 })
        disk    = $disk
        started = (Get-Date).ToString('o')
    } | ConvertTo-Json | Set-Content -Path $state -Encoding UTF8

    # QEMU opens the QMP socket a moment after the process exists; returning
    # before then makes the very next subcommand fail for no reason.
    $ok = $false
    for ($i = 0; $i -lt 50; $i++) {
        try {
            $t = New-Object System.Net.Sockets.TcpClient
            $t.Connect('127.0.0.1', $qmpPort)
            $t.Close()
            $ok = $true
            break
        } catch { Start-Sleep -Milliseconds 200 }
    }
    if (-not $ok) { throw "QEMU started (pid $($proc.Id)) but never opened its QMP port $qmpPort." }

    Write-Host "Booting: pid $($proc.Id), QMP $qmpPort, monitor $monPort"
    if ($Vnc) { Write-Host "VNC on 127.0.0.1:$vncPort" }
    Write-Host 'Windows 95 needs two to three minutes to reach a desktop. Poll with:'
    Write-Host '    tools\qemu95.ps1 shot boot.png'
}

function Cmd-Stop {
    if (-not (Test-Running)) {
        Write-Host 'Nothing running.'
        if (Test-Path $state) { Remove-Item $state -Force }
        return
    }
    $st = Get-State
    # No ACPI shutdown: Windows 95 predates it and would ignore the request.
    # The disk is a qcow2 nobody is meant to keep changes in, so pulling the
    # plug is the intended exit -- snapshot first if the state matters.
    try { $null = Invoke-Qmp '{"execute":"quit"}' 5000 } catch { }
    Start-Sleep -Milliseconds 500
    $p = Get-Process -Id $st.pid -ErrorAction SilentlyContinue
    if ($p) { Stop-Process -Id $st.pid -Force }
    Remove-Item $state -Force
    Write-Host 'Stopped.'
}

function Cmd-Status {
    if (-not (Test-Path $state)) { Write-Host 'No guest recorded (never run, or stopped).'; return }
    $st = Get-State
    $alive = Test-Running
    Write-Host "pid      $($st.pid)  $(if ($alive) { '(running)' } else { '(gone)' })"
    Write-Host "QMP      127.0.0.1:$($st.qmp)"
    Write-Host "monitor  127.0.0.1:$($st.monitor)"
    if ($st.vnc -gt 0) { Write-Host "VNC      127.0.0.1:$($st.vnc)" }
    Write-Host "disk     $($st.disk)"
    if ($alive) {
        $s = Invoke-Qmp '{"execute":"query-status"}'
        Write-Host "vm       $($s.status)"
    }
}

function Cmd-Shot([string]$out) {
    if (-not $out) { $out = 'shot.png' }
    if (-not [System.IO.Path]::IsPathRooted($out)) { $out = Join-Path (Get-Location).Path $out }
    $dir = Split-Path -Parent $out
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    if (Test-Path $out) { Remove-Item $out -Force }

    $payload = @{
        execute   = 'screendump'
        arguments = @{ filename = (To-QemuPath $out); format = 'png' }
    } | ConvertTo-Json -Compress -Depth 5
    $null = Invoke-Qmp $payload

    if (-not (Test-Path $out)) { throw "QEMU accepted the screendump but wrote no file at $out." }
    Write-Host "$out ($((Get-Item $out).Length) bytes)"
}

<# --------------------------------------------------------------------- mouse #>

# Which pointing device the guest is actually listening to, and whether it
# speaks in absolute coordinates.
#
# This is worth asking rather than assuming, and the assumption that was here
# first was wrong. Windows 95 has no USB, so the reasonable guess is that the
# only pointer is the PS/2 mouse and that a script can therefore only send
# relative motion -- which is a nuisance, because relative motion has to be
# walked from a known corner and Windows accelerates it on the way.
#
# The image this project uses has VBADOS's VBMOUSE.EXE in its AUTOEXEC, which
# speaks the VMware pointer protocol that QEMU implements as "vmmouse". That
# device is absolute, and QEMU makes it the current one, so relative events go
# to a device the guest is not reading and nothing at all happens -- which is
# exactly what the first version of this did. query-mice says so plainly.
function Get-Mouse {
    $mice = Invoke-Qmp '{"execute":"query-mice"}'
    foreach ($m in $mice) { if ($m.current) { return $m } }
    if ($mice) { return $mice[0] }
    throw 'The guest has no pointing device at all.'
}

function Send-Mouse([object[]]$events) {
    $payload = @{
        execute   = 'input-send-event'
        arguments = @{ events = @($events) }
    } | ConvertTo-Json -Compress -Depth 8
    $null = Invoke-Qmp $payload
}

# The guest's screen, in pixels, taken from a screendump rather than guessed:
# the resolution is the guest's business and it can change.
function Get-Screen {
    if ($script:screenW) { return }
    $tmp = Join-Path $env:TEMP ("qemu95-size-" + [guid]::NewGuid().ToString('N') + '.png')
    $payload = @{
        execute   = 'screendump'
        arguments = @{ filename = (To-QemuPath $tmp); format = 'png' }
    } | ConvertTo-Json -Compress -Depth 5
    $null = Invoke-Qmp $payload
    Add-Type -AssemblyName System.Drawing
    $bmp = [System.Drawing.Bitmap]::FromFile($tmp)
    $script:screenW = $bmp.Width
    $script:screenH = $bmp.Height
    $bmp.Dispose()
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
}

# QEMU's absolute axes run 0..0x7FFF across whatever the guest is displaying.
function Abs([string]$axis, [int]$pixel, [int]$span) {
    if ($span -lt 2) { $span = 2 }
    $v = [int][Math]::Round($pixel * 32767.0 / ($span - 1))
    if ($v -lt 0) { $v = 0 }
    if ($v -gt 32767) { $v = 32767 }
    return @{ type = 'abs'; data = @{ axis = $axis; value = $v } }
}

function Rel([string]$axis, [int]$value) {
    return @{ type = 'rel'; data = @{ axis = $axis; value = $value } }
}

# One axis of relative motion, in steps small enough that Windows' pointer
# acceleration does not multiply them. Only reached on a guest whose current
# device is relative; the absolute path needs none of it.
function Step-Axis([string]$axis, [int]$distance, [int]$step = 4) {
    $events = @()
    $left = [Math]::Abs($distance)
    $sign = if ($distance -lt 0) { -1 } else { 1 }
    while ($left -gt 0) {
        $d = [Math]::Min($step, $left)
        $events += Rel $axis ($d * $sign)
        $left -= $d
    }
    for ($i = 0; $i -lt $events.Count; $i += 32) {
        $end = [Math]::Min($i + 31, $events.Count - 1)
        Send-Mouse $events[$i..$end]
        Start-Sleep -Milliseconds 20
    }
}

function Move-Pointer([int]$x, [int]$y) {
    $mouse = Get-Mouse
    if ($mouse.absolute) {
        Get-Screen
        Send-Mouse @((Abs 'x' $x $script:screenW), (Abs 'y' $y $script:screenH))
        return
    }
    # Relative: park against the top-left clamp, which is the only fixed point
    # a relative device offers, then walk out to the target.
    for ($i = 0; $i -lt 12; $i++) { Send-Mouse @((Rel 'x' -120), (Rel 'y' -120)) }
    Start-Sleep -Milliseconds 150
    Step-Axis 'x' $x
    Step-Axis 'y' $y
}

function Cmd-Click([string[]]$rest) {
    if (-not $rest -or $rest.Count -lt 2) {
        throw 'click needs an x and a y in guest screen pixels, e.g. click 512 384'
    }
    $x = [int]$rest[0]
    $y = [int]$rest[1]
    $button = if ($rest.Count -ge 3) { $rest[2] } else { 'left' }

    Move-Pointer $x $y
    Start-Sleep -Milliseconds 250
    Send-Mouse @(@{ type = 'btn'; data = @{ down = $true;  button = $button } })
    Start-Sleep -Milliseconds 120
    Send-Mouse @(@{ type = 'btn'; data = @{ down = $false; button = $button } })
    Write-Host "Clicked $button at $x,$y."
}

# The walk without the click, for checking where the pointer actually landed
# before trusting it -- take a screenshot after this and look for the cursor.
function Cmd-Move([string[]]$rest) {
    if (-not $rest -or $rest.Count -lt 2) { throw 'move needs an x and a y' }
    Move-Pointer ([int]$rest[0]) ([int]$rest[1])
    Write-Host "Pointer at $($rest[0]),$($rest[1])."
}

function Cmd-Push {
    # By default only the executable goes across. build\win32 is a build
    # directory, not a payload: it holds a .map, a .res and a
    # note.exe.manifest, and the last of those has no 8.3 name at all, so
    # handing the whole directory to a FAT12 generator fails on a file the
    # guest was never going to run. Pass -Payload <dir> to send a directory
    # you have curated yourself.
    if (-not $Payload) {
        $exe = Join-Path $root 'build\win32\note.exe'
        if (-not (Test-Path $exe)) {
            throw "No build at $exe. Run ``build.bat x86 own`` from the repo root first."
        }
        $Payload = Join-Path $work 'payload'
        if (Test-Path $Payload) { Remove-Item $Payload -Recurse -Force }
        New-Item -ItemType Directory -Force $Payload | Out-Null
        Copy-Item $exe (Join-Path $Payload 'NOTE.EXE')
    }
    if (-not (Test-Path $Payload)) { throw "No payload directory at $Payload" }
    Build-Floppy $Payload $flop

    if (-not (Test-Running)) {
        Write-Host 'Guest is not running; the floppy is in place for the next run.'
        return
    }

    # blockdev-change-medium, not the monitor's `change`: the human monitor
    # command was removed for block devices, and this is the replacement that
    # raises the disk-change line the guest watches for.
    $payload = @{
        execute   = 'blockdev-change-medium'
        arguments = @{
            id       = 'floppy0'
            filename = (To-QemuPath $flop)
            format   = 'raw'
            'read-only-mode' = 'read-only'
        }
    } | ConvertTo-Json -Compress -Depth 6
    $null = Invoke-Qmp $payload
    Write-Host 'Swapped into the running guest as A:. Re-list A:\ before opening the file.'
}

function Cmd-Demo {
    # Push a build together with something to open, then open it -- the whole
    # loop in one command.
    #
    # Passing the file on the command line rather than steering File>Open is
    # deliberate: the Run dialog is one text field that takes the path in a
    # single burst, whereas a file dialog is several controls whose focus
    # order has to be guessed at blind.  Fewer keystrokes sent hopefully is
    # fewer that can land somewhere unintended.
    if (-not $Source) { $Source = Join-Path $root 'src\core\note_buffer.c' }
    if (-not (Test-Path $Source)) { throw "No source file at $Source" }

    $exe = Join-Path $root 'build\win32\note.exe'
    if (-not (Test-Path $exe)) {
        throw "No build at $exe. Run ``build.bat x86 own`` from the repo root first."
    }

    $dir = Join-Path $work 'payload'
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item $exe (Join-Path $dir 'NOTE.EXE')
    Copy-Item $Source (Join-Path $dir 'DEMO.C')

    # Back to a known desktop before anything else, and this ordering is the
    # whole reason the command exists.  Swapping the floppy under a copy of
    # note that is still running *from* that floppy gets the previous medium
    # back: DOS identifies a disk by its serial number and stops with "Please
    # insert disk NOTE BENCH" when the one in the drive is a different disk.
    # Restoring the snapshot ends every program holding A:, and it also
    # settles the other half of the problem -- a menu left open, a dialog
    # waiting on an answer -- which would otherwise swallow the keystrokes
    # below and scatter the rest across whatever did have focus.
    if (Test-Running) {
        $snaps = Invoke-Monitor 'info snapshots'
        if ($snaps -match '\bdesktop\b') {
            Cmd-Load 'desktop'
            Start-Sleep -Milliseconds 800
        } else {
            Write-Host "No 'desktop' snapshot; using the guest as it stands."
            Send-Keys @('esc', 'esc', 'esc')
            Start-Sleep -Milliseconds 500
        }
    }

    $script:Payload = $dir
    Cmd-Push
    if (-not (Test-Running)) { return }
    Start-Sleep -Milliseconds 500

    # Ctrl+Esc opens the Start menu, r picks Run.  The generous pauses are
    # the whole trick: the guest is emulated and its menus animate, and a
    # chord sent while Windows is still opening one goes nowhere.
    Send-Keys @('ctrl-esc')
    Start-Sleep -Milliseconds 1500
    # r opens the Run dialog outright -- no Enter.  Windows treats a unique
    # accelerator in this menu as a choice, not just a move, and a screenshot
    # taken a second later still showing Run merely highlighted is the guest
    # being slow rather than the key needing confirmation.  An Enter here is
    # actively harmful: by the time it lands the dialog has focus with its
    # last command selected, so it runs the *history* -- which is how this
    # ended up launching sysedit and typing a path into AUTOEXEC.BAT.
    Send-Keys @('r')

    # Three seconds for one dialog looks absurd and is not.  Keystrokes sent
    # before it has focus are not dropped, they are queued, and they arrive
    # wherever focus lands next.
    Start-Sleep -Seconds 3

    # Select whatever the field already holds before replacing it.  It opens
    # with its previous contents selected already, but only when it has a
    # history to offer, and a first run on a fresh snapshot does not.
    Send-Keys @('end', 'shift-home')
    Start-Sleep -Milliseconds 500
    Send-Text 'A:\NOTE.EXE A:\DEMO.C'
    Start-Sleep -Seconds 1
    Send-Keys @('ret')

    # Loading a program off an emulated floppy is not quick.
    Start-Sleep -Seconds 5

    $shot = Join-Path $work 'demo.png'
    Cmd-Shot $shot
    Write-Host "Opened $(Split-Path $Source -Leaf) as A:\DEMO.C"
}

function Cmd-Save([string]$name) {
    if (-not $name) { $name = 'desktop' }
    # Re-saving a tag that exists is the normal case -- the desktop snapshot
    # gets retaken whenever the machine's devices change -- and savevm will
    # not overwrite one, so the old one goes first. It failing is fine and
    # means there was nothing there.
    try { $null = Invoke-Monitor "delvm $name" 60000 } catch { }
    # savevm can take a while: it writes the whole of RAM into the qcow2.
    $out = Invoke-Monitor "savevm $name" 300000
    if ($out -and $out.Trim()) { Write-Host $out.Trim() }
    Write-Host "Saved snapshot '$name'."
}

function Cmd-Load([string]$name) {
    if (-not $name) { $name = 'desktop' }
    $out = Invoke-Monitor "loadvm $name" 300000
    if ($out -and $out.Trim()) { Write-Host $out.Trim() }
    Write-Host "Loaded snapshot '$name'."
}

function Cmd-Snapshots {
    $out = Invoke-Monitor 'info snapshots'
    Write-Host $out
}

<# ----------------------------------------------------------------------- main #>

switch ($Command) {
    'setup'     { Cmd-Setup }
    'run'       { Cmd-Run }
    'stop'      { Cmd-Stop }
    'status'    { Cmd-Status }
    'shot'      { Cmd-Shot ($Rest -join ' ') }
    'push'      { Cmd-Push }
    'demo'      { if ($Rest) { $Source = $Rest -join ' ' }; Cmd-Demo }
    'keys'      { if (-not $Rest) { throw 'keys needs at least one combination, e.g. ctrl-esc' }; Send-Keys $Rest }
    'click'     { Cmd-Click $Rest }
    'move'      { Cmd-Move $Rest }
    'type'      { if (-not $Rest) { throw 'type needs some text' }; Send-Text ($Rest -join ' ') }
    'save'      { Cmd-Save ($Rest -join ' ') }
    'load'      { Cmd-Load ($Rest -join ' ') }
    'snapshots' { Cmd-Snapshots }
    'monitor'   { Write-Host (Invoke-Monitor ($Rest -join ' ')) }
    'qmp'       { Invoke-Qmp ($Rest -join ' ') | ConvertTo-Json -Depth 10 }
}
