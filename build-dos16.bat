@echo off
REM Builds note for 16-bit real-mode MS-DOS: build\dos16\note16.exe
REM
REM The second DOS target, and not a duplicate of the first.  build-retro.bat
REM builds DJGPP: 32-bit protected mode, a DPMI host beside it, three hundred
REM kilobytes.  This one is 8086 code in the small model, which is what lets it
REM be the /STUB: image of a Win32 PE -- the MZ header at the front of every PE
REM describes a real-mode program, normally the one that prints "This program
REM cannot be run in DOS mode", and it can just as well describe an editor.
REM
REM Same source as both of the others: src\platform\console\console_main.c plus
REM note_buffer.c.  What this arm gives up against DJGPP is the uploadable VGA
REM font -- see the caps at the top of console_main.c for that and for the sizes
REM one 64 KB data segment leaves.
REM
REM Override NOTE_WATCOM to build against another Open Watcom install.
REM
REM One thing this does not do: note16.exe is not yet in the shape /STUB: wants.
REM Watcom writes a three-paragraph MZ header with the relocations at 0x20, so
REM byte 0x3C -- where link.exe puts e_lfanew, the offset of the PE header -- is
REM not header at all but the program's own first instructions, and the linker
REM overwrites four of them.  That is LNK4060.  The header has to be expanded to
REM 0x40 and the relocations moved with it; tools\stubdemo\build_stubdemo.ps1
REM has the routine and the explanation.  Whatever links the PE does that.

setlocal
set ROOT=%~dp0
set OUT=%ROOT%build\dos16

if "%NOTE_WATCOM%"=="" set NOTE_WATCOM=%ROOT%tools\watcom

if not exist "%NOTE_WATCOM%\binnt64\wcl.exe" (
  echo Open Watcom not found at %NOTE_WATCOM%
  echo Run:  powershell -ExecutionPolicy Bypass -File tools\setup_watcom.ps1
  echo   or  set NOTE_WATCOM to an existing install.
  exit /b 1
)

if not exist "%ROOT%build" mkdir "%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"

REM setup_watcom.ps1 installs and deliberately sets nothing, so every consumer
REM says what it needs.  For a DOS build that is these three.
set WATCOM=%NOTE_WATCOM%
set INCLUDE=%NOTE_WATCOM%\h
set PATH=%NOTE_WATCOM%\binnt64;%PATH%

REM -0 is 8086 code, so it runs on the machines this is for as well as on the
REM ones it will be a stub on.  -bcl=dos compiles and links a DOS executable in
REM one step, -ms is the small model (one 64 KB code segment, one 64 KB data
REM segment), -os optimises for size, and -k8192 is the stack: the default four
REM kilobytes is not enough for the syntax parser's line buffer and the regex
REM compiler's recursion at the same time.
REM
REM -fm= asks for the map file, which is where the only number that matters
REM lives: _TEXT against the small model's 64 KB.
pushd "%OUT%"
REM note_pack.c is here and not in build-retro.bat's list because this is the
REM DOS target that will read its definitions out of its own executable: the
REM same file is note.exe's MS-DOS stub, so the pack embedded for the Win32
REM half is sitting in it already.  build-retro.bat's DJGPP build is a
REM standalone note-dos.exe with nothing in it to find, which is why
REM NOTE_EMBEDDED_PACKS is off there and this needs no switch to turn it on.
wcl -q -0 -bcl=dos -ms -os -k8192 -fe=note16.exe -fm=note16.map ^
  "%ROOT%src\platform\console\console_main.c" "%ROOT%src\core\note_buffer.c" ^
  "%ROOT%src\core\note_pack.c"
if errorlevel 1 goto :fail
popd

for %%f in ("%OUT%\note16.exe") do echo Built %%f  (%%~zf bytes)
echo Segments:
findstr /b "_TEXT CONST CONST2 _DATA _BSS STACK" "%OUT%\note16.map"

endlocal
goto :eof

:fail
popd
echo Build failed.
endlocal
exit /b 1
