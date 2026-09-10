@echo off
REM Builds note for MS-DOS and the Commodore 64.
REM Usage: build-retro.bat [dos|c64]   (default: both)
REM
REM Both targets come from one source, src\platform\console\console_main.c,
REM plus the same note_buffer.c the Windows build uses.  What they do not
REM share is note_core.c: it reaches for a syntax registry, a theme registry,
REM a palette and a regex engine, which on a machine with 64 KB in total is
REM not a trade worth making.

setlocal
set ROOT=%~dp0
set DOSOUT=%ROOT%build\dos
set C64OUT=%ROOT%build\c64
set WHAT=%1
if "%WHAT%"=="" set WHAT=both

if not exist "%ROOT%build" mkdir "%ROOT%build"

set "SRC=%ROOT%src\platform\console\console_main.c %ROOT%src\core\note_buffer.c"

REM ---- MS-DOS -------------------------------------------------------------
REM DJGPP, so 32-bit protected mode: flat memory and no 64 KB segments, at the
REM cost of needing a DPMI host beside the executable.  CWSDPMI.EXE is copied
REM along for that reason -- without it the program will not start on a bare
REM DOS.
if "%WHAT%"=="c64" goto :c64

set "DJGPP=C:\djgpp\bin\i586-pc-msdosdjgpp-gcc.exe"
if not exist "%DJGPP%" (
  echo DJGPP not found at %DJGPP% -- skipping the DOS build.
  goto :c64
)

if not exist "%DOSOUT%" mkdir "%DOSOUT%"
"%DJGPP%" -Os -Wall -o "%DOSOUT%\note-dos.exe" %SRC%
if errorlevel 1 goto :fail
if exist "C:\djgpp\bin\CWSDPMI.EXE" copy /y "C:\djgpp\bin\CWSDPMI.EXE" "%DOSOUT%\" >nul

REM The theme catalogue, inside the executable.
REM
REM This arm has the registry (NOTE_THEME_CATALOGUE) and reduces every theme
REM onto the sixteen colours a CGA card has, so -Catalogue: all 338
REM definitions, not the curated fourteen note.exe carries.  This is a
REM standalone DJGPP build of a third of a megabyte with a DPMI host beside
REM it and no size budget on it at all, so the whole catalogue is free here
REM in a way it is not in a 128 KB dual binary.
REM
REM It has no PE resource directory to keep them in either.  What it has
REM instead is its own file: the blob is appended to note-dos.exe and found
REM again at startup by scanning back from the end for the NPK1 magic, which
REM is what note_pack.h put the magic there for.
REM
REM Appended rather than compiled in because nothing has to agree about it: no
REM generated C, no linker script, and the DJGPP loader stops at the end of the
REM last section and never looks at what follows.
if not exist "%ROOT%assets\themes.pack" goto :packed
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\compress_packs.ps1" "%DOSOUT%" -Themes -Catalogue
if errorlevel 1 goto :fail
copy /b "%DOSOUT%\note-dos.exe" + "%DOSOUT%\themes.lz" "%DOSOUT%\note-dos.tmp" >nul
if errorlevel 1 goto :fail
move /y "%DOSOUT%\note-dos.tmp" "%DOSOUT%\note-dos.exe" >nul
del "%DOSOUT%\themes.lz" >nul 2>&1
:packed
for %%f in ("%DOSOUT%\note-dos.exe") do echo Built %%f  (%%~zf bytes)

if "%WHAT%"=="dos" goto :done

REM ---- Commodore 64 -------------------------------------------------------
REM cc65 with 16-bit ints and no heap to speak of; see the caps at the top of
REM console_main.c for what the machine actually leaves us.
:c64
set "CC65=C:\cc65\bin\cl65.exe"
if not exist "%CC65%" (
  echo cc65 not found at %CC65% -- skipping the C64 build.
  goto :done
)

if not exist "%C64OUT%" mkdir "%C64OUT%"
"%CC65%" -t c64 -O -o "%C64OUT%\note-c64.prg" %SRC%
if errorlevel 1 goto :fail
for %%f in ("%C64OUT%\note-c64.prg") do echo Built %%f  (%%~zf bytes)

:done
endlocal
goto :eof

:fail
echo Build failed.
endlocal
exit /b 1
