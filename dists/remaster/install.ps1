<#
.SYNOPSIS
  Installs scummvm-ai-upscale for The Curse of Monkey Island (you need your own copy of the game).

.DESCRIPTION
  - Copies this package to -InstallDir (default: %LOCALAPPDATA%\ScummVM-AI-Upscale).
  - Finds your Steam or GOG installation of The Curse of Monkey Island, or uses -GamePath.
    The game files are only read, never copied or changed.
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

.EXAMPLE
  .\install.ps1
  .\install.ps1 -GamePath "D:\Games\The Curse of Monkey Island"
#>
[CmdletBinding()]
param(
	[string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'ScummVM-AI-Upscale'),
	[string]$GamePath,
	[switch]$NoShortcuts,
	[ValidateSet('ask', 'yes', 'no')][string]$SideArt = 'ask',
	[ValidateSet('ask', 'yes', 'no')][string]$Cutscenes = 'ask',
	[ValidateSet('ask', 'yes', 'no')][string]$Prewarm = 'ask',
	[ValidateSet('ask', 'yes', 'no')][string]$TensorRT = 'ask',
	[ValidateSet('ask', 'cloud', 'local', 'no')][string]$GenerateSideArt = 'ask',
	[ValidateSet('ask', 'original', 'classic', 'enhanced', 'experimental', 'keep')][string]$Preset = 'ask'
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

function Find-Steam {
	$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
	if (-not $steam) { return $null }
	$vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
	$libs = @($steam)
	if (Test-Path $vdf) {
		$libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
	}
	foreach ($lib in ($libs | Select-Object -Unique)) {
		$acf = Join-Path $lib 'steamapps\appmanifest_730820.acf'   # The Curse of Monkey Island
		if (Test-Path $acf) {
			$dir = (Select-String -Path $acf -Pattern '"installdir"\s+"([^"]+)"').Matches[0].Groups[1].Value
			$found = Test-GameDir (Join-Path $lib "steamapps\common\$dir")
			if ($found) { return $found }
		}
	}
	return $null
}

function Find-GOG {
	foreach ($root in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games') {
		if (-not (Test-Path $root)) { continue }
		foreach ($k in Get-ChildItem $root) {
			$p = Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue
			if ($p.gameName -like '*Curse of Monkey Island*') {
				$found = Test-GameDir $p.path
				if ($found) { return $found }
			}
		}
	}
	return $null
}

# ── Game data ─────────────────────────────────────────────────────────────────────────────────────
$game = if ($GamePath) { Test-GameDir $GamePath } else { $null }
if ($GamePath -and -not $game) { throw "No Curse of Monkey Island data (COMI.LA0 and RESOURCE\) found in '$GamePath'." }
if (-not $game) { $game = Find-Steam; if ($game) { Say "Found Steam copy: $game" } }
if (-not $game) { $game = Find-GOG; if ($game) { Say "Found GOG copy: $game" } }
if (-not $game) {
	Add-Type -AssemblyName System.Windows.Forms
	$dlg = New-Object System.Windows.Forms.FolderBrowserDialog
	$dlg.Description = 'Select your The Curse of Monkey Island folder (it contains COMI.LA0)'
	if ($dlg.ShowDialog() -eq 'OK') { $game = Test-GameDir $dlg.SelectedPath }
	if (-not $game) { throw 'Game data not found. Install the game from Steam or GOG, or run: .\install.ps1 -GamePath "<folder with COMI.LA0>"' }
}

# ── Files ─────────────────────────────────────────────────────────────────────────────────────────
New-Item -ItemType Directory -Force $InstallDir, (Join-Path $InstallDir 'saves'), (Join-Path $InstallDir 'logs') | Out-Null
if ((Resolve-Path $pkg).Path -ne (Resolve-Path $InstallDir).Path) {
	if (Get-Process scummvm -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$InstallDir*" }) { throw 'The game is running from the install folder. Close it and run the installer again.' }
	foreach ($item in 'scummvm.exe', 'SDL2.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'DirectML.dll', 'models', 'licenses', 'README.txt', 'install.ps1', 'install.cmd', 'prewarm.ps1', 'generate_sides.ps1', 'generate_sides.cmd', 'sides-tools', 'generate_cutscenes.ps1', 'generate_cutscenes.cmd', 'video-tools', 'scummmodern.zip') {  # extras: see below
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
$useSides = Install-Extra $SideArt 'sides' 'side art'
$videoDir = Join-Path $InstallDir 'video-sides'
$useVideo = Install-Extra $Cutscenes 'video-sides' 'widened cutscenes'
if (-not $useSides) { Say 'Narrow rooms use the ambient glow at the sides (side art can be generated from your own copy, see the README).' }

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
if (Test-Path $ini) {
	$lines = (Get-Content $ini) -replace '^path=.*', "path=$game" -replace '^remaster_ai_model=.*', "remaster_ai_model=$model" |
		Where-Object { $_ -notmatch '^remaster_(sides_path|video_sides_path)=' }
	if ($useSides) { $lines += "remaster_sides_path=$sidesDir" }
	if ($useVideo) { $lines += "remaster_video_sides_path=$videoDir" }
	$lines | Set-Content $ini -Encoding ascii
	Say 'Kept your existing settings (scummvm.ini), updated the game path.'
	$newIni = $false
} else {
	@"
[scummvm]
gui_theme=scummmodern
themepath=$InstallDir
gui_scale=200
lastselectedgame=comi
fullscreen=true
gfx_mode=opengl
stretch_mode=fit
filtering=true
savepath=$InstallDir\saves
screenshotpath=$InstallDir\logs
updates_check=0

[comi]
gameid=comi
engineid=scumm
description=The Curse of Monkey Island (AI upscaling)
path=$game
savepath=$InstallDir\saves
subtitles=true
remaster_ai_model=$model
remaster_ai_scale=3
remaster_widescreen=wide
remaster_ambient_sides=true
"@ | Set-Content $ini -Encoding ascii
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
if ($chosen -in 'original', 'enhanced', 'experimental') { Set-Preset $chosen }
if (-not (Test-Path (Join-Path $env:WINDIR 'System32\vulkan-1.dll'))) {
	Say 'Note: no Vulkan driver found. The game still runs, but without AI upscaling. Update your graphics driver to enable it.'
}

# ── Shortcuts ─────────────────────────────────────────────────────────────────────────────────────
if (-not $NoShortcuts) {
	$exe = Join-Path $InstallDir 'scummvm.exe'
	$scArgs = "--config=`"$ini`" --logfile=`"$InstallDir\logs\scummvm.log`" comi"
	$ws = New-Object -ComObject WScript.Shell
	foreach ($dir in @([Environment]::GetFolderPath('Desktop'), (Join-Path ([Environment]::GetFolderPath('Programs')) 'ScummVM AI Upscale'))) {
		New-Item -ItemType Directory -Force $dir | Out-Null
		$s = $ws.CreateShortcut((Join-Path $dir 'The Curse of Monkey Island (AI upscaling).lnk'))
		$s.TargetPath = $exe; $s.Arguments = $scArgs; $s.WorkingDirectory = Join-Path $InstallDir 'logs'
		$s.IconLocation = "$exe,0"; $s.Description = 'The Curse of Monkey Island with real-time AI upscaling (scummvm-ai-upscale)'
		$s.Save()
		if ($dir -notlike '*Desktop*') {
			# Start menu only: a windowed variant (resizable; Alt+Enter switches to fullscreen any time).
			$s = $ws.CreateShortcut((Join-Path $dir 'The Curse of Monkey Island (windowed).lnk'))
			$s.TargetPath = $exe; $s.Arguments = "--no-fullscreen --window-size=1280,720 $scArgs"; $s.WorkingDirectory = Join-Path $InstallDir 'logs'
			$s.IconLocation = "$exe,0"; $s.Description = 'The Curse of Monkey Island in a resizable window'
			$s.Save()
		}
	}
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
if ($GenerateSideArt -eq 'ask' -and $chosen -ne 'experimental') { $GenerateSideArt = 'no' }   # see generate_sides.cmd
if (-not $haveSides -and $GenerateSideArt -ne 'no' -and (Test-Path (Join-Path $InstallDir 'generate_sides.ps1'))) {
	$m = $GenerateSideArt
	if ($m -eq 'ask') {
		Write-Host ''
		Write-Host 'Optional: widescreen side art for the narrow rooms, generated now on this PC from your copy of the game'
		Write-Host '(nothing like it is distributed). It uses an AI model: your own OpenRouter account (about 4 USD, best'
		Write-Host 'results) or Stable Diffusion XL on an NVIDIA card (free, about 10 GB download). Without it the narrow'
		Write-Host 'rooms show an ambient glow at the sides. You can do it later with "generate_sides.cmd" in the install folder.'
		$r = Read-Host 'Generate side art now? [y/N]'
		$m = if ($r -match '^(y|yes)$') { 'ask' } else { 'no' }
	}
	if ($m -ne 'no') {
		try { & (Join-Path $InstallDir 'generate_sides.ps1') -InstallDir $InstallDir -Method $m }
		catch { Say "Side art not generated: $($_.Exception.Message) (run generate_sides.cmd any time)." }
	}
}

# ── HD backgrounds cache (optional, like a shader cache) ───────────────────────────────────────────────
if ((Test-Path (Join-Path $env:WINDIR 'System32\vulkan-1.dll')) -and (Test-Path (Join-Path $InstallDir 'prewarm.ps1'))) {
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
Say 'Done. In game: F10 = display mode, F11 = AI on/off, Ctrl+F9 = upscaler, Shift+F9 = photo, F12 = side art / glow / black bars, Alt+Enter = window/fullscreen.'
