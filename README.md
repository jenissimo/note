# note

A small, fast text editor. One executable, no runtime, no installer.

Tabs, session restore for unsaved work, line numbers, syntax highlighting and
light/dark themes — with the editing itself done by the platform's own native
text control rather than a reimplementation of one.

```
build\win32\note.exe      155 KB   x86, the default
build\win32\note.min.exe  113 KB   the same binary, UPX-compressed
```

That is one file with two programs in it. The PE is the Windows editor; the
MZ stub at the front of it — the real-mode program that normally does nothing
but print *This program cannot be run in DOS mode* -- is a 16-bit editor that
runs on MS-DOS. The same `note.exe` starts on MS-DOS, on Windows 95 and on
Windows 11. See [One file, two editors](#one-file-two-editors).

## Building

Needs Visual Studio 2022 with the C++ toolset. From a shell in the repo root:

```
build.bat            REM x86, the default
build.bat x64
build.bat own        REM the owner-drawn text view instead of RICHEDIT
```

x86 by default because it is smaller for the same sources — 156 KB against
180 — and a text editor has no use for a 64-bit address space. `own` builds
`win32_view.c`, note's own text window over the core's gap buffer, instead of
the RICHEDIT control; it is the same buffer the MS-DOS, C64 and Game Boy ports
edit, and it is built alongside the shipped path rather than in place of it, so
the two can be compared while the view matures.

The result lands in `build\win32\`, together with the syntax pack. Nothing
else is required at runtime: the binary links no CRT and imports only system
DLLs plus `Msftedit.dll`. The fourteen curated themes are inside the
executable; the other 324 are `assets/themes.pack` in this repo, and dropping
that file into the folder note already searches loads every one of them.

The build also copies the executable to `%LOCALAPPDATA%\Microsoft\WindowsApps`
so `note` works from any shell. That directory is where Windows keeps app
execution aliases: it is already on every user's PATH, so nothing has to edit
the environment, and it needs no elevation — unlike dropping a binary into
`C:\Windows`. The packs go to `%LOCALAPPDATA%\note`, which note searches
anyway, so the alias directory does not collect data files.

The application icon is generated rather than committed as an opaque blob:

```
python tools/make_icon.py assets/note.ico
```

`src/note.rc` compiles it in along with a version block. Both have to be
resources rather than something loaded at startup, because Explorer draws a
file's icon by reading it out of the executable without ever running it.

## How it is put together

The point of the layout is that the interesting half of an editor is not
platform-specific, and the half that is should be genuinely native rather than
a lookalike.

```
src/core/          no OS headers, no CRT, C89 throughout
  note_config.h      every size bound and feature switch, per build profile
  note_core.*        documents, commands, the menu model, encodings, session
  note_conf.*        the key/value format definitions are written in
  note_syntax.*      language registry and the lexer
  note_regex.*       a Pike VM, for the lexer's pattern rules and for Find
  note_theme.*       palette registry
  note_palette.*     the command palette's rows and its filtering
  note_buffer.*      a gap buffer with its own undo log and line index
  note_pack.*        finding one definition inside a pack without parsing it
  note_reduce.*      fitting a theme onto a fixed hardware palette
src/platform/win32/
  note_win32.h       the state and types the backend shares across its parts
  win32_main.c       the window, the message loop, startup
  win32_edit.c       the RICHEDIT control, the gutter, the highlighter
  win32_menu.c       menus, the caption and the tab strip, all hand-drawn
  win32_palette.c    the palette overlay and its list modes
  win32_help.c       the key sheet F1 puts up
  win32_chrome.c     tabs, the status bar, applying a theme
  win32_dialogs.c    the system dialogs: open, save, font, print
  win32_host.c       files, folders, the clock — the plain services
src/platform/console/
  console_main.c     one source for MS-DOS, 16-bit MS-DOS and the Commodore 64
  font_terminus.h    a VGA character set the DOS build uploads at startup
src/platform/gb/
  gb_main.c          the Game Boy: 40 columns on a 160x144 screen
  gb_basic.*         a tokenising BASIC, because a ROM cannot load a compiler
```

The backend was one file until it passed three and a half thousand lines, at
which point it was the thing preventing two people from working on the editor
at once. Splitting it cost something real: `struct note_host` had to move into
a header, so state that was private to one file is now visible to seven. At
that size the privacy was already nominal — the gutter, the tab strip and the
message loop were all reading the same handles — so the header makes explicit
what was true anyway.

The core never calls the system. Everything it cannot do itself it asks for
through `note_host_ops` — about forty function pointers covering the text
control, the tab strip, dialogs, files and the clock. A backend is that table
plus a message loop.

What lives in the core is the part users would notice diverging between
platforms: the menu and accelerator tables, what each command means, the
dirty/discard flow, how a file's encoding and line endings are preserved, the
session format, and the syntax lexer. What lives in the backend is everything
that should feel native: real menus, a real file dialog, the platform's own
text control and its undo, IME and accessibility.

### Portability notes

The core is strict C89 with no CRT, no floating point and no assumption that
`int` is wider than 16 bits, and every buffer bound is named in
`note_config.h` rather than scattered through the code. `NOTE_PROFILE_TINY`
selects a small profile with one document, no session and no syntax tables.

That is what opened the door to machines with a few tens of kilobytes, and
`note_buffer.c` is what walked through it: a gap buffer with selection, undo
and a line index, which the backends with no native text control own
themselves. Four of them now do — 32-bit MS-DOS, 16-bit MS-DOS, the
Commodore 64 and the Game Boy — and past that buffer such a backend is
mostly drawing.

## Definitions

Languages and themes are plain text, not code. The compiled-in defaults are
written in the same format and parsed by the same code as anything on disk, so
a file can express everything a built-in can.

Loaded in this order, each layer overriding the last by name:

```
1. compiled-in defaults
2. <folder containing note.exe>\syntax.pack   \themes.pack
3. <folder containing note.exe>\syntax\*.syntax   \themes\*.theme
4. %LOCALAPPDATA%\note\syntax.pack   \themes.pack
5. %LOCALAPPDATA%\note\syntax\*.syntax   \themes\*.theme
```

A pack is simply many definitions in one file separated by a line of dashes.
The shipped packs hold 143 languages and 338 palettes; keeping them in two
files rather than several hundred is what keeps startup at roughly 170 ms.

A language is word lists plus, where a list cannot say it, pattern rules:

```
name = C
extensions = c h ii i def
line_comment = //
block_comment = /* */
quotes = "'
preproc = yes
keywords = if else for while return ...
types = int char float ...
rule = operator [-+*/%=<>.:;,~&|^!?]
rule = number \b0[Xx][0-9A-Fa-f]+\b
rule = type \b[A-Z_][0-9A-Z_]+\b
```

Words stay lists on purpose: a lookup is cheaper than an automaton and a
hundred keywords cost a few hundred bytes instead of a program that would not
fit. Rules carry what lists cannot express — operators, the shape of a number
literal, "an identifier in capitals is a constant" — as regular expressions
run by `note_regex`, a Pike VM that is linear in the input, so a pattern like
`(a+)+b` cannot hang the editor the way a backtracking engine would.

Rules are compiled only for the language of the document on screen, and one
that will not compile is dropped rather than breaking its language. `rule`
kinds are `keyword`, `type`, `comment`, `string`, `number`, `preproc` and
`operator`.

A theme, with colours as `#RRGGBB`:

```
name = Gruvbox dark, hard
dark = yes
background = #1d2021
foreground = #d5c4a1
gutter_bg  = #1d2021
gutter_fg  = #665c54
selection  = #504945
ui_bg      = #3c3836
ui_fg      = #bdae93
keyword    = #d3869b
type       = #fabd2f
comment    = #665c54
string     = #b8bb26
number     = #fe8019
preproc    = #8ec07c
operator   = #d5c4a1
```

To change a shipped definition, copy it out of the pack into a file of its own
under `%LOCALAPPDATA%\note\` and edit that; a later layer wins.

The packs are converted from two open collections — see `assets/NOTICE.md` for
attribution and licences. Regenerate them with:

```
python tools/import_packs.py --fetch
```

The converter compiles the upstream regex grammars down to this format: an
alternation that is only words becomes a word list, everything else becomes a
rule, and POSIX bracket expressions are rewritten as ordinary classes. Of the
1017 rules it produces, 25 are rejected by the engine as too large or using a
construct it lacks — those languages lose one pattern each, not the language.

## MS-DOS and the Commodore 64

The first two of the four backends that predate every API the Windows version
talks to. Both are 32-bit-or-nothing in their own way: DJGPP needs a DPMI host,
cc65 needs a 6502. The other two, the 16-bit DOS arm and the Game Boy, have
sections of their own below.

```
build-retro.bat          REM both
build-retro.bat dos
build-retro.bat c64
```

```
build\note-dos.exe       DJGPP, 32-bit protected mode, 80x25
build\note-c64.prg       cc65, 6502, 40x25
```

DOS needs DJGPP in `C:\djgpp` and the C64 needs cc65 in `C:\cc65`; the script
says which one is missing and builds the other rather than stopping.
`CWSDPMI.EXE` is copied beside `note-dos.exe`, because a DJGPP binary is a
protected-mode program and will not start on bare DOS without a DPMI host.

Both come from one source file, `src/platform/console/console_main.c`, over the
same `note_buffer.c` — a gap buffer with its own undo log and line index — that
the desktop version is moving to. What they leave behind is `note_core.c`: the
syntax registry, theme registry and regex engine assume more memory than a C64
has in total, so the console build carries its own smaller versions.

To run either under an emulator:

```
tools\run_retro.ps1 c64
tools\run_retro.ps1 dos demo.txt
tools\run_retro.ps1 dos demo.txt -Keys "hello{ENTER}world" -Capture shot.png
```

`-Capture` waits `-Seconds`, photographs the emulator's window and quits, which
is how these builds get checked without a person watching the screen. `-Keys`
types into the editor first, with `{ENTER}`, `{ESC}`, `{F1}`, `{UP}` and
friends for the keys that have no printable form — DOSBox-X takes them through
`AUTOTYPE`, VICE through `-keybuf`, which only understands characters, so the
C64 silently drops the rest rather than mistyping them.

DOSBox-X is expected in `C:\dosbox-x` and VICE wherever winget put it; set
`NOTE_DOSBOX` or `NOTE_VICE` to override either.

Two things about that harness are worth knowing before trusting a capture.
DOSBox-X is started with `output=surface`, its software renderer: the
accelerated backends draw through a surface Windows will not hand back, so the
window reads as blank white to every screen-capture API — which looks exactly
like a guest that has crashed, and is not one. And `AUTOTYPE` is issued *before*
the editor, because `note-dos.exe` holds the command line until it exits;
anything after it would only type at the DOS prompt afterwards.

The C64 side is thinner. `-keybuf` delivers a whole string to BASIC but only the
first couple of characters reach an autostarted program, and it cannot express a
key that has no character at all, so `Ctrl`-anything is out of reach. Both are
the same missing piece: VICE's binary monitor, which would also let a test read
screen RAM at `$0400` instead of photographing a window.

### What the C64 build does not have

`Run`, and not by oversight. Running a BASIC program means tokenising the text
into BASIC's program area at `$0801` and jumping to `RUN` — and `$0801` is
exactly where note itself is loaded. Doing it properly means either relocating
the editor above the BASIC area or copying the tokenised program down through a
trampoline in memory neither program owns. Either is real work, and a Run that
overwrote the editor mid-copy would be worse than none.

## One file, two editors

```
build-dos16.bat
build\dos16\note16.exe   39 KB    Open Watcom, 8086 real mode, small model
```

Every PE begins with an MZ header describing a real-mode program, and in every
other executable on the machine that program prints one line and exits. It does
not have to. `note16.exe` is 8086 code in the small model, small enough and
plain enough to be the `/STUB:` image of `build\win32\note.exe`, so the same
file is a Windows editor to Windows and a DOS editor to DOS. That is also why
the Windows binary targets subsystem version 4.00: a PE that claims to need a
later Windows is refused by Windows 95 before any of this matters.

What the 16-bit arm gives up against the DJGPP one is the uploadable font and
the room a single 64 KB data segment does not have. What it gains is that it
runs on an 8086 with no DPMI host, which is the only way the stub trick works
at all.

The curated themes ride at the end of the file rather than in `.rsrc`, past the
last section, and both halves find them by the same rule: the last `NPK1` magic
in the last 64 KB. A resource directory is no use to a real-mode program — it
cannot walk one and cannot run the loader that would expand it — but it can
open its own file and seek.

Open Watcom is the only compiler still emitting 16-bit real-mode code.
`tools\setup_watcom.ps1` downloads it into `tools\watcom\`; set `NOTE_WATCOM`
to build against another install.

## The Game Boy

```
build-gb.bat
build\gb\note.gb         32 KB    GBDK-2020 and SDCC
```

A text editor on a machine with eight buttons, 8 KB of RAM and a 160x144
screen — 20 tiles across, half of what a line of text needs. It draws 40
columns by giving each character half a tile and composing the pairs at
runtime, and it takes text in through an on-screen keyboard, because with eight
buttons there is no other way in.

It also carries a small tokenising BASIC (`gb_basic.c`): keywords are stored as
single bytes, which is what makes a program fit in the few kilobytes a
cartridge's battery-backed SRAM has, and is the reason a `Run` exists here and
not on the C64 — on the Game Boy the interpreter is note's own and knows
where it put things.

`tools\setup_gb.ps1` downloads GBDK-2020 into `tools\gbdk-2020\`; it carries
its own SDCC. `tools\run_gb.ps1` starts the ROM in an emulator.

## Emulator stands

The retro targets are checked without a person watching a screen, which needs a
machine a script can start, type into and photograph:

```
tools\run_retro.ps1 dos demo.txt     DOSBox-X
tools\run_retro.ps1 c64              VICE
tools\run_v86.ps1                    v86 in a browser, FreeDOS
tools\qemu95.ps1                     QEMU, Windows 95
tools\run_gb.ps1                     a Game Boy emulator
```

Nothing here downloads an operating system that is not free to redistribute.
The v86 and QEMU stands boot a Windows 95 image *you* supply; `setup` says
where to put one and stops if it is missing. `tools/v86/`, `tools/qemu95/`,
`tools/gbdk-2020/` and `tools/watcom/` are installed into the tree by their own
setup scripts and none of them is committed — they are hundreds of megabytes
of prebuilt binaries, and in the Windows case not ours to ship.

## Tests

Six programs, one per thing worth being sure of. They are host programs and may
use the CRT; the code they test may not. Each is a `cl` line and an executable,
so there is nothing to install and nothing to configure:

```
cl /nologo /W4 /TC tests\test_buffer.c  src\core\note_buffer.c
cl /nologo /W4 /TC tests\test_regex.c   src\core\note_regex.c
cl /nologo /W4 /TC tests\test_pack.c    src\core\note_pack.c
cl /nologo /W4 /TC tests\test_reduce.c  src\core\note_reduce.c
cl /nologo /W4 /TC tests\test_gbbasic.c src\platform\gb\gb_basic.c
cl /nologo /W4 /TC tests\test_syntax.c  src\core\note_syntax.c ^
   src\core\note_conf.c src\core\note_regex.c src\core\note_pack.c
```

`test_buffer` covers the gap buffer the console ports edit with — insertion
and deletion at every position, undo grouping, the line index across edits,
search with wrapping and whole-word matching, and filling the buffer to
capacity to check that it refuses further text without corrupting what is
already there. It finishes by timing an edit against document size, which is
how "a 10 MB file still types in microseconds" stays a fact rather than a
claim.

`test_regex` covers the engine: every syntax construct, capture offsets,
malformed patterns, and a pathological `(a+)+b` that must stay fast.

`test_syntax` covers the registry and the lexer, and finishes on the shipped
pack, which it needs as an argument:

```
test_syntax.exe assets\syntax.pack
```

It parses all 143 definitions, reports how many rules the engine turns down,
and checks that C really does colour a hex literal, an operator and an all-caps
name.

`test_pack` covers finding one definition inside a pack without parsing the
rest, including what happens to a blob with the wrong magic or a truncated
header. `test_reduce` covers fitting a theme onto a fixed hardware palette —
the C64's sixteen colours and the Game Boy's four greys — and fuzzes random
themes against random palettes to check the documented merge order holds.
`test_gbbasic` covers the Game Boy BASIC: tokenising, detokenising back to the
same text, and the integer evaluator.

## Session

Unsaved work is not lost by closing note. Every few seconds, and on exit, each
modified buffer is written to `%LOCALAPPDATA%\note\` alongside an index of the
open tabs, the chosen theme and the four view switches -- word wrap, the
status bar, the line numbers and the highlighting -- and restored on the next
launch. A status bar that came back every time it was dismissed would not
really have been dismissed. Closing a single tab still asks, because that
discards the buffer for good.

## Keys

| | |
|---|---|
| `F1` | the key sheet — every binding below, read off the same tables |
| `Ctrl+N` / `Ctrl+W` | new tab / close tab |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | next / previous tab |
| `Ctrl+O` `Ctrl+S` `Ctrl+Shift+S` `Ctrl+P` | open, save, save as, print |
| `Ctrl+Shift+O` | open by typing a path, with completion |
| `Ctrl+F` `F3` `Shift+F3` `Ctrl+H` `Ctrl+G` | find, again, back, replace, go to |
| `Ctrl+Z` `Ctrl+Y` `Ctrl+A` | undo, redo, select all |
| `Tab` / `Shift+Tab` | indent / outdent the selected lines |
| `Ctrl+Shift+N` `Ctrl+Shift+X` `Ctrl+Shift+B` | line numbers, syntax, status bar |
| `Ctrl++` `Ctrl+-` `Ctrl+0` | zoom |
| `F5` | insert time and date |
| `Ctrl+R` | run a command from this file's folder |
| `Ctrl+K` | the command palette |
| `Shift`, `Shift` | go to an open tab |

The sheet `F1` puts up is not a list written out a second time: `note_help_fill()`
walks the same menu and accelerator tables the menu bar and the accelerators are
built from, so a shortcut that changes in one place changes on the sheet, and one
that is only ever documented cannot exist.

## Running a command

`Ctrl+R` opens the palette on a command line whose working directory is the
folder of the file on screen, which is the difference between an editor and
somewhere you can work: `make`, `python %file%`, `cl /nologo thing.c`, a test
run — without first telling a shell where you already are.

The rows are what has been run before, most recent first, so repeating the last
command is two keys; typing anything replaces it, because the query is the
answer rather than a filter. A named document that has unsaved changes is
written out first — running the file as it was two edits ago is the one way
this can waste an afternoon.

On Windows the command goes to `cmd.exe /k` in a console of its own: the output
is the point, and a window that closed the moment the command finished would
hide it. On MS-DOS there is one screen and one process, so note hands the
machine over — ROM font, plain attributes, cleared screen — runs the command,
waits for a key, and takes it all back. The C64 has neither a shell nor a second
process, so it has no Run; what it should have instead is `RUN`, and that is a
separate piece of work (see below).

## Opening a file by typing its path

`Ctrl+Shift+O` opens the palette on a path rather than a filter.  Everything up
to the last separator is a folder, which gets listed; what follows filters the
listing, `Tab` and `Shift+Tab` walk the matches, and a folder brings its own
separator so completing to one descends into it.  The listing is filtered as it
is read rather than afterwards, so a folder with ten thousand files in it costs
what the few matching names cost.

It does not replace the system dialog on `Ctrl+O`.  Someone who knows where the
file is should not have to find it in a tree, and someone who does not should
still get the dialog that can search, preview and sort.

## Highlighting

RichEdit keeps formatting once it is set, so the useful unit of work is not
"colour the screen" but "extend the range that is already coloured".  Each
document carries the stretch of it known to be coloured; a pass fills in only
what that does not cover.  The viewport is always finished before the pass
returns — half a coloured screen is worse than none — and a screenful either
side of it is filled in from a timer, a chunk at a time, so the message loop
keeps its turn.  Holding `Page Down` then costs one screenful per press instead
of recolouring the same viewport on every frame, and costs the same on a
thirteen-megabyte file as on a small one.

A worker thread would not help.  The expensive half is not the lexing — a
screenful is a couple of milliseconds — but the `EM_SETCHARFORMAT` calls, and
those have to happen on the thread that owns the control.

The line the caret is on is washed across the gutter and the text.  The control
has no notion of a highlighted line and nothing can paint underneath it, so the
band under the text is a background given to that paragraph's own characters,
and the empty ends of each row — the margin before the first character, and
everything from the last one out to the scroll bar — are filled in after the
control has painted.  Neither the wash nor anything else here is named by a
theme: both are mixed from colours every theme does name, so all 338 of the
shipped palettes have them.

## Licence

MIT, in `LICENSE`.

The shipped syntax and theme packs are converted from two MIT-licensed
collections, and the DOS build's font is under the SIL Open Font License;
`assets/NOTICE.md` names all three and says what was changed.
