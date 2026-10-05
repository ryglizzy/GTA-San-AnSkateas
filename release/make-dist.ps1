# Assembles release\dist\GTA San Anskateas\ (what players download) from the
# builds, the mashup's converter and the licences. Build skate-ffi and
# sa-plugin first. Usage: powershell -File release\make-dist.ps1 [-Zip <path>]
# (-Zip also zips it there and checks the zip itself for game files).
param([string]$Zip)
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
Copy-Item -LiteralPath "$root\sa-plugin\build\skate-audio.exe" -Destination "$out\converter"

# vgmstream (decodes Skate 3's Xbox audio for skate-audio.exe): the pinned
# official 32-bit Windows release, fetched once into release\vgmstream.
$vgm = Join-Path $PSScriptRoot 'vgmstream'
if (-not (Test-Path -LiteralPath "$vgm\vgmstream-cli.exe")) {
    $zip = Join-Path $env:TEMP 'vgmstream-win-r2117.zip'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/vgmstream/vgmstream/releases/download/r2117/vgmstream-win.zip' -OutFile $zip
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne '4FA8F0F567A3636E45931B8462A04898FBCECA17290A1458B80063B9558B323B') {
        throw 'vgmstream-win.zip does not match the pinned r2117 release'
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $vgm -Force
    Remove-Item -LiteralPath "$vgm\in_vgmstream.dll", "$vgm\xmp-vgmstream.dll" -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path "$out\converter\vgmstream" | Out-Null
Copy-Item -Path "$vgm\*" -Destination "$out\converter\vgmstream" -Exclude 'in_vgmstream.dll', 'xmp-vgmstream.dll'
Set-Content -LiteralPath "$out\converter\vgmstream\THIRD-PARTY.txt" -Encoding ascii -Value @'
vgmstream r2117 (https://github.com/vgmstream/vgmstream), unmodified official
Windows build; its licence is in COPYING. The DLLs beside it are the libraries
that build uses, each under its own licence: FFmpeg (avcodec, avformat, avutil,
swresample; LGPL 2.1+, source at https://ffmpeg.org), mpg123 (LGPL 2.1,
https://mpg123.org), libvorbis (BSD, https://xiph.org), Speex and CELT (BSD,
https://xiph.org), libatrac9 (MIT, https://github.com/Thealexbarney/LibAtrac9)
and the G.719 reference decoder (ITU-T). vgmstream's build notes list where
each comes from: https://github.com/vgmstream/vgmstream/blob/master/doc/BUILD.md
'@
Copy-Item -LiteralPath "$root\mashup\LICENSE" -Destination "$out\licenses\mashup-LICENSE.txt"
Copy-Item -LiteralPath "$root\mashup\NOTICE" -Destination "$out\licenses\mashup-NOTICE.txt"
Copy-Item -LiteralPath "$root\plugin-sdk\LICENSE" -Destination "$out\licenses\plugin-sdk-LICENSE.txt"

# Nothing from either game may ship: Skate 3's files and anything made from
# them (data, audio, trick names, board/rig), GTA's files and collision dumps.
$gameExtensions = '.xex', '.abin', '.rx2', '.r2b', '.stategraph', '.glb', '.img', '.txd', '.dff', '.col', '.ipl', '.ide',
    '.wav', '.m4a', '.mp3', '.ogg', '.snr', '.sns', '.xma', '.xwb', '.big', '.mus', '.mpf', '.abk', '.bnk', '.ast', '.csi', '.tris', '.png', '.dds'
$gameNames = 'board.json', 'rig.json', 'skater-collections.json', 'trick-names.txt', 'gta_sa.exe', 'gta-sa.exe'
function Test-GameFile([string]$name) {
    return ([System.IO.Path]::GetExtension($name).ToLowerInvariant() -in $gameExtensions) -or ([System.IO.Path]::GetFileName($name) -in $gameNames)
}
$game = Get-ChildItem -LiteralPath $out -Recurse -File | Where-Object { Test-GameFile $_.Name }
if ($game) { throw "game files in the package: $($game.FullName -join ', ')" }
# Nor the builder's user name or folders (compilers keep source paths in
# asserts and panic messages): searched as ASCII and UTF-16, both alignments.
$private = @($env:USERNAME, $env:USERPROFILE, $root) | Where-Object { $_ -and $_.Length -ge 3 }
$leaks = foreach ($f in Get-ChildItem -LiteralPath $out -Recurse -File) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $texts = [System.Text.Encoding]::ASCII.GetString($bytes), [System.Text.Encoding]::Unicode.GetString($bytes)
    if ($bytes.Length -gt 1) { $texts += [System.Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1) }
    foreach ($p in $private) { if ($texts | Where-Object { $_.IndexOf($p, [StringComparison]::OrdinalIgnoreCase) -ge 0 }) { "$($f.Name) contains '$p'"; break } }
}
if ($leaks) { throw "the builder's name or folders are in the package: $($leaks -join '; ')" }
$size = (Get-ChildItem -LiteralPath $out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Package ready: {0} ({1:N1} MB)." -f $out, $size)
if ($Zip) {
    if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
    Compress-Archive -Path $out -DestinationPath $Zip
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip))
    try {
        $bad = $archive.Entries | Where-Object { $_.Name -and (Test-GameFile $_.Name) } | ForEach-Object { $_.FullName }
        $count = $archive.Entries.Count
    } finally { $archive.Dispose() }
    if ($bad) { Remove-Item -LiteralPath $Zip -Force; throw "game files in the zip: $($bad -join ', ')" }
    $hash = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash
    Write-Host ("Zip ready: {0} ({1} files, {2:N1} MB, no game files)`nsha256 {3}" -f $Zip, $count, ((Get-Item -LiteralPath $Zip).Length / 1MB), $hash)
}
