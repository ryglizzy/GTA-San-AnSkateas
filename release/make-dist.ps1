# Assembles release\dist\GTA San Anskateas\ (what players download) from the
# builds, the mashup's converter and the licences. Build skate-ffi and
# sa-plugin first. Usage: powershell -File release\make-dist.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$out = Join-Path $PSScriptRoot 'dist\GTA San Anskateas'
$mashupRelease = Join-Path $env:USERPROFILE 'Desktop\2010-Rust-Rewrite-Mashup'
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$out\mod\SanAnskateas", "$out\converter", "$out\licenses" | Out-Null

Copy-Item -LiteralPath "$PSScriptRoot\Setup.cmd", "$PSScriptRoot\Setup.ps1", "$PSScriptRoot\README.txt" -Destination $out
Copy-Item -LiteralPath "$root\sa-plugin\build\SanAnskateas.asi", "$root\sa-plugin\SanAnskateas.ini" -Destination "$out\mod"
Copy-Item -LiteralPath "$root\skate-ffi\target\i686-pc-windows-msvc\release\skate_ffi.dll" -Destination "$out\mod\SanAnskateas"
Copy-Item -LiteralPath "$mashupRelease\skate\iw4l-skate-convert.exe" -Destination "$out\converter"
Copy-Item -LiteralPath "$mashupRelease\skate\licenses" -Destination "$out\converter" -Recurse
Copy-Item -LiteralPath "$root\mashup\LICENSE" -Destination "$out\licenses\mashup-LICENSE.txt"
Copy-Item -LiteralPath "$root\mashup\NOTICE" -Destination "$out\licenses\mashup-NOTICE.txt"
Copy-Item -LiteralPath "$root\plugin-sdk\LICENSE" -Destination "$out\licenses\plugin-sdk-LICENSE.txt"

# Nothing from either game may ship.
$game = Get-ChildItem -LiteralPath $out -Recurse -File |
    Where-Object { $_.Extension -in '.xex', '.abin', '.rx2', '.r2b', '.stategraph', '.glb', '.img', '.txd', '.dff' -or $_.Name -in 'board.json', 'rig.json', 'skater-collections.json' }
if ($game) { throw "game files in the package: $($game.FullName -join ', ')" }
$size = (Get-ChildItem -LiteralPath $out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Package ready: {0} ({1:N1} MB). Zip this folder to publish." -f $out, $size)
