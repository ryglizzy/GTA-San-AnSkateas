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
cl /nologo /W3 /Fe:%OUT%\marker.exe /Fo:%OUT%\ tests-c\marker.c %OUT%\skate_ffi.dll.lib /link /LARGEADDRESSAWARE || exit /b 1
cl /nologo /W3 /Fe:%OUT%\options.exe /Fo:%OUT%\ tests-c\options.c %OUT%\skate_ffi.dll.lib /link /LARGEADDRESSAWARE || exit /b 1
cl /nologo /W3 /Fe:%OUT%\bail.exe /Fo:%OUT%\ tests-c\bail.c %OUT%\skate_ffi.dll.lib /link /LARGEADDRESSAWARE || exit /b 1
cl /nologo /W3 /Fe:%OUT%\landing.exe /Fo:%OUT%\ tests-c\landing.c %OUT%\skate_ffi.dll.lib /link /LARGEADDRESSAWARE || exit /b 1
cl /nologo /W3 /Fe:%OUT%\grind.exe /Fo:%OUT%\ tests-c\grind.c %OUT%\skate_ffi.dll.lib /link /LARGEADDRESSAWARE || exit /b 1
rem The offline checks need converted Skate data: build.cmd <skate-data\assets>
if not "%~1"=="" %OUT%\smoke.exe %1 || exit /b 1
rem build.cmd <skate-data\assets> push   also measures push speeds per difficulty
rem build.cmd <skate-data\assets> knock  also traces a car-hit roll-over step by step
if /i "%~2"=="push" %OUT%\push.exe %1
if /i "%~2"=="knock" %OUT%\knock.exe %1
rem build.cmd <skate-data\assets> marker  checks a marker set on foot returns facing the same way
if /i "%~2"=="marker" %OUT%\marker.exe %1
rem build.cmd <skate-data\assets> options  checks difficulty, camera and trucks change live
if /i "%~2"=="options" %OUT%\options.exe %1
rem grind.exe (run it yourself) grinds in a dumped world: see tests-c\grind.c
