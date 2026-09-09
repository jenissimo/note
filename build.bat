@echo off
REM Builds note.exe with MSVC, no CRT.
REM Usage: build.bat [x86|x64]   (default: x86)
REM
REM x86 by default because it is measurably smaller for the same sources --
REM 90 KB against 112 -- and a text editor has no use for a 64-bit address
REM space.  Pass x64 if you want it; everything works either way.

setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x86

set ROOT=%~dp0

REM NOTE_OUT, NOTE_CFLAGS and NOTE_LDFLAGS let a measurement build go to its
REM own directory with extra switches, so comparing configurations never
REM disturbs the real build or races another one over the same note.exe.
if defined NOTE_OUT (set "OUT=%NOTE_OUT%") else (set "OUT=%ROOT%build")

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

if not exist "%OUT%" mkdir "%OUT%"

REM /Gs disables stack probes so no __chkstk is needed without a CRT.
cl /nologo /c /O1 /Os /GS- /Gs1000000 /Gy /W4 /wd4100 /wd4127 /wd4201 ^
   /utf-8 /D_UNICODE /DUNICODE /GL %NOTE_CFLAGS% ^
   /Fo"%OUT%\\" ^
   "%ROOT%src\core\note_core.c" ^
   "%ROOT%src\core\note_conf.c" ^
   "%ROOT%src\core\note_syntax.c" ^
   "%ROOT%src\core\note_regex.c" ^
   "%ROOT%src\core\note_theme.c" ^
   "%ROOT%src\core\note_palette.c" ^
   "%ROOT%src\platform\win32\win32_edit.c" ^
   "%ROOT%src\platform\win32\win32_chrome.c" ^
   "%ROOT%src\platform\win32\win32_menu.c" ^
   "%ROOT%src\platform\win32\win32_dialogs.c" ^
   "%ROOT%src\platform\win32\win32_host.c" ^
   "%ROOT%src\platform\win32\win32_palette.c"
if errorlevel 1 exit /b 1

REM win32_main.c holds the memset/memcpy the compiler still emits calls to in
REM a build with no CRT.  /GL refuses to let a program define those -- it
REM treats them as its own library helpers -- so this one file is compiled on
REM its own, without whatever extra flags a measurement build is trying.
cl /nologo /c /O1 /Os /GS- /Gs1000000 /Gy /W4 /wd4100 /wd4127 /wd4201 ^
   /utf-8 /D_UNICODE /DUNICODE ^
   /Fo"%OUT%\\" "%ROOT%src\platform\win32\win32_main.c"
if errorlevel 1 exit /b 1

REM Compress the curated packs for embedding.  Failure here is not fatal: the
REM executable simply falls back to the definitions compiled into the C, and
REM to whatever packs are found on disk.
if exist "%ROOT%assets\core.syntax.pack" (
  powershell -NoProfile -ExecutionPolicy Bypass ^
    -File "%ROOT%tools\compress_packs.ps1" "%OUT%" 2>nul
)

set "PACKDEF="
if exist "%OUT%\core.syntax.lz" if exist "%OUT%\core.themes.lz" set "PACKDEF=/d HAVE_PACKS"

REM The icon and version block.  Explorer reads these out of the file without
REM running it, so they have to be compiled in rather than loaded at startup.
set "RES="
if exist "%ROOT%src\note.rc" (
  rc /nologo %PACKDEF% /i "%ROOT%src" /fo "%OUT%\note.res" "%ROOT%src\note.rc"
  if errorlevel 1 exit /b 1
  set "RES=%OUT%\note.res"
)

link /nologo /SUBSYSTEM:WINDOWS /ENTRY:noteEntry /NODEFAULTLIB ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO /LTCG /MAP:"%OUT%\note.map" %NOTE_LDFLAGS% ^
     /OUT:"%OUT%\note.exe" ^
     "%OUT%\note_core.obj" "%OUT%\note_conf.obj" "%OUT%\note_syntax.obj" ^
     "%OUT%\note_regex.obj" ^
     "%OUT%\note_theme.obj" "%OUT%\note_palette.obj" ^
     "%OUT%\win32_main.obj" "%OUT%\win32_edit.obj" "%OUT%\win32_chrome.obj" ^
     "%OUT%\win32_menu.obj" "%OUT%\win32_dialogs.obj" "%OUT%\win32_host.obj" ^
     "%OUT%\win32_palette.obj" ^
     %RES% ^
     kernel32.lib user32.lib gdi32.lib comdlg32.lib shell32.lib ^
     uxtheme.lib dwmapi.lib advapi32.lib
if errorlevel 1 exit /b 1

REM note looks for definitions beside the executable, so the packs ship next
REM to it.  They are optional: without them the compiled-in languages and the
REM light/dark palettes still work.
if exist "%ROOT%assets\syntax.pack" copy /y "%ROOT%assets\syntax.pack" "%OUT%\" >nul
if exist "%ROOT%assets\themes.pack" copy /y "%ROOT%assets\themes.pack" "%OUT%\" >nul
if exist "%ROOT%assets\NOTICE.md"   copy /y "%ROOT%assets\NOTICE.md"   "%OUT%\" >nul

del "%OUT%\note_core.obj" "%OUT%\note_conf.obj" "%OUT%\note_syntax.obj" 2>nul
del "%OUT%\note_regex.obj" 2>nul
del "%OUT%\note_theme.obj" "%OUT%\note_palette.obj" 2>nul
del "%OUT%\win32_main.obj" "%OUT%\win32_edit.obj" "%OUT%\win32_chrome.obj" 2>nul
del "%OUT%\win32_menu.obj" "%OUT%\win32_dialogs.obj" "%OUT%\win32_host.obj" 2>nul
del "%OUT%\win32_palette.obj" 2>nul
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
REM A measurement build is not an installation.
if defined NOTE_OUT goto :done

set "BIN=%LOCALAPPDATA%\Microsoft\WindowsApps"
set "SHARE=%LOCALAPPDATA%\note"
if exist "%BIN%" (
  copy /y "%OUT%\note.exe" "%BIN%\note.exe" >nul
  if errorlevel 1 (
    echo   not installed: note.exe is running, close it and build again
  ) else (
    if not exist "%SHARE%" mkdir "%SHARE%"
    if exist "%ROOT%assets\syntax.pack" copy /y "%ROOT%assets\syntax.pack" "%SHARE%\" >nul
    if exist "%ROOT%assets\themes.pack" copy /y "%ROOT%assets\themes.pack" "%SHARE%\" >nul
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
