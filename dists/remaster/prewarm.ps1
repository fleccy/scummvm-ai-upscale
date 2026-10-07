<#
.SYNOPSIS
  Prepares the HD backgrounds of every room once, so rooms show them from the first frame (like a shader cache),
  and, if the optional TensorRT upscaler is installed, builds its engines for this graphics card.

.DESCRIPTION
  Starts the game minimized in each room in turn with remaster_bg_prewarm=true: the game upscales that room's
  background with the HD model, saves it to the hd-cache folder and quits by itself. Takes a few minutes; the
  game can be played meanwhile only after it has finished. Rooms you visit later are cached anyway, so this
  is optional. Run again any time (already cached rooms are quick).
#>
[CmdletBinding()]
param(
	[string]$InstallDir = $PSScriptRoot,
	[int[]]$Rooms = @(1..95),
	[int]$TimeoutSec = 45
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $InstallDir 'scummvm.exe'
$ini = Join-Path $InstallDir 'scummvm.ini'
if (-not (Test-Path $exe) -or -not (Test-Path $ini)) { throw "No installation found in $InstallDir (run install.cmd first)." }
if (Get-Process scummvm -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) { throw 'Close the game first.' }

# A private copy of the settings: windowed, muted, prewarm mode, own save folder (nothing of yours is touched).
$work = Join-Path $env:TEMP 'scummvm-ai-upscale-prewarm'
New-Item -ItemType Directory -Force $work | Out-Null
$lines = Get-Content $ini | Where-Object { $_ -notmatch '^(fullscreen|mute|savepath|remaster_bg_prewarm)=' }
$i = [array]::IndexOf($lines, '[comi]')
if ($i -lt 0) { throw "scummvm.ini has no [comi] section." }
$lines = @($lines[0..$i]) + 'remaster_bg_prewarm=true' + 'fullscreen=false' + 'mute=true' + "savepath=$work" + @($lines[($i + 1)..($lines.Count - 1)])
$tmpIni = Join-Path $work 'prewarm.ini'
$lines | Set-Content $tmpIni -Encoding ascii

# TensorRT upscaler (if installed): build its engines for this card once (a few minutes the first time)
if (Test-Path (Join-Path $InstallDir 'tensorrt\nvinfer_10.dll')) {
	Write-Progress -Activity 'Preparing the TensorRT upscaler for your graphics card' -Status 'a few minutes the first time'
	$trtLines = $lines | Where-Object { $_ -notmatch '^(remaster_ai_backend|remaster_trt_prewarm|remaster_bg_prewarm)=' }
	$j = [array]::IndexOf($trtLines, '[comi]')
	$trtLines = @($trtLines[0..$j]) + 'remaster_ai_backend=trt-ultra' + 'remaster_trt_prewarm=true' + @($trtLines[($j + 1)..($trtLines.Count - 1)])
	$trtIni = Join-Path $work 'prewarm-trt.ini'
	$trtLines | Set-Content $trtIni -Encoding ascii
	$log = Join-Path $work 'tensorrt.log'
	$p = Start-Process $exe -PassThru -WindowStyle Minimized -WorkingDirectory $work -ArgumentList "--config=`"$trtIni`"", "--logfile=`"$log`"", '--window-size=640,360', '--boot-param=15', 'comi'
	if (-not $p.WaitForExit(900 * 1000)) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue }
	Write-Progress -Activity 'Preparing the TensorRT upscaler for your graphics card' -Completed
	if (Select-String -Path $log -Pattern 'TensorRT prewarm complete' -Quiet -ErrorAction SilentlyContinue) {
		Write-Host '[remaster] TensorRT upscaler prepared for your graphics card.'
	} else {
		Write-Host '[remaster] TensorRT upscaler not prepared now; it prepares itself the first time you choose it.'
	}
}

$n = 0
$ok = 0
foreach ($r in $Rooms) {
	$n++
	Write-Progress -Activity 'Preparing HD backgrounds' -Status "Room $r ($n of $($Rooms.Count))" -PercentComplete ([int](100 * ($n - 1) / $Rooms.Count))
	$log = Join-Path $work "room$r.log"
	$p = Start-Process $exe -PassThru -WindowStyle Minimized -WorkingDirectory $work -ArgumentList "--config=`"$tmpIni`"", "--logfile=`"$log`"", '--window-size=640,360', "--boot-param=$r", 'comi'
	if (-not $p.WaitForExit($TimeoutSec * 1000)) {
		Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
		continue
	}
	if (Select-String -Path $log -Pattern 'prewarm of room \d+ complete' -Quiet -ErrorAction SilentlyContinue) { $ok++ }
}
Write-Progress -Activity 'Preparing HD backgrounds' -Completed
Write-Host "[remaster] HD backgrounds prepared for $ok rooms."
