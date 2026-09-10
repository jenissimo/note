<#
    run_retro.ps1 — launches a retro build under its emulator.

        tools\run_retro.ps1 dos [file]      DOSBox-X + build\dos\note-dos.exe
        tools\run_retro.ps1 c64 [file]      VICE x64sc + build\c64\note-c64.prg

    Add -Capture <path> to grab a screenshot after -Seconds and quit, which is
    how these builds get verified without a human watching the window.  Add
    -Keys to type into the editor first: plain text goes through as typed, and
    a few names in braces stand for keys that have no printable form --
    {ESC} {ENTER} {F1}..{F10} {UP} {DOWN} {LEFT} {RIGHT} {HOME} {END}.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('dos', 'c64')][string]$Target,
    [string]$File,
    [string]$Keys,
    [string]$Capture,
    [int]$Seconds = 6
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$dosdir = Join-Path $root 'build\dos'
$c64dir = Join-Path $root 'build\c64'

function Require-Path([string]$path, [string]$what) {
    if (-not (Test-Path $path)) { throw "$what not found at $path" }
    return $path
}

# VICE arrives by winget, whose install directory carries the version in its
# name, so the emulator is looked up rather than assumed.  NOTE_VICE or
# NOTE_DOSBOX in the environment wins, for a machine that keeps them elsewhere.
function Find-Emulator([string]$envVar, [string[]]$candidates, [string]$what) {
    $fromEnv = [Environment]::GetEnvironmentVariable($envVar)
    if ($fromEnv) { return (Require-Path $fromEnv $what) }
    foreach ($c in $candidates) {
        $hit = Get-Item $c -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "$what not found. Set $envVar to its full path, or install it."
}

# The emulators drive the editor through their own autotype facilities, and the
# two spell special keys differently, so each target translates {NAME} itself.
$specials = @{
    'ESC' = 'esc'; 'ENTER' = 'enter'; 'UP' = 'up'; 'DOWN' = 'down'
    'LEFT' = 'left'; 'RIGHT' = 'right'; 'HOME' = 'home'; 'END' = 'end'
}

if ($Target -eq 'dos') {
    $exe = Find-Emulator 'NOTE_DOSBOX' @(
        'C:\dosbox-x\dosbox-x.exe'
        "$env:ProgramFiles\DOSBox-X\dosbox-x.exe"
    ) 'DOSBox-X'
    Require-Path (Join-Path $dosdir 'note-dos.exe') 'note-dos.exe' | Out-Null

    # A generated .conf is the only reliable way to script DOSBox-X: the
    # [autoexec] section runs, and AUTOTYPE feeds keys to the running program.
    $lines = @(
        # output=surface forces DOSBox-X's software renderer.  Its accelerated
        # backends draw through a surface the compositor will not hand back:
        # the window reads as blank white to every screen-capture API, which
        # looks exactly like a guest that has crashed and is not one.
        '[sdl]', 'autolock=false', 'output=surface', ''
        '[dosbox]', 'machine=svga_s3', 'memsize=16', ''
        '[render]', 'aspect=true', ''
        '[autoexec]'
        "MOUNT C `"$dosdir`""
        'C:'
    )
    if ($Keys) {
        # AUTOTYPE wants a space-separated key list, and it queues the keys and
        # returns rather than blocking, so it has to run BEFORE the editor:
        # note-dos.exe holds the command line until it exits, and anything
        # after it would only type into the DOS prompt afterwards.  -w gives
        # the editor time to start and draw before the keys begin arriving.
        $keyList = [System.Collections.Generic.List[string]]::new()
        foreach ($piece in [regex]::Split($Keys, '(\{[A-Z0-9]+\})')) {
            if (-not $piece) { continue }
            if ($piece -match '^\{([A-Z0-9]+)\}$') {
                $name = $Matches[1]
                if ($specials.ContainsKey($name))   { $keyList.Add($specials[$name]) }
                elseif ($name -match '^F([1-9]|10)$') { $keyList.Add($name.ToLower()) }
            } else {
                foreach ($ch in $piece.ToCharArray()) {
                    switch ($ch) {
                        ' '     { $keyList.Add('space') }
                        default { $keyList.Add([string]$ch) }
                    }
                }
            }
        }
        if ($keyList.Count) { $lines += ('AUTOTYPE -w 3 -p 0.08 ' + ($keyList -join ' ')) }
    }

    $cmd = 'note-dos.exe'
    if ($File) { $cmd += " $File" }
    $lines += $cmd

    if ($Capture) {
        # No screenshot command exists, so the frame is taken from outside;
        # the guest just has to stay up long enough to be photographed.
        $lines += 'PAUSE'
    }

    $conf = Join-Path $env:TEMP 'note-retro-dos.conf'
    Set-Content -Path $conf -Value $lines -Encoding ASCII

    $proc = Start-Process -FilePath $exe -ArgumentList @('-conf', "`"$conf`"") -PassThru
}
else {
    $exe = Find-Emulator 'NOTE_VICE' @(
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\VICE-Team.VICE.GTK3_*\*\bin\x64sc.exe"
        'C:\vice\bin\x64sc.exe'
    ) 'VICE (x64sc)'
    $prg = Require-Path (Join-Path $c64dir 'note-c64.prg') 'note-c64.prg'

    # -autostart loads and RUNs the .prg.  Nothing else is passed: VICE 3.10
    # rejects the whole command line over one option it does not know, and a
    # .prg started this way needs no drive settings anyway.
    $args = @('-autostart', "`"$prg`"")

    if ($Keys) {
        # VICE's -keybuf stuffs the keyboard buffer; \n is its line break, and
        # keys with no character have no representation at all, so those are
        # dropped rather than silently mistyped.
        $text = [regex]::Replace($Keys, '\{ENTER\}', "`n")
        $text = [regex]::Replace($text, '\{[A-Z0-9]+\}', '')
        if ($text) { $args += @('-keybuf', "`"$text`"") }
    }

    $proc = Start-Process -FilePath $exe -ArgumentList $args -PassThru
}

if (-not $Capture) {
    Write-Host "Started $Target (pid $($proc.Id)). Close the window when done."
    exit 0
}

Start-Sleep -Seconds $Seconds

# Photograph the emulator's own window rather than the whole desktop, so the
# capture is the same size whatever else happens to be on screen.
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
'@

# Both emulators can hand their window to a process other than the one that
# was started -- VICE relaunches itself through a GTK shim -- so the window is
# looked up by image name, with the started process only as a first guess.
$name = [IO.Path]::GetFileNameWithoutExtension($exe)
$owners = @($proc) + @(Get-Process -Name $name -ErrorAction SilentlyContinue)
$hwnd = [IntPtr]::Zero
foreach ($p in $owners) {
    try { $p.Refresh() } catch { continue }
    if ($p.MainWindowHandle -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
        $hwnd = $p.MainWindowHandle
        break
    }
}

$r = New-Object Win+RECT
$onTop = $false
if ($hwnd -ne [IntPtr]::Zero) {
    [void][Win]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 500
    $onTop = ([Win]::GetForegroundWindow() -eq $hwnd)
    [void][Win]::GetWindowRect($hwnd, [ref]$r)
} else {
    # No window to aim at: photograph the whole desktop rather than fail, so
    # there is still something to look at when diagnosing why.
    $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $r.L = $vs.Left; $r.T = $vs.Top; $r.R = $vs.Right; $r.B = $vs.Bottom
    Write-Warning 'The emulator window was not found; capturing the whole screen.'
}

$w = $r.R - $r.L
$h = $r.B - $r.T
if ($w -le 0 -or $h -le 0) { throw "The capture region is empty ($w x $h)." }

# Copying off the screen is the truthful capture: it shows exactly what an
# emulator drew, hardware surface and all.  It only works while nothing is
# covering the window, so when the window could not be brought to the front,
# fall back to PrintWindow -- which asks the window to redraw itself into our
# bitmap, and which some emulators (DOSBox-X among them) answer with an empty
# frame.  Neither method is right for both, so the choice is made by which
# situation we are actually in.
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g   = [System.Drawing.Graphics]::FromImage($bmp)

if ($onTop) {
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
} else {
    Write-Warning 'The emulator window would not come to the front; asking it to redraw instead.'
    $dc = $g.GetHdc()
    try { [void][Win]::PrintWindow($hwnd, $dc, 2) } finally { $g.ReleaseHdc($dc) }
}
$g.Dispose()

$dir = Split-Path -Parent $Capture
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$bmp.Save($Capture, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

foreach ($p in $owners) { try { $p.Kill() } catch { } }
Write-Host "Captured $Capture"
