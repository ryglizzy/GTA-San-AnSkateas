# GTA San Anskateas setup: makes the Skate 3 data from the player's own copy of
# Skate 3 and installs the mod into their own GTA San Andreas. Nothing from
# either game ships with the mod. Run through Setup.cmd.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$here = $PSScriptRoot
$us10Size = 14383616 # gta_sa.exe 1.0 US (or a downgraded copy)

function Say($text) { Write-Host $text }
function Fail($text) {
    Write-Host ""
    Write-Host "Setup stopped: $text" -ForegroundColor Red
    [System.Windows.Forms.MessageBox]::Show($text, 'GTA San Anskateas setup', 'OK', 'Error') | Out-Null
    exit 1
}
function GameExe($folder) {
    foreach ($name in 'gta_sa.exe', 'gta-sa.exe') {
        $exe = Join-Path $folder $name
        if (Test-Path -LiteralPath $exe) { return Get-Item -LiteralPath $exe }
    }
    return $null
}

Say "GTA San Anskateas setup"
Say "======================="

# 1. GTA San Andreas: the Steam copy if it's there, otherwise ask.
$gta = $null
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
$guesses = @()
if ($steam) { $guesses += (Join-Path $steam 'steamapps\common\Grand Theft Auto San Andreas') }
$guesses += 'C:\Program Files (x86)\Rockstar Games\GTA San Andreas', 'C:\Program Files\Rockstar Games\GTA San Andreas'
foreach ($g in $guesses) { if ((Test-Path -LiteralPath $g) -and (GameExe $g)) { $gta = $g; break } }
if ($gta) {
    $answer = [System.Windows.Forms.MessageBox]::Show("Install into this GTA San Andreas folder?`n`n$gta", 'GTA San Anskateas setup', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { $gta = $null }
}
if (-not $gta) {
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Select your GTA San Andreas folder (the one with gta_sa.exe)'
    if ($dialog.ShowDialog() -ne 'OK') { Fail 'no GTA San Andreas folder was selected.' }
    $gta = $dialog.SelectedPath
}
$exe = GameExe $gta
if (-not $exe) { Fail "there is no gta_sa.exe in $gta." }
Say "GTA San Andreas: $gta"
if ($exe.Length -ne $us10Size) {
    Write-Host "  Warning: $($exe.Name) is not version 1.0 US. The mod only works with 1.0 US;" -ForegroundColor Yellow
    Write-Host "  downgrade the game first (for example with gta-sa-open-downgrader)." -ForegroundColor Yellow
}
if (-not ((Test-Path -LiteralPath (Join-Path $gta 'vorbisHooked.dll')) -or (Test-Path -LiteralPath (Join-Path $gta 'dinput8.dll')))) {
    Write-Host "  Warning: no ASI loader found. Install Silent's ASI Loader so the game loads .asi mods." -ForegroundColor Yellow
}

# 2. Skate 3: the player's own extracted Xbox 360 copy.
$data = Join-Path $gta 'SanAnskateas\skate-data'
$redo = $true
if (Test-Path -LiteralPath (Join-Path $data 'assets')) {
    $answer = [System.Windows.Forms.MessageBox]::Show("Skate 3 data is already installed. Make it again from your Skate 3 files?", 'GTA San Anskateas setup', 'YesNo', 'Question')
    $redo = $answer -eq 'Yes'
}
if ($redo) {
    $pick = New-Object System.Windows.Forms.OpenFileDialog
    $pick.Title = 'Select default.xex from your extracted Skate 3 (Xbox 360) folder'
    $pick.Filter = 'Skate 3 executable (default.xex)|default.xex|All files (*.*)|*.*'
    if ($pick.ShowDialog() -ne 'OK') { Fail 'no default.xex was selected.' }
    $xex = $pick.FileName
    if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $xex) 'data'))) {
        Fail "there is no 'data' folder next to $xex. Select default.xex inside the whole extracted Skate 3 folder."
    }
    $converter = Join-Path $here 'converter\iw4l-skate-convert.exe'
    if (-not (Test-Path -LiteralPath $converter)) { Fail "the converter is missing ($converter)." }
    Say "Converting Skate 3 from $xex ..."
    # Convert in a short folder (some converted paths are long; deep folders
    # hit Windows' 260-character limit), then copy in with robocopy.
    $work = Join-Path $env:TEMP 'SanAnskateas-convert'
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    & $converter --xex $xex --out $work
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $work 'assets\private\skater.glb'))) {
        Fail "the Skate 3 conversion failed (see the messages above). Check that this is Skate 3 for Xbox 360."
    }
    robocopy $work $data /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "could not copy the Skate 3 data into $data." }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    Say "Skate 3 data made in $data"
}

# 3. The mod itself.
$mod = Join-Path $here 'mod'
New-Item -ItemType Directory -Force -Path (Join-Path $gta 'SanAnskateas') | Out-Null
Copy-Item -LiteralPath (Join-Path $mod 'SanAnskateas.asi') -Destination $gta -Force
Copy-Item -LiteralPath (Join-Path $mod 'SanAnskateas\skate_ffi.dll') -Destination (Join-Path $gta 'SanAnskateas') -Force
$ini = Join-Path $gta 'SanAnskateas.ini'
if (-not (Test-Path -LiteralPath $ini)) { Copy-Item -LiteralPath (Join-Path $mod 'SanAnskateas.ini') -Destination $gta }
Say "Mod installed."
Say ""
Say "Done. Start GTA San Andreas, wait for 'Skate 3 is ready', then press J (or L3 + R3) to skate."
Say "Skating needs an Xbox (XInput) controller; DS4Windows works for PlayStation pads."
[System.Windows.Forms.MessageBox]::Show("Installed into`n$gta`n`nIn game, press J (or L3 + R3) to skate.", 'GTA San Anskateas setup', 'OK', 'Information') | Out-Null
