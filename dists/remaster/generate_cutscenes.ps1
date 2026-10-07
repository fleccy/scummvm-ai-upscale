<#
.SYNOPSIS
  Widens the cutscenes to 16:9 from YOUR OWN copy of The Curse of Monkey Island (advanced, takes hours).

.DESCRIPTION
  scummvm-ai-upscale distributes nothing derived from the game's videos. This script makes the widened cutscenes on
  your PC with the AI Remaster Pipeline (ARP, https://github.com/dtaddis/ai-remaster-pipeline) and Lightricks'
  LTX-2.3 video model with an outpainting LoRA, running locally in ComfyUI:
    1. each cutscene is decoded with ffmpeg and split into shots at hard cuts;
    2. ARP widens every shot to 16:9 (resumable: finished shots are kept if you stop and start again);
    3. the new sides are colour-matched to the real video and packed into video-sides\<VIDEO>.sides;
    4. the game is set to use them (F12 in game switches them on and off).

  This is demanding. ARP's own requirements: an NVIDIA graphics card with 16 GB of VRAM (24 GB recommended),
  32 GB of RAM (64 GB recommended), about 150 GB of free disk space, and many hours for all 15 cutscenes (about
  1 minute per short shot on an RTX 5080). Install ARP first with its own installer (it needs Python 3.13 and Git):
    install_windows.bat -DownloadModels
  and accept the LTX-2.3 licence on Hugging Face when it asks. The outpainting LoRA used here is
  ltx-2.3-22b-ic-lora-outpaint.safetensors (oumoumad/LTX-2.3-22b-IC-LoRA-Outpaint) in ComfyUI's models\loras.
  Generated video is under the LTX-2 Community License; keep it for your own use.

.EXAMPLE
  .\generate_cutscenes.ps1 -ArpDir D:\ai-remaster-pipeline
#>
[CmdletBinding()]
param(
	[string]$InstallDir = $PSScriptRoot,
	[Parameter(Mandatory = $true)][string]$ArpDir,
	[switch]$PackOnly
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { Write-Host "[cutscenes] $m" }
$ini = Join-Path $InstallDir 'scummvm.ini'
$tools = Join-Path $InstallDir 'video-tools'
if (-not (Test-Path $ini) -or -not (Test-Path (Join-Path $tools 'batch_outpaint.py'))) { throw "No installation found in $InstallDir (run install.cmd first)." }
$py = Join-Path $ArpDir '.venv\Scripts\python.exe'
$wrapper = Join-Path $ArpDir 'wrappers\outpaint_video.bat'
$lora = Join-Path $ArpDir 'tools\comfyui\models\loras\ltx-2.3-22b-ic-lora-outpaint.safetensors'
if (-not (Test-Path $py) -or -not (Test-Path $wrapper)) {
	throw "No AI Remaster Pipeline found in $ArpDir. Install it first (https://github.com/dtaddis/ai-remaster-pipeline, install_windows.bat -DownloadModels)."
}
if (-not (Test-Path $lora)) {
	throw "The outpainting LoRA is missing: $lora. Download ltx-2.3-22b-ic-lora-outpaint.safetensors from https://huggingface.co/oumoumad/LTX-2.3-22b-IC-LoRA-Outpaint into that folder."
}
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue) -or -not (Get-Command ffprobe -ErrorAction SilentlyContinue)) {
	throw 'ffmpeg and ffprobe are needed on the PATH (for example: winget install Gyan.FFmpeg).'
}
# the game's videos: the [comi] path from scummvm.ini
$game = (Get-Content $ini | Where-Object { $_ -match '^path=' } | Select-Object -First 1) -replace '^path=', ''
if (-not $game -or -not (Get-ChildItem $game -Recurse -Filter '*.SAN' -ErrorAction SilentlyContinue | Select-Object -First 1)) {
	throw "The game's cutscene files (*.SAN) were not found in '$game'."
}
$work = Join-Path $InstallDir 'video-work'
New-Item -ItemType Directory -Force $work | Out-Null
$env:COMI_GAME = $game
$env:COMI_ARP = $ArpDir
$env:COMI_VIDEO_WORK = $work

if (-not $PackOnly) {
	Write-Host ''
	Write-Host 'This widens all 15 cutscenes with the LTX-2.3 video model on this PC. It takes many hours (overnight is'
	Write-Host 'typical) and keeps the graphics card fully busy. You can stop it (close the window) and run this again: finished'
	Write-Host 'shots are kept. Progress is written to' (Join-Path $work 'batch.log')
	$r = Read-Host 'Start now? [y/N]'
	if ($r -notmatch '^(y|yes)$') { Say 'Nothing done.'; return }
	& $py (Join-Path $tools 'batch_outpaint.py')
}
$out = Join-Path $InstallDir 'video-sides'
Say 'Packing the widened sides for the game...'
& $py (Join-Path $tools 'pack_sides.py') $out
$n = @(Get-ChildItem $out -Filter '*.sides' -ErrorAction SilentlyContinue).Count
if ($n -gt 0) {
	$lines = @(Get-Content $ini | Where-Object { $_ -notmatch '^remaster_video_sides_path=' })
	$i = [array]::IndexOf($lines, '[comi]')
	if ($i -ge 0) {
		$lines = @($lines[0..$i]) + "remaster_video_sides_path=$out" + @($lines[($i + 1)..($lines.Count - 1)])
		$lines | Set-Content $ini -Encoding ascii
	}
	Say "Widened cutscenes ready for $n videos. F12 in game switches them on and off. The working files in $work can be deleted."
}
