# GTA San Anskateas setup: makes the Skate 3 data from the player's own copy of
# Skate 3 and installs the mod into their own GTA San Andreas. Nothing from
# either game ships with the mod. Run through Setup.cmd. Run over an older
# install, it updates it: settings are kept (new ones added), the Skate data
# stays, and the sounds are made again when this version has new ones.
# -GtaFolder <folder> -XexFile <default.xex> -Unattended answers every question itself
# (keeps existing Skate data and sounds unless they're out of date), for tests.
param([string]$GtaFolder, [string]$XexFile, [switch]$Unattended)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$here = $PSScriptRoot
$us10Size = 14383616 # gta_sa.exe 1.0 US (or a downgraded copy)
$modVersion = '1.1'
$audioVersion = '1.1' # raise when skate-audio.exe makes new sounds: older installs make them again

function Say($text) { Write-Host $text }
function Fail($text) {
    Write-Host ""
    Write-Host "Setup stopped: $text" -ForegroundColor Red
    if (-not $Unattended) { [System.Windows.Forms.MessageBox]::Show($text, 'GTA San Anskateas setup', 'OK', 'Error') | Out-Null }
    exit 1
}
# A question box; unattended, the given answer.
function Ask($text, $buttons, $icon, $unattendedAnswer) {
    if ($Unattended) { return $unattendedAnswer }
    return [string][System.Windows.Forms.MessageBox]::Show($text, 'GTA San Anskateas setup', $buttons, $icon)
}

# The player's SanAnskateas.ini, kept as it is, with every setting the
# template has and it lacks added (with the template's comments above it) at
# the end of its [Skate] section. Returns the keys added.
function Merge-Ini([string]$template, [string]$ini) {
    $ansi = [System.Text.Encoding]::Default # GetPrivateProfileString reads ANSI
    $lines = [System.Collections.Generic.List[string]]::new([string[]][System.IO.File]::ReadAllLines($ini, $ansi))
    $have = @{}
    foreach ($l in $lines) { if ($l -match '^\s*([A-Za-z0-9_]+)\s*=') { $have[$Matches[1].ToLowerInvariant()] = $true } }
    $added = @(); $block = @(); $comments = @()
    foreach ($l in [System.IO.File]::ReadAllLines($template, $ansi)) {
        if ($l -match '^\s*;') { $comments += $l; continue }
        if ($l -match '^\s*([A-Za-z0-9_]+)\s*=' -and -not $have.ContainsKey($Matches[1].ToLowerInvariant())) {
            $block += $comments; $block += $l; $added += $Matches[1]
        }
        $comments = @()
    }
    if ($added.Count -eq 0) { return @() }
    # After the [Skate] section's last line (before the next section, if any).
    $at = $lines.Count
    $inSkate = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[(.+)\]') {
            if ($inSkate) { $at = $i; break }
            $inSkate = $Matches[1] -eq 'Skate'
        }
    }
    while ($at -gt 0 -and $lines[$at - 1].Trim() -eq '') { $at-- }
    $lines.InsertRange($at, [string[]](@('; Added by the ' + $modVersion + ' update:') + $block))
    [System.IO.File]::WriteAllLines($ini, $lines, $ansi)
    return $added
}

# One setting's value in the ini (or $null).
function Get-IniValue([string]$ini, [string]$key) {
    foreach ($l in [System.IO.File]::ReadAllLines($ini, [System.Text.Encoding]::Default)) {
        if ($l -match "^\s*$key\s*=\s*(.*?)\s*$") { return $Matches[1] }
    }
    return $null
}

# Sets one existing setting in the ini.
function Set-IniValue([string]$ini, [string]$key, [string]$value) {
    $ansi = [System.Text.Encoding]::Default
    $lines = [System.IO.File]::ReadAllLines($ini, $ansi)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match "^\s*$key\s*=") { $lines[$i] = "$key=$value" }
    }
    [System.IO.File]::WriteAllLines($ini, $lines, $ansi)
}
function GameExe($folder) {
    foreach ($name in 'gta_sa.exe', 'gta-sa.exe') {
        $exe = Join-Path $folder $name
        if (Test-Path -LiteralPath $exe) { return Get-Item -LiteralPath $exe }
    }
    return $null
}

Say "GTA San Anskateas $modVersion setup"
Say "==========================="

# 1. GTA San Andreas: the Steam copy if it's there, otherwise ask.
$gta = $null
if ($GtaFolder) {
    $gta = $GtaFolder
} else {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    $guesses = @()
    if ($steam) { $guesses += (Join-Path $steam 'steamapps\common\Grand Theft Auto San Andreas') }
    $guesses += 'C:\Program Files (x86)\Rockstar Games\GTA San Andreas', 'C:\Program Files\Rockstar Games\GTA San Andreas'
    foreach ($g in $guesses) { if ((Test-Path -LiteralPath $g) -and (GameExe $g)) { $gta = $g; break } }
    if ($gta) {
        $answer = Ask "Install into this GTA San Andreas folder?`n`n$gta" 'YesNo' 'Question' 'Yes'
        if ($answer -ne 'Yes') { $gta = $null }
    }
}
if (-not $gta) {
    if ($Unattended) { Fail 'no GTA San Andreas folder was given (-GtaFolder).' }
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Select your GTA San Andreas folder (the one with gta_sa.exe)'
    if ($dialog.ShowDialog() -ne 'OK') { Fail 'no GTA San Andreas folder was selected.' }
    $gta = $dialog.SelectedPath
}
$exe = GameExe $gta
if (-not $exe) { Fail "there is no gta_sa.exe in $gta." }
Say "GTA San Andreas: $gta"
$downgrader = 'https://github.com/xxanqw/gtasa-open-downgrader/releases'
if ($exe.Length -ne $us10Size) {
    $answer = Ask ("This GTA San Andreas doesn't look like version 1.0 US, which the mod needs.`n`n" +
        "Run GTA SA Open Downgrader on it first (it also installs the ASI Loader).`n`n" +
        "Yes: open the downgrader's download page and stop here.`nNo: carry on anyway.") 'YesNo' 'Warning' 'No'
    if ($answer -eq 'Yes') { Start-Process $downgrader; exit 1 }
}
$loaders = 'vorbisHooked.dll', 'dinput8.dll', 'version.dll', 'winmm.dll'
if (-not ($loaders | Where-Object { Test-Path -LiteralPath (Join-Path $gta $_) })) {
    Write-Host "  Warning: no ASI Loader found, so the game won't load the mod." -ForegroundColor Yellow
    Write-Host "  GTA SA Open Downgrader can install it ($downgrader)." -ForegroundColor Yellow
}
# What's installed already: the version this setup last installed (none
# before 1.1, which first wrote it).
$modDir = Join-Path $gta 'SanAnskateas'
$versionFile = Join-Path $modDir 'version.txt'
$ini = Join-Path $gta 'SanAnskateas.ini'
$previous = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -Raw).Trim() } else { $null }
$updating = (Test-Path -LiteralPath (Join-Path $gta 'SanAnskateas.asi')) -or (Test-Path -LiteralPath $ini)
if ($updating) { Say ("Updating the installed version ({0}) to {1}." -f $(if ($previous) { $previous } else { '1.0' }), $modVersion) }

# 2. Skate 3: the player's own extracted Xbox 360 copy.
function PickXex {
    if ($XexFile) {
        $picked = $XexFile
    } elseif ($Unattended) {
        Fail 'no default.xex was given (-XexFile).'
    } else {
        $pick = New-Object System.Windows.Forms.OpenFileDialog
        $pick.Title = 'Select default.xex from your extracted Skate 3 (Xbox 360) folder'
        $pick.Filter = 'Skate 3 executable (default.xex)|default.xex|All files (*.*)|*.*'
        if ($pick.ShowDialog() -ne 'OK') { Fail 'no default.xex was selected.' }
        $picked = $pick.FileName
    }
    if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $picked) 'data'))) {
        Fail "there is no 'data' folder next to $picked. Select default.xex inside the whole extracted Skate 3 folder."
    }
    return $picked
}
$data = Join-Path $modDir 'skate-data'
$xex = $null
$redo = $true
if (Test-Path -LiteralPath (Join-Path $data 'assets')) {
    $answer = Ask "Skate 3 data is already installed. Make it again from your Skate 3 files?" 'YesNo' 'Question' 'No'
    $redo = $answer -eq 'Yes'
}
if ($redo) {
    $xex = PickXex
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

# 2b. Skate 3's audio: the skateboard sounds, SK8-FM's songs (Skate 3's
# soundtrack) and the trick names, decoded from the player's own copy by
# vgmstream. Songs are made into AAC with Windows' own encoder. Its
# version.txt says which setup made them: older sounds are made again.
$audio = Join-Path $modDir 'skate-audio'
$audioMarker = Join-Path $audio 'version.txt'
$audioCurrent = (Test-Path -LiteralPath $audioMarker) -and ((Get-Content -LiteralPath $audioMarker -Raw).Trim() -eq $audioVersion)
$makeAudio = $true
if ((-not $redo) -and $audioCurrent) {
    $answer = Ask "Skate 3 sounds and SK8-FM are already installed. Make them again from your Skate 3 files?" 'YesNo' 'Question' 'No'
    $makeAudio = $answer -eq 'Yes'
}
if ($makeAudio) {
    if (-not $xex) {
        $why = if (Test-Path -LiteralPath $audio) { "This version has new skateboard sounds." } else { "The skateboard sounds and SK8-FM's songs are made from it." }
        if (-not $Unattended) {
            [System.Windows.Forms.MessageBox]::Show("Next, select your Skate 3 default.xex again. $why", 'GTA San Anskateas setup', 'OK', 'Information') | Out-Null
        }
        $xex = PickXex
    }
    $audioTool = Join-Path $here 'converter\skate-audio.exe'
    $vgmstream = Join-Path $here 'converter\vgmstream\vgmstream-cli.exe'
    if (-not (Test-Path -LiteralPath $audioTool) -or -not (Test-Path -LiteralPath $vgmstream)) { Fail "the audio converter is missing ($audioTool)." }
    Say "Making Skate 3's sounds and SK8-FM's songs (a few minutes) ..."
    if (Test-Path -LiteralPath $audio) { Remove-Item -LiteralPath $audio -Recurse -Force }
    & $audioTool --skate (Split-Path $xex) --out $audio --vgmstream $vgmstream
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  Warning: Skate 3's audio could not be made (see the messages above); skating stays silent." -ForegroundColor Yellow
    } else {
        Set-Content -LiteralPath $audioMarker -Value $audioVersion -Encoding ascii
        Say "Skate 3 audio made in $audio"
    }
}

# 3. The mod itself. A player's settings are kept; new ones are added.
$mod = Join-Path $here 'mod'
New-Item -ItemType Directory -Force -Path $modDir | Out-Null
Copy-Item -LiteralPath (Join-Path $mod 'SanAnskateas.asi') -Destination $gta -Force
Copy-Item -LiteralPath (Join-Path $mod 'SanAnskateas\skate_ffi.dll') -Destination $modDir -Force
$notes = @()
if (-not (Test-Path -LiteralPath $ini)) {
    Copy-Item -LiteralPath (Join-Path $mod 'SanAnskateas.ini') -Destination $gta
} else {
    $added = Merge-Ini (Join-Path $mod 'SanAnskateas.ini') $ini
    if ($added.Count) { Say ("Settings kept; new ones added to SanAnskateas.ini: {0}" -f ($added -join ', ')) }
    # 1.1 made Easy the default (and put it in the pause menu): an install
    # from before 1.1 still on 1.0's default, Normal, moves to Easy once.
    if (-not $previous -and (Get-IniValue $ini 'Difficulty') -eq 'normal') {
        Set-IniValue $ini 'Difficulty' 'easy'
        $notes += "Difficulty is now Easy, the new default. Change it any time in the pause menu: Options > SAN ANSKATEAS."
    }
}
Set-Content -LiteralPath $versionFile -Value $modVersion -Encoding ascii
Say "Mod installed."
foreach ($n in $notes) { Say $n }
Say ""
Say "Done. Start GTA San Andreas, wait for 'Skate 3 is ready', then press J (or L3 + R3) to skate."
Say "Skating needs an Xbox (XInput) controller; DS4Windows works for PlayStation pads."
if (-not $Unattended) {
    $extra = if ($notes) { "`n`n" + ($notes -join "`n") } else { '' }
    [System.Windows.Forms.MessageBox]::Show("Installed into`n$gta`n`nIn game, press J (or L3 + R3) to skate.$extra", 'GTA San Anskateas setup', 'OK', 'Information') | Out-Null
}
