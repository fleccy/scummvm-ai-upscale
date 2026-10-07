<#
.SYNOPSIS
  Generates widescreen side art for the narrow rooms from YOUR OWN copy of The Curse of Monkey Island.

.DESCRIPTION
  scummvm-ai-upscale distributes nothing derived from the game's art. This script makes the side art on your PC:
  the game saves each narrow room's background, an AI model continues it outwards to 16:9, and the result is
  aligned and blended in (sides-tools\generate.py). You choose the model:
    cloud  Google Gemini through your own OpenRouter account (https://openrouter.ai/keys). Best results; about
           7 US cents per room (about 4 USD for all 57); the room backgrounds are sent to OpenRouter/Google.
           Downloads about 150 MB of tools (about 250 MB installed).
    local  Stable Diffusion XL on your NVIDIA graphics card (8 GB or more). Free and offline after the download
           (about 4 GB of tools plus a 7 GB model); simpler results.
  Everything it downloads goes into the install folder's tools\ folder (delete that folder to free the space):
  uv (Astral's Python tool, checked against its SHA-256), a private Python and the Python packages it needs.
  Nothing is installed system-wide. Run it again any time: rooms already done are kept (delete a room's
  sides\NNNN.png to redo it).

.EXAMPLE
  .\generate_sides.ps1
  .\generate_sides.ps1 -Method local
#>
[CmdletBinding()]
param(
	[string]$InstallDir = $PSScriptRoot,
	[ValidateSet('ask', 'cloud', 'local')][string]$Method = 'ask',
	[string]$Rooms = ''
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { Write-Host "[side art] $m" }
$tools = Join-Path $InstallDir 'tools'
$gen = Join-Path $InstallDir 'sides-tools\generate.py'
$ini = Join-Path $InstallDir 'scummvm.ini'
if (-not (Test-Path $gen) -or -not (Test-Path $ini)) { throw "No installation found in $InstallDir (run install.cmd first)." }
if (Get-Process scummvm -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$InstallDir*" }) { throw 'Close the game first.' }

if ($Method -eq 'ask') {
	Write-Host ''
	Write-Host 'Widescreen side art fills the sides of the narrow rooms. It is made now, on this PC, from your copy of the game.'
	Write-Host '  1  cloud: Google Gemini through your own OpenRouter account (openrouter.ai). Best results, about'
	Write-Host '     7 US cents per room (about 4 USD for all rooms). The room backgrounds are sent to OpenRouter/Google.'
	Write-Host '  2  local: Stable Diffusion XL on your NVIDIA graphics card. Free and offline; about 10 GB of downloads'
	Write-Host '     (tools and model); simpler results.'
	$r = Read-Host 'Choose 1 or 2 (Enter = 1)'
	$Method = if ($r -eq '2') { 'local' } else { 'cloud' }
}
if ($Method -eq 'local' -and -not (Get-Command nvidia-smi -ErrorAction SilentlyContinue)) {
	throw 'The local method needs an NVIDIA graphics card (nvidia-smi was not found). Use the cloud method instead.'
}

# ── uv (Astral), checked against its SHA-256 ─────────────────────────────────────────────────────────────
$uvVer = '0.12.23'
$uvSha = '75d05de6762778c31ee183398de7dd15093fad0ed90b1f236d8205ea5ec00c90'
$uv = Join-Path $tools 'uv\uv.exe'
if (-not (Test-Path $uv)) {
	New-Item -ItemType Directory -Force (Join-Path $tools 'uv') | Out-Null
	$zip = Join-Path $env:TEMP "uv-$uvVer.zip"
	Say "Downloading uv $uvVer (Python tool, 18 MB)..."
	$ProgressPreference = 'SilentlyContinue'
	Invoke-WebRequest "https://github.com/astral-sh/uv/releases/download/$uvVer/uv-x86_64-pc-windows-msvc.zip" -OutFile $zip -UseBasicParsing
	if ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $uvSha) { Remove-Item $zip; throw 'uv download failed its checksum; nothing installed.' }
	Expand-Archive $zip (Join-Path $tools 'uv') -Force
	Remove-Item $zip
}
# everything uv fetches stays in tools\
$env:UV_CACHE_DIR = Join-Path $tools 'uv-cache'
$env:UV_PYTHON_INSTALL_DIR = Join-Path $tools 'python'
$env:UV_PYTHON_PREFERENCE = 'only-managed'
$env:HF_HOME = Join-Path $tools 'hf'
$venv = Join-Path $tools 'sides-venv'
$py = Join-Path $venv 'Scripts\python.exe'
if (-not (Test-Path $py)) {
	Say 'Setting up a private Python...'
	& $uv venv $venv --python 3.12 --quiet
	if ($LASTEXITCODE) { throw 'Could not set up Python.' }
}
Say 'Installing the image tools...'
& $uv pip install --python $py --quiet numpy pillow opencv-python-headless
if ($LASTEXITCODE) { throw 'Could not install the image tools.' }
if ($Method -eq 'local') {
	Say 'Installing PyTorch (CUDA) and diffusers, about 3 GB the first time...'
	& $uv pip install --python $py --quiet torch --index-url https://download.pytorch.org/whl/cu128
	if ($LASTEXITCODE) { throw 'Could not install PyTorch.' }
	& $uv pip install --python $py --quiet diffusers transformers accelerate safetensors
	if ($LASTEXITCODE) { throw 'Could not install diffusers.' }
}

& $uv cache clean --quiet 2>$null   # the downloaded packages are installed; their cache is not needed

# ── generate ──────────────────────────────────────────────────────────────────────────────────────────────
$args2 = @($gen, '--install', $InstallDir, '--method', $Method)
if ($Rooms) { $args2 += @('--rooms', $Rooms) }
& $py @args2
$sides = Join-Path $InstallDir 'sides'
$n = @(Get-ChildItem $sides -Filter '*.png' -ErrorAction SilentlyContinue).Count
if ($n -gt 0) {
	# switch it on in the settings
	$lines = @(Get-Content $ini | Where-Object { $_ -notmatch '^(remaster_sides_path|remaster_side_art)=' })
	$i = [array]::IndexOf($lines, '[comi]')
	if ($i -ge 0) {
		$lines = @($lines[0..$i]) + "remaster_sides_path=$sides" + 'remaster_side_art=true' + @($lines[($i + 1)..($lines.Count - 1)])
		$lines | Set-Content $ini -Encoding ascii
	}
	Say "Side art ready for $n rooms. F12 in game switches between side art, ambient glow and black bars."
}
