<#
.SYNOPSIS
  Installs scummvm-ai-upscale for The Curse of Monkey Island, and The Dig, Indiana Jones and the Fate of Atlantis,
  Indiana Jones and the Last Crusade, Loom and Sam & Max Hit the Road (experimental). You need your own copy of the game.

.DESCRIPTION
  - Copies this package to -InstallDir (default: %LOCALAPPDATA%\ScummVM-AI-Upscale).
  - Finds your Steam or GOG installation of The Curse of Monkey Island, or uses -GamePath.
    The game files are only read, never copied or changed.
  - Also finds The Dig, Fate of Atlantis, Last Crusade, Loom (the VGA CD talkie version) and Sam & Max (CD version)
    (Steam or GOG, or -DigPath, -AtlantisPath, -Indy3Path, -LoomPath, -SamnmaxPath) and asks whether to set each up too
    (experimental); -Games chooses without asking: comi, dig, atlantis, indy3, loom, samnmax or a list
    (comi,dig,atlantis). Each gets its own target ([dig], [atlantis], [indy3], [loom], [samnmax]) in the same
    scummvm.ini and its own shortcuts. Side art, widened cutscenes and preparing the HD backgrounds are for Monkey Island only.
  - Writes scummvm.ini from the preset you choose (-Preset, asked on a new install):
      original      no AI: the 1997 picture in 4:3, with the non-AI extras (also accepted as "classic")
      enhanced      AI upscaling, HD characters and backgrounds, 16:9 with an ambient glow (the default)
      experimental  enhanced, plus side art and widened cutscenes when you have them, and smooth scrolling
    Every individual setting can still be changed in game afterwards. An existing scummvm.ini is kept (only the game
    path is updated) unless you pass -Preset.
  - Creates desktop and Start menu shortcuts.
  - Installs side art (sides\) or widened cutscenes (video-sides\) that you generated yourself from your own
    copy, if those folders are next to install.cmd (nothing AI-generated is distributed, see the README).
  - Optionally (NVIDIA graphics cards) downloads the TensorRT upscaler: ONNX Runtime GPU and NVIDIA's TensorRT,
    CUDA and cuDNN libraries from their official Python package pages (about 1.5 GB download, 2 GB installed,
    in the tensorrt folder). Another choice for Ctrl+F9 in game; the other upscalers do not need it.
  - Removes Windows' "downloaded from the internet" mark from the program files it installed (Windows asks once,
    when install.cmd is started; like a setup program, the installed game then starts without a second prompt).
  Run it again any time to update; settings and saves are preserved.
  Double-clicking install.cmd opens the same choices in a window (install-gui.ps1, which runs this script);
  "install.cmd /console", or install.cmd / install.ps1 with any parameters, use this text version.

.EXAMPLE
  .\install.ps1
  .\install.ps1 -GamePath "D:\Games\The Curse of Monkey Island"
  .\install.ps1 -Games comi,dig -DigPath "D:\Games\The Dig"
  .\install.ps1 -Games atlantis,indy3
  .\install.ps1 -Games loom,samnmax -LoomPath "D:\Games\Loom" -SamnmaxPath "D:\Games\Sam and Max\SAMNMAX"
#>
[CmdletBinding()]
param(
	[string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'ScummVM-AI-Upscale'),
	[string]$GamePath,
	[string]$DigPath,                     # The Dig: the folder with The Dig.exe (Steam) or DIG.LA0 (GOG)
	[string]$AtlantisPath,                # Fate of Atlantis: the folder with its .exe (Steam) or ATLANTIS.000 (GOG)
	[string]$Indy3Path,                   # Last Crusade: the folder with its .exe (Steam) or 00.LFL (GOG)
	[string]$LoomPath,                    # Loom, VGA CD talkie: the folder with 000.LFL and DISK01.LEC (or Loom.exe on Steam)
	[string]$SamnmaxPath,                 # Sam & Max Hit the Road, CD version: the folder with SAMNMAX.000 and SAMNMAX.001
	[string]$Games = '',                  # comi, dig, atlantis, indy3, loom, samnmax or a list; default: Monkey Island, the others asked when found
	[switch]$NoShortcuts,
	[ValidateSet('ask', 'yes', 'no')][string]$SideArt = 'ask',
	[ValidateSet('ask', 'yes', 'no')][string]$Cutscenes = 'ask',
	[ValidateSet('ask', 'yes', 'no')][string]$Prewarm = 'ask',
	[ValidateSet('ask', 'yes', 'no')][string]$TensorRT = 'ask',
	[ValidateSet('ask', 'cloud', 'local', 'no')][string]$GenerateSideArt = 'ask',
	[ValidateSet('ask', 'original', 'classic', 'enhanced', 'experimental', 'keep')][string]$Preset = 'ask',
	[switch]$FindGame   # only print "game=<folder>" / "dig=<folder>" / "atlantis=" / "indy3=" / "loom=" / "samnmax=" for the found (or given) copies and stop (install-gui.ps1)
)
$ErrorActionPreference = 'Stop'
$pkg = $PSScriptRoot
function Say($m) { Write-Host "[remaster] $m" }

function Test-GameDir([string]$dir) {
	if (-not $dir) { return $null }
	foreach ($c in @($dir, (Join-Path $dir 'monkey3'), (Join-Path $dir 'ScummVM\monkey3'))) {
		if ((Test-Path (Join-Path $c 'COMI.LA0')) -and (Test-Path (Join-Path $c 'RESOURCE'))) { return (Resolve-Path $c).Path }
	}
	return $null
}

# The Dig: Steam has The Dig.exe (it holds the index) and the data in DIG\; GOG and CD copies have DIG.LA0 and DIG.LA1.
function Test-DigDir([string]$dir) {
	if (-not $dir) { return $null }
	foreach ($c in @($dir, (Split-Path $dir -Parent))) {   # the Steam folder, or its DIG folder
		if ($c -and (Test-Path (Join-Path $c 'The Dig.exe')) -and (Test-Path (Join-Path $c 'DIG\DIG.LA1'))) { return (Resolve-Path $c).Path }
	}
	foreach ($c in @($dir, (Join-Path $dir 'DIG'))) {
		if ((Test-Path (Join-Path $c 'DIG.LA0')) -and (Test-Path (Join-Path $c 'DIG.LA1'))) { return (Resolve-Path $c).Path }
	}
	return $null
}

# Indiana Jones and the Fate of Atlantis: Steam has its .exe (it holds the index) and the data in ATLANTIS\; GOG and CD
# copies have ATLANTIS.000 and ATLANTIS.001.
function Test-AtlantisDir([string]$dir) {
	if (-not $dir) { return $null }
	foreach ($c in @($dir, (Split-Path $dir -Parent))) {   # the Steam folder, or its ATLANTIS folder
		if ($c -and (Test-Path (Join-Path $c 'Indiana Jones and the Fate of Atlantis.exe')) -and (Test-Path (Join-Path $c 'ATLANTIS\ATLANTIS.001'))) { return (Resolve-Path $c).Path }
	}
	foreach ($c in @($dir, (Join-Path $dir 'ATLANTIS'))) {
		if ((Test-Path (Join-Path $c 'ATLANTIS.000')) -and (Test-Path (Join-Path $c 'ATLANTIS.001'))) { return (Resolve-Path $c).Path }
	}
	return $null
}

# Indiana Jones and the Last Crusade: Steam has its .exe (it holds the index, 00.LFL) and the rooms in INDY3\; GOG and
# floppy copies have 00.LFL, 01.LFL, ...
function Test-Indy3Dir([string]$dir) {
	if (-not $dir) { return $null }
	foreach ($c in @($dir, (Split-Path $dir -Parent))) {   # the Steam folder, or its INDY3 folder
		if ($c -and (Test-Path (Join-Path $c 'Indiana Jones and the Last Crusade.exe')) -and (Test-Path (Join-Path $c 'INDY3\01.LFL'))) { return (Resolve-Path $c).Path }
	}
	foreach ($c in @($dir, (Join-Path $dir 'INDY3'))) {
		if ((Test-Path (Join-Path $c '00.LFL')) -and (Test-Path (Join-Path $c '01.LFL'))) { return (Resolve-Path $c).Path }
	}
	return $null
}

# Loom: only the VGA CD talkie version is remastered (version 4: 000.LFL, DISK01.LEC, 901-904.LFL; speech and music are
# the CD audio track, e.g. track1.flac / .ogg / .mp3 next to them). Steam has Loom.exe (it holds the index) with the
# same data. The EGA, FM Towns, PC Engine and Mac versions (00.LFL, ...) are not offered. The data may sit in a subfolder.
function Test-LoomDir([string]$dir) {
	if (-not $dir -or -not (Test-Path $dir)) { return $null }
	$cands = @($dir) + @(Get-ChildItem $dir -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
	foreach ($c in $cands) {
		if ((Test-Path (Join-Path $c 'DISK01.LEC')) -and ((Test-Path (Join-Path $c '000.LFL')) -or (Test-Path (Join-Path $c 'Loom.exe')))) { return (Resolve-Path $c).Path }
	}
	return $null
}

# Sam & Max Hit the Road, CD version: SAMNMAX.000 and SAMNMAX.001, also in a SAMNMAX subfolder (the CD's layout).
function Test-SamnmaxDir([string]$dir) {
	if (-not $dir -or -not (Test-Path $dir)) { return $null }
	$cands = @($dir) + @(Get-ChildItem $dir -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
	foreach ($c in $cands) {
		if ((Test-Path (Join-Path $c 'SAMNMAX.000')) -and (Test-Path (Join-Path $c 'SAMNMAX.001'))) { return (Resolve-Path $c).Path }
	}
	return $null
}

# 730820 The Curse of Monkey Island, 6040 The Dig, 6010 Fate of Atlantis, 32310 Last Crusade, 32340 Loom
function Find-Steam([string]$AppId = '730820', [string]$Test = 'Test-GameDir') {
	$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
	if (-not $steam) { return $null }
	$vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
	$libs = @($steam)
	if (Test-Path $vdf) {
		$libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
	}
	foreach ($lib in ($libs | Select-Object -Unique)) {
		$acf = Join-Path $lib "steamapps\appmanifest_$AppId.acf"
		if (Test-Path $acf) {
			$dir = (Select-String -Path $acf -Pattern '"installdir"\s+"([^"]+)"').Matches[0].Groups[1].Value
			$found = & $Test (Join-Path $lib "steamapps\common\$dir")
			if ($found) { return $found }
		}
	}
	return $null
}

function Find-GOG([string]$Name = '*Curse of Monkey Island*', [string]$Test = 'Test-GameDir') {
	foreach ($root in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games') {
		if (-not (Test-Path $root)) { continue }
		foreach ($k in Get-ChildItem $root) {
			$p = Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue
			if ($p.gameName -like $Name) {
				$found = & $Test $p.path
				if ($found) { return $found }
			}
		}
	}
	return $null
}

# ── Game data ─────────────────────────────────────────────────────────────────────────────────────
# The other games (experimental): each gets its own target in the same scummvm.ini and its own shortcuts. SteamKeys:
# what ScummVM's own detection (--add) writes for the Steam version, whose index is in the game's .exe.
$ExtraGames = @(
	@{ Id = 'dig'; Title = 'The Dig'; Test = 'Test-DigDir'; AppId = '6040'; Gog = 'The Dig'; Path = $DigPath; Param = 'DigPath'
	   Data = 'The Dig.exe and a DIG folder, or DIG.LA0 and DIG.LA1'; Browse = 'it contains The Dig.exe and a DIG folder, or DIG.LA0'
	   Exe = 'The Dig.exe'; Subtitles = $true; SteamDrop = '^(platform|extra|guioptions)='
	   SteamKeys = @('platform=windows', 'extra=Steam', 'guioptions=sndNoMIDI gameOption2 gameOption4 gameOption5 gameOption9  plat_windows') },
	@{ Id = 'atlantis'; Title = 'Indiana Jones and the Fate of Atlantis'; Test = 'Test-AtlantisDir'; AppId = '6010'; Gog = '*Fate of Atlantis*'
	   Path = $AtlantisPath; Param = 'AtlantisPath'
	   Data = 'Indiana Jones and the Fate of Atlantis.exe and an ATLANTIS folder, or ATLANTIS.000 and ATLANTIS.001'
	   Browse = 'it contains Indiana Jones and the Fate of Atlantis.exe and an ATLANTIS folder, or ATLANTIS.000'
	   Exe = 'Indiana Jones and the Fate of Atlantis.exe'; Subtitles = $true; SteamDrop = '^(platform|language|extra|guioptions)='
	   SteamKeys = @('platform=windows', 'language=en', 'extra=Steam',
	                 'guioptions=ega gameOption2 gameOption4 gameOption9 midiPCSpk midiAdLib midiGM midiMt32 lang_English plat_windows') },
	@{ Id = 'indy3'; Title = 'Indiana Jones and the Last Crusade'; Test = 'Test-Indy3Dir'; AppId = '32310'; Gog = '*Last Crusade*'
	   Path = $Indy3Path; Param = 'Indy3Path'
	   Data = 'Indiana Jones and the Last Crusade.exe and an INDY3 folder, or 00.LFL and 01.LFL'
	   Browse = 'it contains Indiana Jones and the Last Crusade.exe and an INDY3 folder, or 00.LFL'
	   Exe = 'Indiana Jones and the Last Crusade.exe'; Subtitles = $false; SteamDrop = '^(platform|language|extra|guioptions)='   # no speech
	   SteamKeys = @('platform=windows', 'language=en', 'extra=Steam',
	                 'guioptions=sndNoSpeech sndNoMIDI gameOption2 gameOption4 gameOption9 midiPCSpk midiCMS midiPCJr midiAdLib lang_English plat_windows') },
	# Loom: Fixed keys pick the VGA CD talkie among Loom's versions (as ScummVM's detection writes them); Steam's Loom.exe
	# holds the index of the same data.
	@{ Id = 'loom'; Title = 'Loom'; Test = 'Test-LoomDir'; AppId = '32340'; Gog = 'Loom*'; Path = $LoomPath; Param = 'LoomPath'
	   Data = '000.LFL and DISK01.LEC of the VGA CD version (or Loom.exe and DISK01.LEC from Steam)'
	   Browse = 'the VGA CD version: it contains 000.LFL and DISK01.LEC, or Loom.exe'
	   Exe = 'Loom.exe'; Subtitles = $true; SteamDrop = '^(platform|language|extra|guioptions)='
	   SteamKeys = @('platform=windows', 'extra=Steam'); Fixed = @('platform=pc', 'extra=VGA') },
	@{ Id = 'samnmax'; Title = 'Sam & Max Hit the Road'; Test = 'Test-SamnmaxDir'; AppId = '355170'; Gog = '*Sam*Max Hit the Road*'
	   Path = $SamnmaxPath; Param = 'SamnmaxPath'
	   Data = 'SAMNMAX.000 and SAMNMAX.001 of the CD version'; Browse = 'the CD version: it contains SAMNMAX.000, or a SAMNMAX folder'
	   Exe = $null; Subtitles = $true; SteamDrop = '^(platform|extra|guioptions)='; SteamKeys = @() }
)
$want = @($Games -split '[\s,]+' | Where-Object { $_ })
foreach ($g in $want) { if ($g -notin @('comi') + @($ExtraGames | ForEach-Object { $_.Id })) { throw "-Games: unknown game '$g' (comi, dig, atlantis, indy3, loom, samnmax, or a list such as comi,dig)." } }
$lookComi = $want.Count -eq 0 -or $want -contains 'comi'
$game = if ($GamePath) { Test-GameDir $GamePath } else { $null }
foreach ($x in $ExtraGames) {
	$x.Look = $want.Count -eq 0 -or $want -contains $x.Id
	$x.Dir = if ($x.Path) { & $x.Test $x.Path } else { $null }
	$x.Do = $false
	$x.Old = $null
}
if (-not $FindGame) {
	if ($GamePath -and -not $game) { throw "No Curse of Monkey Island data (COMI.LA0 and RESOURCE\) found in '$GamePath'." }
	foreach ($x in $ExtraGames) { if ($x.Path -and -not $x.Dir) { throw "No $($x.Title) data ($($x.Data)) found in '$($x.Path)'." } }
}
if ($lookComi -and -not $GamePath) {
	if (-not $game) { $game = Find-Steam; if ($game) { Say "Found Steam copy: $game" } }
	if (-not $game) { $game = Find-GOG; if ($game) { Say "Found GOG copy: $game" } }
}
foreach ($x in $ExtraGames) {
	if (-not $x.Look -or $x.Path) { continue }
	if (-not $x.Dir) { $x.Dir = Find-Steam $x.AppId $x.Test; if ($x.Dir) { Say "Found $($x.Title) (Steam copy): $($x.Dir)" } }
	if (-not $x.Dir) { $x.Dir = Find-GOG $x.Gog $x.Test; if ($x.Dir) { Say "Found $($x.Title) (GOG copy): $($x.Dir)" } }
}
if ($FindGame) {
	if ($game -and $lookComi) { "game=$game" }
	foreach ($x in $ExtraGames) { if ($x.Dir -and $x.Look) { "$($x.Id)=$($x.Dir)" } }
	return
}
# Which games: -Games, otherwise Monkey Island, and each other game that was found when you say so.
$doComi = $lookComi
foreach ($x in $ExtraGames) { $x.Do = $want -contains $x.Id }
if ($want.Count -eq 0) {
	$foundExtra = @($ExtraGames | Where-Object { $_.Dir })
	$asked = $false
	foreach ($x in $foundExtra) {
		Write-Host ''
		if ($game -or $GamePath -or $asked) {
			$r = Read-Host "Also found $($x.Title) ($($x.Dir)). Set it up too (experimental)? [Y/n]"
			$x.Do = $r -notmatch '^(n|no)$'
		} else {
			$q = if ($foundExtra.Count -gt 1) { "Set up $($x.Title) instead" } else { "Set up only $($x.Title)" }
			$r = Read-Host "The Curse of Monkey Island was not found, but $($x.Title) was ($($x.Dir)). $q (experimental)? [Y/n]"
			if ($r -notmatch '^(n|no)$') { $doComi = $false; $x.Do = $true }
		}
		$asked = $true
	}
}
if ($doComi -and -not $game) {
	Add-Type -AssemblyName System.Windows.Forms
	$dlg = New-Object System.Windows.Forms.FolderBrowserDialog
	$dlg.Description = 'Select your The Curse of Monkey Island folder (it contains COMI.LA0)'
	if ($dlg.ShowDialog() -eq 'OK') { $game = Test-GameDir $dlg.SelectedPath }
	if (-not $game) { throw 'Game data not found. Install the game from Steam or GOG, or run: .\install.ps1 -GamePath "<folder with COMI.LA0>"' }
}
foreach ($x in $ExtraGames) {
	if (-not $x.Do -or $x.Dir) { continue }
	Add-Type -AssemblyName System.Windows.Forms
	$dlg = New-Object System.Windows.Forms.FolderBrowserDialog
	$dlg.Description = "Select your $($x.Title) folder ($($x.Browse))"
	if ($dlg.ShowDialog() -eq 'OK') { $x.Dir = & $x.Test $dlg.SelectedPath }
	if (-not $x.Dir) { throw "$($x.Title) not found. Install it from Steam or GOG, or run: .\install.ps1 -Games $($x.Id) -$($x.Param) `"<folder of $($x.Title)>`"" }
}
$extras = @($ExtraGames | Where-Object { $_.Do })

# ── Files ─────────────────────────────────────────────────────────────────────────────────────────
New-Item -ItemType Directory -Force $InstallDir, (Join-Path $InstallDir 'saves'), (Join-Path $InstallDir 'logs') | Out-Null
if ((Resolve-Path $pkg).Path -ne (Resolve-Path $InstallDir).Path) {
	if (Get-Process scummvm -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$InstallDir*" }) { throw 'The game is running from the install folder. Close it and run the installer again.' }
	foreach ($item in 'scummvm.exe', 'SDL2.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'DirectML.dll', 'models', 'licenses', 'README.txt', 'install.ps1', 'install.cmd', 'install-gui.ps1', 'prewarm.ps1', 'generate_sides.ps1', 'generate_sides.cmd', 'sides-tools', 'generate_cutscenes.ps1', 'generate_cutscenes.cmd', 'video-tools', 'scummmodern.zip') {  # extras: see below
		$s = Join-Path $pkg $item
		if (Test-Path $s) { Copy-Item $s $InstallDir -Recurse -Force }
	}
	Say "Installed files to $InstallDir"
}

# ── Side art / widened cutscenes you generated yourself ────────────────────────────────────────────
# Nothing AI-generated from the game's art is distributed (see the README). Side art (sides\) or widened cutscenes
# (video-sides\) that the player generated from their own copy are installed when they sit next to this installer
# (or one folder up); otherwise narrow rooms use the ambient glow and cutscenes black bars.
function Install-Extra([string]$Answer, [string]$Folder, [string]$Title) {
	$dest = Join-Path $InstallDir $Folder
	if ($Answer -eq 'no') {
		return (Test-Path $dest)
	}
	$src = @((Join-Path $pkg $Folder), (Join-Path (Split-Path $pkg -Parent) $Folder)) | Where-Object { Test-Path $_ } | Select-Object -First 1
	if ($src -and (Resolve-Path $src).Path -ne $dest) {
		if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
		Copy-Item $src $InstallDir -Recurse -Force
		Say "Installed your $Title."
		return $true
	}
	if (Test-Path $dest) {
		Say "Kept your installed $Title."
		return $true
	}
	return $false
}

$sidesDir = Join-Path $InstallDir 'sides'
$videoDir = Join-Path $InstallDir 'video-sides'
$useSides = $false
$useVideo = $false
if ($doComi) {   # Monkey Island's extras (the other games have none)
	$useSides = Install-Extra $SideArt 'sides' 'side art'
	$useVideo = Install-Extra $Cutscenes 'video-sides' 'widened cutscenes'
	if (-not $useSides) { Say 'Narrow rooms use the ambient glow at the sides (side art can be generated from your own copy, see the README).' }
}

# ── TensorRT upscaler (optional, NVIDIA only) ─────────────────────────────────────────────────────────
# Official packages (Microsoft's ONNX Runtime GPU build; NVIDIA's TensorRT, CUDA runtime, cuBLAS and cuDNN), each checked
# against its SHA-256; only the DLLs the upscaler needs are kept.
$TrtPackages = @(
	@{ Url = 'https://files.pythonhosted.org/packages/50/fe/ed63f743d546fd089f98cfd415790b231ae8d3dba2208df8d22811f788a5/onnxruntime_gpu-1.30.0-cp314-cp314-win_amd64.whl'
	   Sha = '794fc118cdbf340ed02b44c7527e557ea51746002d48105d493ea41f0c9a60d0'; Keep = '^onnxruntime/capi/onnxruntime(_providers_(shared|tensorrt|cuda))?\.dll$' },
	@{ Url = 'https://pypi.nvidia.com/tensorrt-cu13-libs/tensorrt_cu13_libs-10.16.1.11-py3-none-win_amd64.whl'
	   Sha = '96262c3e8c64a45abd29aa3482d99480fac6845bd420b6de125699bb1ae365ff'; Keep = '^tensorrt_libs/nv(infer|onnxparser)[^/]*\.dll$' },
	@{ Url = 'https://files.pythonhosted.org/packages/86/00/d5436004268f049214193659ebc36550b5ef3925c3d13b4cc980e13be6f5/nvidia_cuda_runtime-13.4.92-py3-none-win_amd64.whl'
	   Sha = '08dca5e4aba480c2fd5b55075c0fa71b84ef9dcf0521f2d58baa14a803a7311c'; Keep = '/cudart64_13\.dll$' },
	@{ Url = 'https://files.pythonhosted.org/packages/a3/df/f1246959833e2c437db8be3e5b477f66b87f8817821ed40de6c7561c9a36/nvidia_cublas-13.8.0.4-py3-none-win_amd64.whl'
	   Sha = '8c5494423bb8a46822cb6b0cb95d7fa4be2d7b96a31155dff083839ec8297910'; Keep = '/cublas(Lt)?64_13\.dll$' },
	@{ Url = 'https://files.pythonhosted.org/packages/87/6a/e55ff0ac26a5c6e2b21f41c9d04ad096b4ed6da593fba7e25845c61b0532/nvidia_cudnn_cu13-9.27.0.42-py3-none-win_amd64.whl'
	   Sha = '7d96f634adafd55c72231eb0500ca77ab109ec8ebff7b33000b76e081bc4558e'; Keep = '/cudnn[^/]*\.dll$' }
)
function Install-TensorRT([string]$Answer) {
	$dest = Join-Path $InstallDir 'tensorrt'
	$gpu = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'NVIDIA' } | Select-Object -First 1
	if (-not $gpu) { return }
	if ((Test-Path (Join-Path $dest 'nvinfer_10.dll')) -and $Answer -eq 'ask') {
		Say 'TensorRT upscaler: already installed.'
		return
	}
	if ($Answer -eq 'ask') {
		Write-Host ''
		Write-Host "TensorRT upscaler (optional, for your $($gpu.Name)): NVIDIA's own AI engine, the fastest way to run the"
		Write-Host 'upscaling networks on NVIDIA cards. Another choice for Ctrl+F9 in game; everything else works without it.'
		Write-Host 'Downloads about 1.5 GB of official Microsoft and NVIDIA packages (2 GB installed). The first time you'
		Write-Host 'choose it in game, it prepares its engines for your card in the background (a few minutes).'
		Write-Host 'NVIDIA''s libraries come under their licences: https://docs.nvidia.com/deeplearning/tensorrt/sla/index.html'
		Write-Host 'and https://docs.nvidia.com/deeplearning/cudnn/latest/reference/eula.html'
		$r = Read-Host 'Download the TensorRT upscaler and accept those licences? [y/N]'
		$Answer = if ($r -match '^(y|yes)$') { 'yes' } else { 'no' }
	}
	if ($Answer -ne 'yes') { return }
	# Only the TensorRT engine builder for this card's generation (compute capability, e.g. 12.0 -> sm120)
	$arch = $null
	try {
		$cc = (& nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>$null | Select-Object -First 1).Trim()
		if ($cc -match '^(\d+)\.(\d+)$') { $arch = "sm$($Matches[1])$($Matches[2])" }
	} catch { }
	Add-Type -AssemblyName System.IO.Compression.FileSystem
	New-Item -ItemType Directory -Force $dest | Out-Null
	$ProgressPreference = 'SilentlyContinue'
	foreach ($p in $TrtPackages) {
		$name = Split-Path $p.Url -Leaf
		$whl = Join-Path $env:TEMP $name
		Say "Downloading $name..."
		try {
			Invoke-WebRequest $p.Url -OutFile $whl -UseBasicParsing
		} catch {
			Say "Could not download $name ($($_.Exception.Message)). TensorRT skipped; run the installer again later."
			return
		}
		if ((Get-FileHash $whl -Algorithm SHA256).Hash.ToLower() -ne $p.Sha) {
			Remove-Item $whl -Force
			Say "$name does not match its published checksum; TensorRT skipped."
			return
		}
		$zip = [System.IO.Compression.ZipFile]::OpenRead($whl)
		try {
			foreach ($e in $zip.Entries) {
				$n = $e.FullName
				if ($n -notmatch $p.Keep) { continue }
				if ($arch -and $n -match 'nvinfer_builder_resource_(sm\d+|ptx)_' -and $n -notmatch "_$($arch)_") { continue }
				[System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $dest $e.Name), $true)
			}
			foreach ($e in $zip.Entries) {   # their licences
				if ($e.FullName -match '(\.dist-info/(licenses/)?(LICENSE|License)[^/]*|^onnxruntime/(LICENSE|ThirdPartyNotices\.txt))$') {
					New-Item -ItemType Directory -Force (Join-Path $dest 'licenses') | Out-Null
					[System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $dest "licenses\$($name -replace '-.*$', '')-$($e.Name)"), $true)
				}
			}
		} finally { $zip.Dispose() }
		Remove-Item $whl -Force
	}
	Say "Installed the TensorRT upscaler$(if ($arch) { " (engines for $arch)" }). In game: Ctrl+F9 until it says TensorRT."
}
Install-TensorRT $TensorRT

# ── Configuration ─────────────────────────────────────────────────────────────────────────────────
$ini = Join-Path $InstallDir 'scummvm.ini'
$model = Join-Path $InstallDir 'models\realesr-animevideov3-x3'
# The other games' targets ([dig], [atlantis], [indy3]) are taken out while Monkey Island's settings are written as
# before, and put back (updated) at the end, below.
if (Test-Path $ini) {
	$all = @(Get-Content $ini)
	$cut = $false
	foreach ($x in $ExtraGames) {
		$i = [array]::IndexOf($all, "[$($x.Id)]")
		if ($i -lt 0) { continue }
		$j = $i + 1
		while ($j -lt $all.Count -and $all[$j] -notmatch '^\[') { $j++ }
		$x.Old = @($all[$i..($j - 1)] | Where-Object { $_ -ne '' })
		$rest = @()
		if ($i -gt 0) { $rest += $all[0..($i - 1)] }
		if ($j -lt $all.Count) { $rest += $all[$j..($all.Count - 1)] }
		$n = $rest.Count
		while ($n -gt 1 -and $rest[$n - 1] -eq '') { $n-- }
		$all = @($rest[0..($n - 1)])
		$cut = $true
	}
	if ($cut) { $all | Set-Content $ini -Encoding ascii }
}
$first = if ($doComi) { 'comi' } else { $extras[0].Id }
$GlobalSection = @"
[scummvm]
gui_theme=scummmodern
themepath=$InstallDir
gui_scale=200
lastselectedgame=$first
fullscreen=true
gfx_mode=opengl
stretch_mode=fit
filtering=true
savepath=$InstallDir\saves
screenshotpath=$InstallDir\logs
updates_check=0
"@
$ComiSection = @"
[comi]
gameid=comi
engineid=scumm
description=The Curse of Monkey Island (AI upscaling)
path=$game
savepath=$InstallDir\saves
subtitles=true
remaster_ai_model=$model
remaster_ai_scale=auto
remaster_widescreen=wide
remaster_ambient_sides=true
"@
if ((Test-Path $ini) -and -not $doComi) {
	(Get-Content $ini) -replace '^remaster_ai_model=.*', "remaster_ai_model=$model" | Set-Content $ini -Encoding ascii
	Say 'Kept your existing settings (scummvm.ini).'
	$newIni = $false
} elseif (Test-Path $ini) {
	$lines = @(Get-Content $ini)
	if ($lines -notcontains '[comi]') { $lines += @('') + ($ComiSection -split "`r?`n") }   # set up before for other games only
	$lines = $lines -replace '^path=.*', "path=$game" -replace '^remaster_ai_model=.*', "remaster_ai_model=$model" |
		Where-Object { $_ -notmatch '^remaster_(sides_path|video_sides_path)=' }
	if ($useSides) { $lines += "remaster_sides_path=$sidesDir" }
	if ($useVideo) { $lines += "remaster_video_sides_path=$videoDir" }
	$lines | Set-Content $ini -Encoding ascii
	Say 'Kept your existing settings (scummvm.ini), updated the game path.'
	$newIni = $false
} elseif (-not $doComi) {
	$GlobalSection | Set-Content $ini -Encoding ascii
	Say 'Wrote scummvm.ini.'
	$newIni = $true
} else {
	"$GlobalSection`n`n$ComiSection" | Set-Content $ini -Encoding ascii
	if ($useSides) { Add-Content $ini "remaster_sides_path=$sidesDir" -Encoding ascii }
	if ($useVideo) { Add-Content $ini "remaster_video_sides_path=$videoDir" -Encoding ascii }
	Say 'Wrote scummvm.ini.'
	$newIni = $true
}

# ── Preset ────────────────────────────────────────────────────────────────────────────────────────────
# A starting point: writes the matching individual settings (all still changeable in game: F10, F11, F12, ...).
$Presets = @{
	original     = @{ set = @('remaster_display_mode=1', 'remaster_ai_enabled=false', 'remaster_side_art=false', 'remaster_smooth_scroll=false')
	                  drop = @('remaster_widescreen') }   # plain 4:3, as the game was
	enhanced     = @{ set = @('remaster_display_mode=0', 'remaster_ai_enabled=true', 'remaster_widescreen=wide', 'remaster_ambient_sides=true',
	                          'remaster_side_art=false', 'remaster_video_sides=off', 'remaster_hd_actors=true', 'remaster_hd_backgrounds=true',
	                          'remaster_smooth_scroll=false')
	                  drop = @() }
	experimental = @{ set = @('remaster_display_mode=0', 'remaster_ai_enabled=true', 'remaster_widescreen=wide', 'remaster_ambient_sides=true',
	                          'remaster_side_art=true', 'remaster_hd_actors=true', 'remaster_hd_backgrounds=true', 'remaster_smooth_scroll=true')
	                  drop = @('remaster_video_sides') }
}
function Set-Preset([string]$name) {
	$p = $Presets[$name]
	$keys = @($p.set | ForEach-Object { ($_ -split '=')[0] }) + $p.drop
	$lines = @(Get-Content $ini | Where-Object { ($_ -split '=')[0] -notin $keys })
	$i = [array]::IndexOf($lines, '[comi]')
	$lines = @($lines[0..$i]) + $p.set + @($lines[($i + 1)..($lines.Count - 1)])
	$lines | Set-Content $ini -Encoding ascii
	Say "Preset: $name."
}
$chosen = $Preset
if ($chosen -eq 'ask' -and $newIni) {
	Write-Host ''
	Write-Host 'How do you want to play? This is only a starting point: everything can be changed in game, and F10'
	Write-Host 'switches between the original look and the upscaled one at any time.'
	Write-Host ''
	Write-Host '  1  Original      No AI at all. The game as it looked in 1997 (original pixels, 4:3), with the extras'
	Write-Host '     (no AI)       that need no AI: controller support, photo mode, VGA/EGA/Amiga looks and CRT'
	Write-Host '                   scanlines (F10, Ctrl+F10). Also the best choice for older or low-power PCs.'
	Write-Host ''
	Write-Host '  2  Enhanced      Recommended. The game redrawn sharp at your screen''s resolution by AI on your'
	Write-Host '                   graphics card, filling a modern 16:9 screen: wide rooms show more of the room,'
	Write-Host '                   narrow ones get a soft glow at the sides. Characters and backgrounds get extra detail.'
	Write-Host ''
	Write-Host '  3  Experimental  Enhanced, plus the work-in-progress extras: painted scenery beside the narrow rooms'
	Write-Host '                   and full-width cutscenes, if you make or generate them (offered next), and smoother'
	Write-Host '                   camera movement.'
	Write-Host ''
	$r = Read-Host 'Choose 1, 2 or 3 (Enter = 2)'
	$chosen = switch ($r) { '1' { 'original' } '3' { 'experimental' } default { 'enhanced' } }
}
if ($chosen -eq 'classic') { $chosen = 'original' }
if ($doComi -and $chosen -in 'original', 'enhanced', 'experimental') { Set-Preset $chosen }

# -- The other games (experimental) ------------------------------------------------------------------------
# Each has its own target at the end of scummvm.ini. Their defaults are in the game's profile in the program (HD
# characters off; sharp text and the grain guard as suits the art), so a preset only writes what applies to each:
# true widescreen and the glow beside narrow rooms; side art and widened cutscenes are Monkey Island's. Smooth
# scrolling needs HD characters, so it is left out for the Indiana Jones games, Loom and Sam & Max (preset "indy").
$ExtraPresets = @{
	dig = @{
		original     = @{ set = @('remaster_display_mode=1', 'remaster_ai_enabled=false', 'remaster_smooth_scroll=false')
		                  drop = @('remaster_widescreen') }
		# the look (pixel-perfect widescreen: original pixels, black side bars) and HD characters come from the profile
		enhanced     = @{ set = @('remaster_widescreen=wide', 'remaster_hd_backgrounds=true', 'remaster_smooth_scroll=false')
		                  drop = @('remaster_hd_actors', 'remaster_display_mode', 'remaster_ai_enabled', 'remaster_ambient_sides') }
		experimental = @{ set = @('remaster_widescreen=wide', 'remaster_hd_backgrounds=true', 'remaster_smooth_scroll=true')
		                  drop = @('remaster_hd_actors', 'remaster_display_mode', 'remaster_ai_enabled', 'remaster_ambient_sides') }
	}
	indy = @{
		original     = @{ set = @('remaster_display_mode=1', 'remaster_ai_enabled=false')
		                  drop = @('remaster_widescreen') }
		enhanced     = @{ set = @('remaster_widescreen=wide', 'remaster_hd_backgrounds=true')
		                  drop = @('remaster_hd_actors', 'remaster_sharp_text', 'remaster_grain_guard', 'remaster_display_mode', 'remaster_ai_enabled',
		                           'remaster_ambient_sides') }   # the game profile decides (the look: pixel-perfect widescreen)
	}
}
$ExtraPresets.indy.experimental = $ExtraPresets.indy.enhanced   # the experimental extras are Monkey Island's
foreach ($x in $ExtraGames) {
	if (-not ($x.Do -or $x.Old)) { continue }
	$block = if ($x.Old) { @($x.Old) } else {
		@("[$($x.Id)]", "gameid=$($x.Id)", 'engineid=scumm', "description=$($x.Title) (AI upscaling)", "path=$($x.Dir)", "savepath=$InstallDir\saves") +
		@(if ($x.Subtitles) { 'subtitles=true' }) +
		@("remaster_ai_model=$model", 'remaster_ai_scale=auto', 'remaster_widescreen=wide')
	}
	$block = @($block -replace '^remaster_ai_model=.*', "remaster_ai_model=$model")
	if ($x.Do) {
		# The Steam version: what ScummVM's own detection writes for it (the index is in the game's .exe)
		$steamKeys = @()
		if ($x.Exe -and (Test-Path (Join-Path $x.Dir $x.Exe)) -and -not ($x.Id -eq 'loom' -and (Test-Path (Join-Path $x.Dir '000.LFL')))) { $steamKeys = $x.SteamKeys }
		elseif ($x.Fixed) { $steamKeys = $x.Fixed }
		$b = @()
		foreach ($l in $block) {
			if ($l -match $x.SteamDrop) { continue }
			if ($l -match '^path=') { $b += "path=$($x.Dir)"; $b += $steamKeys } else { $b += $l }
		}
		$block = $b
		if ($chosen -in 'original', 'enhanced', 'experimental') {
			$p = $ExtraPresets[$(if ($x.Id -eq 'dig') { 'dig' } else { 'indy' })][$chosen]
			$keys = @($p.set | ForEach-Object { ($_ -split '=')[0] }) + $p.drop
			$block = @($block[0]) + $p.set + @($block | Select-Object -Skip 1 | Where-Object { ($_ -split '=')[0] -notin $keys })
		}
	}
	$lines = @(Get-Content $ini)
	$n = $lines.Count
	while ($n -gt 1 -and $lines[$n - 1] -eq '') { $n-- }
	@($lines[0..($n - 1)]) + '' + $block | Set-Content $ini -Encoding ascii
	if ($x.Do) { Say "$($x.Title): set up in scummvm.ini (target $($x.Id))$(if ($chosen -in 'original', 'enhanced', 'experimental') { ", preset $chosen" })." }
	if ($x.Do -and $x.Id -eq 'loom' -and (Test-Path (Join-Path $x.Dir '000.LFL')) -and -not (Test-Path (Join-Path $x.Dir 'track1.flac'))) {
		Say 'Loom: no CD audio track (track1.flac) next to the game files; speech and music come from it. Rip it from your CD as FLAC into that folder.'
	}
}
if (-not (Test-Path (Join-Path $env:WINDIR 'System32\vulkan-1.dll'))) {
	Say 'Note: no Vulkan driver found. The game still runs, but without AI upscaling. Update your graphics driver to enable it.'
}

# ── Shortcuts ─────────────────────────────────────────────────────────────────────────────────────
function New-GameShortcuts([string]$target, [string]$title) {   # the game's target id is the last argument
	$exe = Join-Path $InstallDir 'scummvm.exe'
	$scArgs = "--config=`"$ini`" --logfile=`"$InstallDir\logs\scummvm.log`" $target"
	$ws = New-Object -ComObject WScript.Shell
	foreach ($dir in @([Environment]::GetFolderPath('Desktop'), (Join-Path ([Environment]::GetFolderPath('Programs')) 'ScummVM AI Upscale'))) {
		New-Item -ItemType Directory -Force $dir | Out-Null
		$s = $ws.CreateShortcut((Join-Path $dir "$title (AI upscaling).lnk"))
		$s.TargetPath = $exe; $s.Arguments = $scArgs; $s.WorkingDirectory = Join-Path $InstallDir 'logs'
		$s.IconLocation = "$exe,0"; $s.Description = "$title with real-time AI upscaling (scummvm-ai-upscale)"
		$s.Save()
		if ($dir -notlike '*Desktop*') {
			# Start menu only: a windowed variant (resizable; Alt+Enter switches to fullscreen any time).
			$s = $ws.CreateShortcut((Join-Path $dir "$title (windowed).lnk"))
			$s.TargetPath = $exe; $s.Arguments = "--no-fullscreen --window-size=1280,720 $scArgs"; $s.WorkingDirectory = Join-Path $InstallDir 'logs'
			$s.IconLocation = "$exe,0"; $s.Description = "$title in a resizable window"
			$s.Save()
		}
	}
}
if (-not $NoShortcuts) {
	if ($doComi) { New-GameShortcuts 'comi' 'The Curse of Monkey Island' }
	foreach ($x in $extras) { New-GameShortcuts $x.Id $x.Title }
	Say 'Created desktop and Start menu shortcuts (Start menu also has a windowed version).'
}
# ── Download mark ─────────────────────────────────────────────────────────────────────────────────
# Files extracted from a downloaded zip carry Windows' "from the internet" mark. Windows already asked once,
# when install.cmd was started; like any setup program, the installed copies don't keep the mark, so the game
# (and the minimized prepare step below) don't stop at a second prompt. Only this program's own files.
Get-ChildItem $InstallDir -Recurse -File -Include *.exe, *.dll, *.ps1, *.cmd -ErrorAction SilentlyContinue |
	Where-Object { $_.FullName -notlike (Join-Path $InstallDir 'saves\*') } | Unblock-File -ErrorAction SilentlyContinue

# ── Side art, generated from the player's own copy (optional) ────────────────────────────────────────────
$haveSides = @(Get-ChildItem (Join-Path $InstallDir 'sides') -Filter '*.png' -ErrorAction SilentlyContinue).Count -gt 0
if ($GenerateSideArt -eq 'ask' -and $chosen -eq 'original') { $GenerateSideArt = 'no' }   # no AI wanted (generate_sides.cmd any time)
if ($doComi -and -not $haveSides -and $GenerateSideArt -ne 'no' -and (Test-Path (Join-Path $InstallDir 'generate_sides.ps1'))) {
	$m = $GenerateSideArt
	if ($m -eq 'ask') {
		Write-Host ''
		Write-Host 'Optional: painted scenery beside the narrow rooms (side art), made now on this PC from your copy of the game'
		Write-Host '(nothing like it is distributed), for all 55 rooms in one go. It uses an AI model of your choice:'
		Write-Host '  - your own OpenRouter account: best results, about 4 USD, a few minutes'
		Write-Host '  - Stable Diffusion XL on an NVIDIA card: free and offline, about 10 GB download, roughly 15-30 minutes'
		Write-Host 'Without it the narrow rooms show an ambient glow at the sides. You can also do it later with'
		Write-Host '"generate_sides.cmd" in the install folder; F12 in game switches between side art, glow and black bars.'
		$r = Read-Host 'Generate side art now? [y/N]'
		$m = if ($r -match '^(y|yes)$') { 'ask' } else { 'no' }
	}
	if ($m -ne 'no') {
		try { & (Join-Path $InstallDir 'generate_sides.ps1') -InstallDir $InstallDir -Method $m }
		catch { Say "Side art not generated: $($_.Exception.Message) (run generate_sides.cmd any time)." }
	}
}

# ── HD backgrounds cache (optional, like a shader cache) ───────────────────────────────────────────────
if ($extras.Count -and (Test-Path (Join-Path $env:WINDIR 'System32\vulkan-1.dll'))) {
	$t = @($extras | ForEach-Object { $_.Title })
	$t = if ($t.Count -gt 1) { ($t[0..($t.Count - 2)] -join ', ') + ' and ' + $t[-1] } else { $t[0] }
	Say "$($t): each room's HD background is prepared the first time you enter it (no prepare step for $(if ($extras.Count -gt 1) { 'them' } else { $t }) yet)."
}
if ($doComi -and (Test-Path (Join-Path $env:WINDIR 'System32\vulkan-1.dll')) -and (Test-Path (Join-Path $InstallDir 'prewarm.ps1'))) {
	$answer = $Prewarm
	if ($answer -eq 'ask') {
		Write-Host ''
		Write-Host 'HD backgrounds: each room''s background is upscaled once with a heavier AI model and kept.'
		Write-Host 'Preparing them now (a few minutes, the game opens and closes minimized) means every room is HD'
		Write-Host 'from the first frame. Otherwise each room is prepared the first time you enter it.'
		if (Test-Path (Join-Path $InstallDir 'tensorrt\nvinfer_10.dll')) { Write-Host 'It also prepares the TensorRT upscaler for your card (a few minutes more the first time).' }
		$r = Read-Host 'Prepare HD backgrounds now? [Y/n]'
		$answer = if ($r -match '^(n|no)$') { 'no' } else { 'yes' }
	}
	if ($answer -eq 'yes') { & (Join-Path $InstallDir 'prewarm.ps1') -InstallDir $InstallDir }
}
Say 'Done. In game: F9 (controller: Back) = remaster settings, F10 = display mode, F11 = AI on/off, F12 = what fills the sides of narrow rooms, Shift+F9 = photo, Alt+Enter = window/fullscreen.'
if (@($extras | Where-Object { $_.Id -eq 'loom' }).Count) { Say 'Loom uses F9 itself (text on screen on/off): there the remaster settings are on Alt+F9.' }
