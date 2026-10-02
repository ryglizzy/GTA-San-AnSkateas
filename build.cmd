@echo off
rem Builds the whole mod: plugin-sdk's San Andreas library (the first time
rem only), skate_ffi.dll and SanAnskateas.asi. See "Building from source" in
rem README.md for what to install first.
setlocal
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%"

rem plugin-sdk (https://github.com/DK22Pac/plugin-sdk) is downloaded the first
rem time, at the exact version this mod was built and tested with.
set "SDK_COMMIT=15f15b60bbf74c106e1b496ff92c98764abf4605"
if not exist "%ROOT%\plugin-sdk\plugin_sa" (
    echo === Downloading plugin-sdk ^(first time only^)
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue';" ^
        "[Net.ServicePointManager]::SecurityProtocol = 'Tls12';" ^
        "$zip = Join-Path $env:ROOT 'plugin-sdk.zip'; $tmp = Join-Path $env:ROOT 'plugin-sdk.unzip';" ^
        "Invoke-WebRequest ('https://github.com/DK22Pac/plugin-sdk/archive/' + $env:SDK_COMMIT + '.zip') -OutFile $zip -UseBasicParsing;" ^
        "if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }; Expand-Archive $zip $tmp;" ^
        "Move-Item (Join-Path $tmp ('plugin-sdk-' + $env:SDK_COMMIT)) (Join-Path $env:ROOT 'plugin-sdk');" ^
        "Remove-Item $zip, $tmp -Recurse -Force"
    if not exist "%ROOT%\plugin-sdk\plugin_sa" (
        echo Could not download plugin-sdk. Check your internet connection, or download
        echo https://github.com/DK22Pac/plugin-sdk/archive/%SDK_COMMIT%.zip yourself
        echo and unzip it here as a folder named plugin-sdk.
        goto :failed
    )
)
if not exist "%USERPROFILE%\.cargo\bin\cargo.exe" (
    echo Rust is missing. Install it from https://rustup.rs, then run this again.
    goto :failed
)
set "VCVARS="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find VC\Auxiliary\Build\vcvarsall.bat 2^>nul`) do set "VCVARS=%%i"
if not defined VCVARS (
    echo Visual Studio Build Tools with "Desktop development with C++" are missing.
    goto :failed
)

echo === Rust's 32-bit Windows target
"%USERPROFILE%\.cargo\bin\rustup.exe" target add i686-pc-windows-msvc || goto :failed

if not exist "%ROOT%\plugin-sdk\output\lib\Plugin.lib" (
    echo === plugin-sdk ^(first time only, takes a while^)
    set "PLUGIN_SDK_DIR=%ROOT%\plugin-sdk"
    "%ROOT%\plugin-sdk\tools\premake\premake5.exe" vs2022 --file="%ROOT%\plugin-sdk\tools\premake\premake5.lua" || goto :failed
    set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
    call "%VCVARS%" x86 >nul || goto :failed
    msbuild "%ROOT%\plugin-sdk\plugin.sln" /p:Configuration=Release "/p:Platform=Mixed Platforms" /t:Plugin_SA /m /v:minimal /nologo || goto :failed
)

echo === skate_ffi.dll
call "%ROOT%\skate-ffi\build.cmd" || goto :failed
cd /d "%ROOT%"
echo === SanAnskateas.asi
call "%ROOT%\sa-plugin\build.cmd" || goto :failed
cd /d "%ROOT%"

echo.
echo Done:
echo   sa-plugin\build\SanAnskateas.asi
echo   skate-ffi\target\i686-pc-windows-msvc\release\skate_ffi.dll
echo To install into your game, run: sa-plugin\build.cmd install
pause
exit /b 0

:failed
echo.
echo Build stopped (see the messages above).
pause
exit /b 1
