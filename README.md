# note

A small, fast text editor. One executable, no runtime, no installer.

Tabs, session restore for unsaved work, line numbers, syntax highlighting and
light/dark themes — with the editing itself done by the platform's own native
text control rather than a reimplementation of one.

```
build\note.exe            ~62 KB
```

## Building

Needs Visual Studio 2022 with the C++ toolset. From a shell in the repo root:

```
build.bat            REM x64 (default)
build.bat x86
```

The result lands in `build\`, together with the definition packs. Nothing else
is required at runtime: the binary links no CRT and imports only system DLLs
plus `Msftedit.dll`.

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

That leaves the door open to machines with a few tens of kilobytes, but does
not by itself open it: the text-control operations currently assume a native
widget owns the buffer. A DOS or 6502 backend would first need a `note_buffer`
in the core — a gap buffer with selection and undo — implementing those
operations itself, after which such a backend is mostly drawing.

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

The same editor builds for two machines that predate every API the Windows
version talks to:

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

## Tests

Both test programs are host programs and may use the CRT; the code they test
may not. Build and run either with, for example:

```
cl /nologo /W4 /TC tests\test_syntax.c src\core\note_syntax.c ^
   src\core\note_conf.c src\core\note_regex.c
test_syntax.exe assets\syntax.pack
```

`test_regex.c` covers the engine — every syntax construct, capture offsets,
malformed patterns, and a pathological `(a+)+b` that must stay fast.
`test_syntax.c` covers the registry and lexer, and finishes on the shipped
pack: it parses all 143 definitions, reports how many rules the engine turns
down, and checks that C really does colour a hex literal, an operator and an
all-caps name. `test_buffer.c` covers the gap buffer the console ports edit
with — insertion and deletion at every position, undo grouping, the line index
across edits, search with wrapping and whole-word matching, and filling the
buffer to capacity to check that it refuses further text without corrupting
what is already there:

```
cl /nologo /W4 /TC tests\test_buffer.c src\core\note_buffer.c
test_buffer.exe
```

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
