@echo off
REM Builds note.exe with MSVC, no CRT.
REM Usage: build.bat [x86|x64] [own]   (default: x86, RICHEDIT)
REM
REM x86 by default because it is measurably smaller for the same sources --
REM 90 KB against 112 -- and a text editor has no use for a 64-bit address
REM space.  Pass x64 if you want it; everything works either way.
REM
REM "own" builds the owner-drawn text view (src\platform\win32\win32_view.c)
REM instead of the RICHEDIT control: note's own window over the core's gap
REM buffer, the same one the MS-DOS, C64 and Game Boy ports edit.  Without it
REM the editor is built exactly as it shipped, so the two can be compared side
REM by side and the RICHEDIT path keeps working while the view matures.
REM Setting NOTE_VIEW=own in the environment does the same thing.

setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x86
if /I "%1"=="own" set NOTE_VIEW=own
if /I "%1"=="own" set ARCH=x86
if /I "%2"=="own" set NOTE_VIEW=own

REM One define, one source file and one object: everything else about the
REM build is the same either way.
set "VIEWDEF="
set "VIEWSRC="
set "VIEWOBJ="
if /I "%NOTE_VIEW%"=="own" set "VIEWDEF=/D NOTE_OWN_VIEW=1"
if /I "%NOTE_VIEW%"=="own" set VIEWSRC="%~dp0src\platform\win32\win32_view.c" "%~dp0src\core\note_buffer.c"

set ROOT=%~dp0

REM Each target keeps its own directory under build\ -- win32 here, gb, dos and
REM c64 from the other two scripts -- so a Game Boy ROM and a note.exe never
REM share a folder and "what did this target produce" is one listing.
REM
REM NOTE_OUT, NOTE_CFLAGS and NOTE_LDFLAGS let a measurement build go to its
REM own directory with extra switches, so comparing configurations never
REM disturbs the real build or races another one over the same note.exe.
if defined NOTE_OUT (set "OUT=%NOTE_OUT%") else (set "OUT=%ROOT%build\win32")
if /I "%NOTE_VIEW%"=="own" set VIEWOBJ="%OUT%\win32_view.obj" "%OUT%\note_buffer.obj"

REM Locate vcvarsall.bat.  Paths under "Program Files (x86)" carry a closing
REM paren that would end an if/for block early, so each probe is its own call.
set "VCVARS="
call :probe "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
call :probe "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
call :probe "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
call :probe "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
call :probe "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
call :probe "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community"
call :probe "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"

if not defined VCVARS (
  echo Visual Studio with the C++ toolset was not found.
  exit /b 1
)

call "%VCVARS%" %ARCH% >nul
if errorlevel 1 exit /b 1

if not exist "%ROOT%build" mkdir "%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"

REM The instruction set, and the single most important flag in this file.
REM
REM MSVC compiles 32-bit x86 for /arch:SSE2 unless told otherwise -- it has
REM since VS2012, silently, with no warning and nothing in the linker output
REM to show for it.  SSE needs CR4.OSFXSR set by the operating system before
REM the first SSE instruction will execute; Windows 95 predates SSE entirely
REM and never sets it, so every one of them raises #UD there no matter what
REM processor the machine actually has.  Raising the emulated CPU does not
REM help and cannot.
REM
REM The optimiser only vectorises where it finds a loop worth vectorising, so
REM this does not stop the program starting -- it kills it at whichever
REM feature the user reaches first.  Ours was switching themes, where a
REM 256-entry palette gets copied twice and came out as PSHUFD/PADDD.
REM
REM IA32 is as far down as this compiler goes.  It still emits CMOV, which
REM wants a Pentium Pro; that is inside what Windows 95 itself supports, and
REM the 16-bit half in the MZ stub is what actually has to run on a 386.
set ISA=
if /I not "%ARCH%"=="x64" set ISA=/arch:IA32

REM Stack probes.  x86 supplies its own _chkstk (see win32_main.c), so the
REM compiler is free to probe and a frame larger than a page grows the stack
REM instead of stepping over its guard page.  That cost us one crash before
REM the SSE2 one, in note_syntax_add, and the fix there was to move two
REM buffers to static; with the helper present the compiler handles it and
REM the whole class is gone.  Its absence from note.map is the proof that no
REM frame currently needs it.  x64 cannot: that contract is not expressible
REM without an assembler, so the x64 build keeps the old promise.
set PROBE=
if /I "%ARCH%"=="x64" set PROBE=/Gs1000000
cl /nologo /c /O1 /Os /GS- %PROBE% %ISA% /Gy /W4 /wd4100 /wd4127 /wd4201 ^
   /utf-8 /D_UNICODE /DUNICODE /GL %VIEWDEF% %NOTE_CFLAGS% ^
   /Fo"%OUT%\\" ^
   %VIEWSRC% ^
   "%ROOT%src\core\note_core.c" ^
   "%ROOT%src\core\note_conf.c" ^
   "%ROOT%src\core\note_pack.c" ^
   "%ROOT%src\core\note_syntax.c" ^
   "%ROOT%src\core\note_regex.c" ^
   "%ROOT%src\core\note_theme.c" ^
   "%ROOT%src\core\note_reduce.c" ^
   "%ROOT%src\core\note_palette.c" ^
   "%ROOT%src\platform\win32\win32_edit.c" ^
   "%ROOT%src\platform\win32\win32_chrome.c" ^
   "%ROOT%src\platform\win32\win32_menu.c" ^
   "%ROOT%src\platform\win32\win32_dialogs.c" ^
   "%ROOT%src\platform\win32\win32_host.c" ^
   "%ROOT%src\platform\win32\win32_palette.c" ^
   "%ROOT%src\platform\win32\win32_help.c" ^
   "%ROOT%src\platform\win32\win32_ansi.c"
if errorlevel 1 exit /b 1

REM win32_main.c holds the memset/memcpy the compiler still emits calls to in
REM a build with no CRT.  /GL refuses to let a program define those -- it
REM treats them as its own library helpers -- so this one file is compiled on
REM its own, without whatever extra flags a measurement build is trying.
cl /nologo /c /O1 /Os /GS- %PROBE% %ISA% /Gy /W4 /wd4100 /wd4127 /wd4201 ^
   /utf-8 /D_UNICODE /DUNICODE %VIEWDEF% ^
   /Fo"%OUT%\\" "%ROOT%src\platform\win32\win32_main.c"
if errorlevel 1 exit /b 1

REM Compress the curated packs for embedding.  Failure here is not fatal: the
REM executable still runs, with whatever packs are found on disk beside it and
REM no highlighting at all if there are none.
if exist "%ROOT%assets\core.syntax.pack" (
  powershell -NoProfile -ExecutionPolicy Bypass ^
    -File "%ROOT%tools\compress_packs.ps1" "%OUT%" 2>nul
)

REM And the curated theme pack, which does not go in .rsrc.  It is appended
REM to the linked file further down -- see the block that does it for why.
if exist "%ROOT%assets\core.themes.pack" (
  powershell -NoProfile -ExecutionPolicy Bypass ^
    -File "%ROOT%tools\compress_packs.ps1" "%OUT%" -Themes 2>nul
)

set "PACKDEF="
if exist "%OUT%\core.syntax.lz" set "PACKDEF=/d HAVE_PACKS"

REM The icon and version block.  Explorer reads these out of the file without
REM running it, so they have to be compiled in rather than loaded at startup.
set "RES="
if exist "%ROOT%src\note.rc" (
  rc /nologo %PACKDEF% /i "%ROOT%src" /i "%OUT%" /fo "%OUT%\note.res" "%ROOT%src\note.rc"
  if errorlevel 1 exit /b 1
  set "RES=%OUT%\note.res"
)

REM The MS-DOS half.  Every PE starts with an MZ header describing a real-mode
REM program, and /STUB: says which one: given build-dos16.bat's note16.exe, the
REM same file is a Win32 editor to Windows and a 16-bit editor to DOS.  It has
REM to have its header expanded first -- tools\dos_stub.ps1 explains why, and
REM does it.  Optional like the packs and the icon: Open Watcom is a 145 MB
REM download and nobody needs it to build note for Windows, so without a
REM build\dos16\note16.exe this links exactly as it always did, with the
REM linker's own "cannot be run in DOS mode" stub.
set "STUB="
del "%OUT%\note.stub.exe" 2>nul
if exist "%ROOT%build\dos16\note16.exe" (
  powershell -NoProfile -ExecutionPolicy Bypass ^
    -File "%ROOT%tools\dos_stub.ps1" "%ROOT%build\dos16\note16.exe" "%OUT%\note.stub.exe"
  if exist "%OUT%\note.stub.exe" set "STUB=/STUB:%OUT%\note.stub.exe"
)

REM No uxtheme.lib or dwmapi.lib here on purpose.  The loader resolves every
REM import before the program's first instruction, so an import of either one
REM turns "this Windows has no compositor" into "this Windows cannot start
REM note".  Both are looked up by name at startup instead -- see caps_probe().
REM The subsystem version Windows 95 wants is 4.00, and this linker will not
REM write it: support for anything below 5.01 went when support for XP went,
REM and /SUBSYSTEM:WINDOWS,4.0 earns LNK4010 and the default instead.  The
REM field is set after the link by tools\pe_target.ps1, which explains why
REM that is a header edit rather than a trick.
link /nologo /SUBSYSTEM:WINDOWS /ENTRY:noteEntry /NODEFAULTLIB ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO /LTCG /MAP:"%OUT%\note.map" %STUB% %NOTE_LDFLAGS% ^
     /OUT:"%OUT%\note.exe" ^
     "%OUT%\note_core.obj" "%OUT%\note_conf.obj" "%OUT%\note_syntax.obj" ^
     "%OUT%\note_pack.obj" ^
     "%OUT%\note_regex.obj" ^
     "%OUT%\note_theme.obj" "%OUT%\note_palette.obj" ^
     "%OUT%\note_reduce.obj" ^
     "%OUT%\win32_main.obj" "%OUT%\win32_edit.obj" "%OUT%\win32_chrome.obj" ^
     "%OUT%\win32_menu.obj" "%OUT%\win32_dialogs.obj" "%OUT%\win32_host.obj" ^
     "%OUT%\win32_palette.obj" "%OUT%\win32_help.obj" ^
     "%OUT%\win32_ansi.obj" ^
     %VIEWOBJ% ^
     %RES% ^
     kernel32.lib user32.lib gdi32.lib comdlg32.lib shell32.lib ^
     advapi32.lib
if errorlevel 1 exit /b 1

REM Which Windows the file admits to needing.  Four words of the optional
REM header, nothing in the DOS stub, so the check below still sees what it
REM handed over.
powershell -NoProfile -ExecutionPolicy Bypass ^
  -File "%ROOT%tools\pe_target.ps1" "%OUT%\note.exe" 4 0
if errorlevel 1 exit /b 1

REM And check it came through.  A stub whose first instructions the linker
REM overwrote still produces a note.exe Windows loads and runs, so nothing here
REM would look wrong; the DOS half would simply crash on the first machine that
REM tried it.  This compares the embedded stub against the file that went in.
if defined STUB (
  powershell -NoProfile -ExecutionPolicy Bypass ^
    -File "%ROOT%tools\dos_stub.ps1" -Verify "%OUT%\note.exe" "%OUT%\note.stub.exe"
  if errorlevel 1 exit /b 1
)

REM THE THEMES, APPENDED
REM
REM The curated fourteen, compressed to an NPK1 blob and
REM stuck on the end of the file past the last section.  Not a resource, and
REM that is the whole point: this executable is two programs -- the PE for
REM Windows and the 16-bit real-mode editor in its MZ stub for MS-DOS -- and
REM a real-mode program cannot walk a PE resource directory or run the loader
REM that would expand one.  It can open its own file and seek.  So the one
REM region both halves can reach is the end, and both find the blob by the
REM same rule: the last NPK1 in the last 64 KB.  The last, because four bytes
REM of magic turn up by chance in this much code.
REM
REM One physical copy.  Keeping IDR_CORE_THEMES in .rsrc as well would put
REM the same themes in the file twice, which a 128 KB budget notices.
REM
REM The curated pack and not assets\themes.pack, which is the full 338.  That
REM substitution is worth 25,559 bytes compressed and is the whole of what a
REM 128 KB budget cannot absorb; the catalogue rides in note-dos.exe, which
REM is standalone and has no budget.  Everything you need in one file,
REM nothing fetched at startup, is the design this number goes with.
REM
REM After the stub verify, so that what that compared is what went in, and
REM before pack_exe.ps1, which has to see the overlay to preserve it.
if exist "%OUT%\themes.lz" (
  copy /b "%OUT%\note.exe" + "%OUT%\themes.lz" "%OUT%\note.tmp" >nul
  if errorlevel 1 exit /b 1
  move /y "%OUT%\note.tmp" "%OUT%\note.exe" >nul
)

REM note looks for definitions beside the executable, so the syntax pack ships
REM next to it.  It is optional: without it the compiled-in languages still
REM work.  themes.pack does not ship here any more: the curated fourteen are
REM inside the executable, and the other 324 are a download away in the repo
REM for anyone who wants them -- dropping the pack into the folder note
REM already searches still loads every one of them at startup.
if exist "%ROOT%assets\syntax.pack" copy /y "%ROOT%assets\syntax.pack" "%OUT%\" >nul
if exist "%ROOT%assets\NOTICE.md"   copy /y "%ROOT%assets\NOTICE.md"   "%OUT%\" >nul

del "%OUT%\note_core.obj" "%OUT%\note_conf.obj" "%OUT%\note_syntax.obj" 2>nul
del "%OUT%\note_regex.obj" "%OUT%\note_pack.obj" 2>nul
del "%OUT%\note_theme.obj" "%OUT%\note_palette.obj" 2>nul
del "%OUT%\note_reduce.obj" 2>nul
del "%OUT%\win32_main.obj" "%OUT%\win32_edit.obj" "%OUT%\win32_chrome.obj" 2>nul
del "%OUT%\win32_menu.obj" "%OUT%\win32_dialogs.obj" "%OUT%\win32_host.obj" 2>nul
del "%OUT%\win32_palette.obj" "%OUT%\win32_help.obj" 2>nul
del "%OUT%\win32_ansi.obj" 2>nul
del "%OUT%\win32_view.obj" "%OUT%\note_buffer.obj" 2>nul
for %%f in ("%OUT%\note.exe") do echo Built %%f  (%%~zf bytes)

REM A UPX-compressed copy beside the real one, when UPX is installed.
REM See tools\pack_exe.ps1 for why it is a copy and never the shipped file.
powershell -NoProfile -ExecutionPolicy Bypass ^
  -File "%ROOT%tools\pack_exe.ps1" "%OUT%" 2>nul

REM Put it on PATH without touching PATH.  Every user already has
REM %LOCALAPPDATA%\Microsoft\WindowsApps on their path -- it is where Windows
REM keeps app execution aliases -- and it needs no elevation, unlike dropping
REM a third-party binary into C:\Windows.  Only the executable goes there; the
REM definition packs go to the per-user folder note already searches, so the
REM alias directory does not collect a third of a megabyte of data files.
REM A measurement build is not an installation, and neither is a build of the
REM view: it installs nothing over the note.exe on PATH until it is the
REM default, so a day's work on it cannot cost anyone their editor.
if defined NOTE_OUT goto :done
if /I "%NOTE_VIEW%"=="own" goto :done

set "BIN=%LOCALAPPDATA%\Microsoft\WindowsApps"
set "SHARE=%LOCALAPPDATA%\note"
if exist "%BIN%" (
  copy /y "%OUT%\note.exe" "%BIN%\note.exe" >nul
  if errorlevel 1 (
    echo   not installed: note.exe is running, close it and build again
  ) else (
    if not exist "%SHARE%" mkdir "%SHARE%"
    if exist "%ROOT%assets\syntax.pack" copy /y "%ROOT%assets\syntax.pack" "%SHARE%\" >nul
    echo   installed to %BIN%\note.exe  -- run it as: note
  )
)

:done
endlocal
goto :eof

:probe
if defined VCVARS goto :eof
if exist "%~1\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%~1\VC\Auxiliary\Build\vcvarsall.bat"
goto :eof
