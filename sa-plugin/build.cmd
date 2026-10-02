@echo off
rem Builds SanAnskateas.asi (32-bit) against plugin-sdk's San Andreas library.
rem   build.cmd           build only
rem   build.cmd install   build, then copy the mod into the game folder
setlocal
cd /d "%~dp0"
set "SDK=%~dp0..\plugin-sdk"
set "FFI=%~dp0..\skate-ffi\target\i686-pc-windows-msvc\release\skate_ffi.dll"
if not defined GAME set "GAME=C:\Steam\steamapps\common\Grand Theft Auto San Andreas"
if not defined SKATE_DATA set "SKATE_DATA=%USERPROFILE%\Desktop\2010-Rust-Rewrite-Mashup\skate-data"

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find VC\Auxiliary\Build\vcvarsall.bat`) do set "VCVARS=%%i"
rem vcvarsall looks for vswhere on PATH.
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VCVARS%" x86 >nul || exit /b 1

if not exist build mkdir build
rem Unit tests first: a failing test stops the build.
cl /nologo /std:c++latest /EHsc /W3 /Fobuild\ /Fe:build\stick_combo_test.exe tests\stick_combo_test.cpp || exit /b 1
build\stick_combo_test.exe || exit /b 1
cl /nologo /std:c++latest /EHsc /W3 /Fobuild\ /Fe:build\test_script_test.exe tests\test_script_test.cpp || exit /b 1
build\test_script_test.exe || exit /b 1
rem Same settings as plugin-sdk's own SA projects (static CRT, C++latest, LTCG).
cl /nologo /c /std:c++latest /EHsc /MT /O2 /Oi /GL /Gy /GF /W3 /sdl- ^
   /D_CRT_NON_CONFORMING_SWPRINTFS /D_CRT_SECURE_NO_WARNINGS /DGTASA /DPLUGIN_SGV_10US /DRW /D_MBCS ^
   /I"%SDK%\plugin_sa" /I"%SDK%\plugin_sa\game_sa" /I"%SDK%\plugin_sa\game_sa\enums" ^
   /I"%SDK%\plugin_sa\game_sa\rw" /I"%SDK%\shared" /I"%SDK%\shared\game" ^
   /Fobuild\ src\main.cpp || exit /b 1
link /nologo /DLL /LTCG /OPT:REF /OPT:ICF /OUT:build\SanAnskateas.asi build\main.obj ^
   /LIBPATH:"%SDK%\output\lib" Plugin.lib kernel32.lib user32.lib shell32.lib gdiplus.lib xinput.lib || exit /b 1
echo Built build\SanAnskateas.asi

if /i not "%~1"=="install" exit /b 0
if not exist "%GAME%\gta_sa.exe" (echo Game not found at %GAME% & exit /b 1)
if not exist "%FFI%" (echo Build skate-ffi first: %FFI% & exit /b 1)
if not exist "%GAME%\SanAnskateas" mkdir "%GAME%\SanAnskateas"
copy /y build\SanAnskateas.asi "%GAME%\" >nul || exit /b 1
copy /y "%FFI%" "%GAME%\SanAnskateas\" >nul || exit /b 1
if not exist "%GAME%\SanAnskateas.ini" copy SanAnskateas.ini "%GAME%\" >nul
rem Skate data made by the converter (release\Setup.cmd makes it in the game
rem folder itself). robocopy: exit codes below 8 mean success.
if exist "%SKATE_DATA%" (
    robocopy "%SKATE_DATA%" "%GAME%\SanAnskateas\skate-data" /E /NJH /NJS /NFL /NDL /NP >nul
    if errorlevel 8 (echo Copying skate-data failed & exit /b 1)
)
echo Installed into %GAME%
exit /b 0
