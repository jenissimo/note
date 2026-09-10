# Windows 95 under QEMU

`tools\qemu95.ps1` boots a Windows 95 guest that a script can drive: no
window, no focus stolen, screenshots written straight to a PNG. It is the
Win32 counterpart to `tools\run_v86.ps1`, which does the same job for the DOS
build under v86 but cannot photograph a graphical Windows or attach a
debugger.

## What it needs

* **QEMU for Windows.** `setup` looks on `PATH`, then in `C:\Program
  Files\qemu`, `C:\qemu`, `%LOCALAPPDATA%\qemu` and `tools\qemu95\qemu`. If
  none of those has it, install from <https://qemu.weilnetz.de/w64/> and
  re-run.
* **A Windows 95 hard-disk image.** Nothing here downloads one and nothing
  ever will -- Windows is not redistributable. Supply a raw image of an
  installed system and either set `NOTE_V86_WIN95` or put it at
  `tools\v86\images\win95.img`; this is the same image and the same rule as
  `tools\run_v86.ps1`.

Everything the script makes lives in `tools\qemu95\`, which is not committed.

## First run

```
tools\qemu95.ps1 setup          # find QEMU, convert the raw image to qcow2
tools\qemu95.ps1 run            # boot, headless
tools\qemu95.ps1 shot boot.png  # poll until the desktop is there
```

The boot takes two to three minutes. It ends on a "DHCP client was unable to
obtain an IP address" dialog, because QEMU's user-mode network does not answer
the guest's NE2000 quickly enough; dismiss it and you are on the desktop:

```
tools\qemu95.ps1 keys alt-n
tools\qemu95.ps1 save desktop
```

Do that once. From then on `load desktop` puts you back on that desktop in
about a second, and the boot is history.

## The loop

**I changed the code, show me it running on Windows 95:**

```
build.bat x86 own
tools\qemu95.ps1 load desktop
tools\qemu95.ps1 push
tools\qemu95.ps1 keys ctrl-esc r
tools\qemu95.ps1 type "A:\NOTE.EXE"
tools\qemu95.ps1 keys ret
tools\qemu95.ps1 shot note.png
```

`ctrl-esc r` opens the Start menu and picks Run; the rest types the path into
it. Give the guest a second or two between the Enter and the screenshot -- an
Windows 95 loading a program off an emulated floppy is not instant.

If the guest is not running at all, `run` first and `load desktop` after it
has opened its QMP port.

## Driving it by hand

```
tools\qemu95.ps1 stop
tools\qemu95.ps1 run -Gui
tools\qemu95.ps1 load desktop
```

A window opens and the guest is yours: keyboard and mouse both work.  Click
in the window to let QEMU take the pointer and **Ctrl+Alt+G** to get it
back.  The mouse is an ordinary PS/2 one, which is why a *script* cannot aim
it -- sending a pointer to a given coordinate needs absolute positioning,
that needs a USB tablet, and Windows 95 has no USB.  A person moving a
relative mouse has no such trouble.

The display is not part of the migration stream, so this costs nothing:
a snapshot saved headless loads into a windowed guest and the other way
round, and `shot`, `push` and `keys` all keep working while the window is
open.  `-Gui` does change the command line, though, so a guest already
running headless has to be stopped and re-run rather than switched.

If the window never appears, ask for a front end this QEMU actually has:
`qemu-system-i386.exe -display help` lists them, and `-Display sdl` picks
another.

## Subcommands

| | |
|---|---|
| `setup` | Find QEMU; convert the raw image to `tools\qemu95\win95.qcow2`. `-Force` reconverts. |
| `run` | Boot headless. `-Gui` opens a real QEMU window you can use; `-Vnc` serves one over VNC instead; `-Memory 64` sets RAM; `-Cpu` sets the processor. |
| `stop` | Kill the guest. Changes since the last `save` are lost. |
| `status` | PID, ports, and whether the VM is running or paused. |
| `shot <path>` | PNG of the guest's screen. |
| `push` | Put `build\win32\note.exe` on the payload floppy and swap it into the running guest. `-Payload <dir>` sends a directory instead. |
| `keys <combo>...` | `send-key` chords in QEMU's spelling: `ret`, `esc`, `alt-f4`, `ctrl-esc`. |
| `type <text>` | Type ASCII a character at a time. |
| `save <tag>` / `load <tag>` | qcow2 snapshots of the live machine. Tag defaults to `desktop`. |
| `snapshots` | List them. |
| `monitor <cmd>` / `qmp <json>` | Escape hatches, for when this table runs out. |

## How the build gets in

As a 1.44 MB FAT12 floppy, built by the same `make_floppy` in
`tools\v86_bench.mjs` that `run_v86.ps1` uses, and swapped into the running
guest through QMP's `blockdev-change-medium`. The guest sees it as `A:` and
notices the swap because QEMU raises the drive's disk-change line -- but only
when something re-reads the directory, so open the file from a fresh listing
of `A:\` rather than from a window left open across a `push`.

The floppy is attached read-only. That is not caution about the guest: QEMU
refuses to `savevm` a machine that has a writable device which cannot store
snapshots, and a raw floppy image is exactly that. Read-only makes it
invisible to `savevm`, and the snapshots work.

Only `note.exe` goes across by default. `build\win32` is a build directory,
and `note.exe.manifest` in it has no 8.3 name for FAT12 to give it.

## The current build does not run on Windows 95

This is what the stand found the first time it was pointed at `note.exe`, and
it is a fact about the build, not a fault in the stand -- Windows 95's own
Notepad launches, takes typed text and photographs fine through the same four
commands.

`A:\NOTE.EXE` dies on startup with "This program has performed an illegal
operation". `Details>>` gives the instruction, and the instruction changes
with `-Cpu`:

| `-Cpu` | Bytes at CS:EIP | Instruction | Arrived with |
|---|---|---|---|
| `pentium` | `0f 4f c8` | `CMOVG ecx, eax` | Pentium Pro |
| `pentium2` | `66 0f 6e c0`, `66 0f 70 c0 00`, `66 0f fe 05` | `MOVD xmm0, eax`, `PSHUFD`, `PADDD` | Pentium 4 (SSE2) |

Raising `-Cpu` past the `CMOV` fault only uncovers the one behind it. The SSE2
one cannot be fixed by choosing a bigger CPU at all: SSE state has to be
enabled by the operating system through `CR4.OSFXSR`, Windows 95 predates the
register bit and never sets it, so an SSE2 instruction raises #UD on a Windows
95 machine of any vintage.

The cause is the toolchain, not the source: MSVC has defaulted to `/arch:SSE2`
for 32-bit x86 for years, and the vectorised `PADDD` above is the compiler's
own work. A build that runs here needs `/arch:IA32`. That is a `build.bat`
question and this document does not answer it.

`-Cpu pentium2` is the default because it is the most permissive processor
Windows 95 is actually happy on. Pass `-Cpu pentium` when the question is
whether the build runs on a machine of the right era:

```
tools\qemu95.ps1 stop
tools\qemu95.ps1 run -Cpu pentium
```

Snapshots are tied to the machine they were taken on, so a change of `-Cpu`
means retaking the desktop snapshot.

## Notes

* Everything goes over **QMP**, not the human monitor. The human monitor
  echoes each character back with cursor-movement escapes as it is typed, so a
  reply is the command tangled up in its own echo; a `screendump` to a long
  Windows path fails there for no reason a script can diagnose. QMP is
  line-based JSON with no echo. `savevm` and `loadvm` never got QMP commands,
  so they go through `human-monitor-command`, which returns the monitor's
  output without the line editing.
* The human monitor is still exposed on its own port for a person who wants
  to poke at a running guest by hand -- `status` prints the port.
* The mouse is not scriptable. Absolute pointing needs a USB tablet and
  Windows 95 has no USB. Drive the guest from the keyboard.
* A `save` of a booted desktop only reloads on a machine with the same
  devices. Change the QEMU command line in `run` and the old snapshot has to
  be retaken -- `save` deletes and rewrites a tag, so just take it again.
* `-Vnc` is for watching, not for screenshots: `shot` works headless either
  way.
