@echo off
REM Builds note for the Game Boy: build\note.gb
REM
REM GBDK-2020 drives SDCC but does not carry it, so both have to be on PATH and
REM SDCCDIR has to point at the SDCC tree -- without it sdcpp cannot find cc1
REM and the error blames the source file rather than the setup.
REM
REM Override NOTE_GBDK / NOTE_SDCC to build against another install.

setlocal
set ROOT=%~dp0
set OUT=%ROOT%build\gb

REM The official GBDK-2020 release carries its own SDCC, cc1 included, so the
REM in-project install is both at once.  tools\setup_gb.ps1 downloads it.
REM A GBDK copied out of another project's tree may be missing libexec\sdcc\cc1,
REM which is why NOTE_SDCC exists: point it at a full SDCC and the pair works.
if "%NOTE_GBDK%"=="" set NOTE_GBDK=%ROOT%tools\gbdk-2020
if "%NOTE_SDCC%"=="" set NOTE_SDCC=%NOTE_GBDK%

if not exist "%NOTE_GBDK%\bin\lcc.exe" (
  echo GBDK-2020 not found at %NOTE_GBDK%
  echo Run:  powershell -ExecutionPolicy Bypass -File tools\setup_gb.ps1
  echo   or  set NOTE_GBDK to an existing install.
  exit /b 1
)
if not exist "%NOTE_SDCC%\libexec\sdcc\cc1" (
  if not exist "%NOTE_SDCC%\libexec\sdcc\cc1.exe" (
    echo SDCC at %NOTE_SDCC% has no libexec\sdcc\cc1 -- the install is partial.
    echo Run tools\setup_gb.ps1, or set NOTE_SDCC to a complete SDCC.
    exit /b 1
  )
)

if not exist "%ROOT%build" mkdir "%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"

set SDCCDIR=%NOTE_SDCC%
set PATH=%NOTE_SDCC%\bin;%NOTE_SDCC%\libexec\sdcc;%PATH%

REM The font table is generated from a picture rather than committed as hex;
REM see tools/make_gbfont.py for why.
if not exist "%ROOT%src\platform\gb\gb_font.h" (
  python "%ROOT%tools\make_gbfont.py" "%ROOT%src\platform\gb\gb_font.h" || exit /b 1
)

"%NOTE_GBDK%\bin\lcc.exe" -Wa-l -Wl-m -Wl-j -o "%OUT%\note.gb" ^
  "%ROOT%src\platform\gb\gb_main.c" "%ROOT%src\core\note_buffer.c"
if errorlevel 1 goto :fail

for %%f in ("%OUT%\note.gb") do echo Built %%f  (%%~zf bytes)
endlocal
goto :eof

:fail
echo Build failed.
endlocal
exit /b 1
