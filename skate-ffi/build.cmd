@echo off
rem Builds the 32-bit skate_ffi.dll, then compiles and runs the C smoke test.
rem Usage: build.cmd [path to your skate-data folder]
setlocal
cd /d "%~dp0"
rem Source paths end up in the DLL's panic messages: keep the builder's user
rem folder out of it (the most specific prefix comes last and wins).
for %%i in ("%~dp0..") do set "ROOT=%%~fi"
"%USERPROFILE%\.cargo\bin\cargo.exe" build --release --config "target.i686-pc-windows-msvc.rustflags=['--remap-path-prefix=%USERPROFILE%=~', '--remap-path-prefix=%ROOT%=src']" || exit /b 1
set OUT=target\i686-pc-windows-msvc\release
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find VC\Auxiliary\Build\vcvarsall.bat`) do set "VCVARS=%%i"
rem vcvarsall looks for vswhere on PATH.
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VCVARS%" x86 >nul || exit /b 1
cl /nologo /W3 /Fe:%OUT%\smoke.exe /Fo:%OUT%\ tests-c\smoke.c %OUT%\skate_ffi.dll.lib /link /LARGEADDRESSAWARE || exit /b 1
cl /nologo /W3 /Fe:%OUT%\push.exe /Fo:%OUT%\ tests-c\push.c %OUT%\skate_ffi.dll.lib /link /LARGEADDRESSAWARE || exit /b 1
cl /nologo /W3 /Fe:%OUT%\knock.exe /Fo:%OUT%\ tests-c\knock.c %OUT%\skate_ffi.dll.lib /link /LARGEADDRESSAWARE || exit /b 1
%OUT%\smoke.exe %1 || exit /b 1
rem build.cmd <skate-data\assets> push   also measures push speeds per difficulty
rem build.cmd <skate-data\assets> knock  also traces a car-hit roll-over step by step
if /i "%~2"=="push" %OUT%\push.exe %1
if /i "%~2"=="knock" %OUT%\knock.exe %1
