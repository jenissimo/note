<#
    run_gb.ps1 -- runs build\gb\note.gb under mGBA and, optionally, drives it.

        tools\run_gb.ps1
        tools\run_gb.ps1 -Buttons "RIGHT RIGHT A A" -Capture shot.png

    Buttons are Game Boy names -- UP DOWN LEFT RIGHT A B START SELECT -- and are
    translated to mGBA's default bindings.  A name may be repeated with a count,
    "RIGHT*5", and "WAIT*30" pauses for that many frames.

    -Capture asks mGBA for its own screenshot rather than photographing the
    window: the result is exactly 160x144 real pixels, which for a 3x5 font is
    the difference between reading the screen and guessing at it.
#>
[CmdletBinding()]
param(
    [string]$Buttons,
    [string]$Capture,
    [int]$Scale = 4,
    [int]$Seconds = 4
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$rom  = Join-Path $root 'build\gb\note.gb'

if (-not (Test-Path $rom)) { throw "No ROM at $rom -- run build-gb.bat first." }

$mgba = $env:NOTE_MGBA
if (-not $mgba) { $mgba = 'C:\Projects\eira-quest\external\emulator\mGBA.exe' }
if (-not (Test-Path $mgba)) { throw "mGBA not found at $mgba. Set NOTE_MGBA." }

# mGBA's defaults.  F12 is its screenshot key; it writes <rom>-N.png beside the
# ROM, which is why the ROM lives in build\gb\ and the captures are moved out.
$vk = @{
    'UP' = 0x26; 'DOWN' = 0x28; 'LEFT' = 0x25; 'RIGHT' = 0x27
    'A' = 0x58; 'B' = 0x5A; 'START' = 0x0D; 'SELECT' = 0x08
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class GbWin {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, IntPtr e);

  /* Windows refuses a foreground change from a process that is not already
     there, which is exactly our situation.  Attaching to the current
     foreground thread's input queue for the length of the call is the
     sanctioned way round it, and without it every keystroke below would go to
     whatever the user was actually looking at. */
  public static bool Focus(IntPtr h) {
    uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
    uint me = GetCurrentThreadId();
    AttachThreadInput(me, fg, true);
    ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
    AttachThreadInput(me, fg, false);
    return GetForegroundWindow() == h;
  }
}
'@

function Send-Key([byte]$code, [int]$holdMs = 60) {
    [GbWin]::keybd_event($code, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $holdMs
    [GbWin]::keybd_event($code, 0, 2, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 50
}

# Old captures would be numbered after, not over, so they go first.
Get-ChildItem (Join-Path $root 'build\gb') -Filter 'note-*.png' -ErrorAction SilentlyContinue |
    Remove-Item -Force

$proc = Start-Process $mgba -ArgumentList @("-$Scale", "`"$rom`"") -PassThru
Start-Sleep -Seconds $Seconds
$proc.Refresh()

if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { $proc.Kill(); throw 'mGBA opened no window.' }
if (-not [GbWin]::Focus($proc.MainWindowHandle)) {
    $proc.Kill(); throw 'Could not bring mGBA to the front; keys would go elsewhere.'
}
Start-Sleep -Milliseconds 500

foreach ($token in ($Buttons -split '\s+' | Where-Object { $_ })) {
    $name = $token; $count = 1
    if ($token -match '^(.+)\*(\d+)$') { $name = $Matches[1]; $count = [int]$Matches[2] }
    $name = $name.ToUpper()

    if ($name -eq 'WAIT') { Start-Sleep -Milliseconds ($count * 17); continue }
    if (-not $vk.ContainsKey($name)) { throw "Unknown button '$name'." }
    for ($i = 0; $i -lt $count; $i++) { Send-Key ([byte]$vk[$name]) }
}

if (-not $Capture) {
    Write-Host "Running (pid $($proc.Id)). Close the window when done."
    exit 0
}

Start-Sleep -Milliseconds 400
Send-Key 0x7B 100                      # F12
Start-Sleep -Seconds 2

$shot = Get-ChildItem (Join-Path $root 'build\gb') -Filter 'note-*.png' |
        Sort-Object LastWriteTime | Select-Object -Last 1
try { $proc.Kill() } catch { }

if (-not $shot) { throw 'mGBA took no screenshot.' }

$dir = Split-Path -Parent $Capture
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
Move-Item $shot.FullName $Capture -Force
Write-Host "Captured $Capture"
