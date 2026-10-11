<#
.SYNOPSIS
  The windowed installer for scummvm-ai-upscale (install.cmd opens it when double-clicked).

.DESCRIPTION
  Asks the same questions as install.ps1 in a window, then runs install.ps1 with the answers (and, if chosen,
  generate_sides.ps1 afterwards) and shows its output. It installs nothing itself: install.ps1 does all the work,
  and stays the text version for unattended use ("install.cmd /console", or install.ps1 with parameters).
  Exit codes: 0 installed (or closed), 1 the install failed or was stopped, 10 the window could not be opened
  (install.cmd then falls back to the text version).

.EXAMPLE
  .\install-gui.ps1
  .\install-gui.ps1 -InstallDir D:\Games\COMI-AI -NoShortcuts
#>
[CmdletBinding()]
param(
	[string]$InstallDir,                 # pre-filled install folder
	[string]$GamePath,                   # pre-filled game folder (otherwise found like install.ps1 does)
	[string]$DigPath,                    # pre-filled folder of The Dig (otherwise found like install.ps1 does)
	[string]$AtlantisPath,               # pre-filled folder of Indiana Jones and the Fate of Atlantis
	[string]$Indy3Path,                  # pre-filled folder of Indiana Jones and the Last Crusade
	[string]$LoomPath,                   # pre-filled folder of Loom (VGA CD talkie)
	[string]$SamnmaxPath,                # pre-filled folder of Sam & Max Hit the Road (CD)
	[string]$Games = '',                 # pre-ticked games: comi, dig, atlantis, indy3, loom, samnmax or a list (default: the ones found)
	[switch]$NoShortcuts,                # pre-clears "create shortcuts"
	[ValidateSet('', 'original', 'enhanced', 'experimental', 'keep')][string]$Preset = '',
	[ValidateSet('', 'yes', 'no')][string]$Prewarm = '',
	[switch]$AutoTest,                   # testing: clicks through every page with the pre-filled answers
	[string]$ScreenshotDir,              # testing: with -AutoTest, saves a picture of every page here
	[double]$UiScale = 0                 # testing: force a DPI scale (1.5 = 150 %)
)
$ErrorActionPreference = 'Stop'
$pkg = Split-Path -Parent $MyInvocation.MyCommand.Path
$installPs1 = Join-Path $pkg 'install.ps1'
$shown = $false

try {
	Add-Type -AssemblyName System.Windows.Forms, System.Drawing
} catch {
	exit 10
}
try {
	Add-Type -Namespace RemasterInstaller -Name Native -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
	[void][RemasterInstaller.Native]::SetProcessDPIAware()
} catch { }   # not DPI aware: Windows scales the window up (a little blurry), everything still works

[System.Windows.Forms.Application]::EnableVisualStyles()

$State = @{
	Game = $null; Extra = [ordered]@{}
	Proc = $null; Out = $null; Err = $null; OutTask = $null; ErrTask = $null; Step = ''; Steps = @(); Failed = $false
	Start = [datetime]::MinValue; Ticks = 0; KeepFor = $null; Existing = $false; Phase = ''; Downloads = 0; SideArtResult = $null; Cancelled = $false; Log = New-Object System.Text.StringBuilder
}

# -- Helpers ---------------------------------------------------------------------------------------------
function Quote-Arg([string]$a) {
	# for CommandLineToArgvW: a trailing backslash would escape the closing quote
	if ($a -match '\\$') { $a += '\' }
	return '"' + $a + '"'
}

function New-ChildProcess([string]$script, [string[]]$argList, [hashtable]$envVars) {
	$psi = New-Object System.Diagnostics.ProcessStartInfo
	$psi.FileName = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
	$psi.Arguments = (@('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Quote-Arg $script)) + $argList) -join ' '
	$psi.UseShellExecute = $false
	$psi.RedirectStandardOutput = $true
	$psi.RedirectStandardError = $true
	$psi.RedirectStandardInput = $true    # closed right away: nothing can wait for typed input
	$psi.StandardOutputEncoding = [Console]::OutputEncoding
	$psi.StandardErrorEncoding = [Console]::OutputEncoding
	$psi.WorkingDirectory = $pkg
	# Started from PowerShell 7, Windows PowerShell 5.1 would inherit its module folders and miss its own commands
	# (Get-FileHash, Expand-Archive): let it use its own (like install.cmd).
	$psi.EnvironmentVariables.Remove('PSModulePath')
	if ($envVars) { foreach ($k in $envVars.Keys) { $psi.EnvironmentVariables[$k] = $envVars[$k] } }
	$p = [System.Diagnostics.Process]::Start($psi)
	$p.StandardInput.Close()
	return $p
}

# The games the window offers: install.ps1 knows them by Id (-Games), its -<Param> takes the folder, and -FindGame
# prints "<Key>=<folder>" for each copy found.
$GameDefs = @(
	@{ Id = 'comi'; Key = 'game'; Param = 'GamePath'; Path = $GamePath; Title = 'The Curse of Monkey Island'; Check = 'The Curse of Monkey Island'
	   Browse = 'Select your The Curse of Monkey Island folder (it contains COMI.LA0)'
	   Missing = 'No Curse of Monkey Island game data (COMI.LA0 and a RESOURCE folder)' },
	@{ Id = 'dig'; Key = 'dig'; Param = 'DigPath'; Path = $DigPath; Title = 'The Dig'; Check = 'The Dig (experimental)'
	   Browse = 'Select your The Dig folder (it contains The Dig.exe and a DIG folder, or DIG.LA0)'
	   Missing = 'No The Dig game data (The Dig.exe with a DIG folder, or DIG.LA0 and DIG.LA1)' },
	@{ Id = 'atlantis'; Key = 'atlantis'; Param = 'AtlantisPath'; Path = $AtlantisPath; Title = 'Indiana Jones and the Fate of Atlantis'
	   Check = 'Indiana Jones and the Fate of Atlantis (experimental)'
	   Browse = 'Select your Indiana Jones and the Fate of Atlantis folder (it contains its .exe and an ATLANTIS folder, or ATLANTIS.000)'
	   Missing = 'No Fate of Atlantis game data (Indiana Jones and the Fate of Atlantis.exe with an ATLANTIS folder, or ATLANTIS.000 and ATLANTIS.001)' },
	@{ Id = 'indy3'; Key = 'indy3'; Param = 'Indy3Path'; Path = $Indy3Path; Title = 'Indiana Jones and the Last Crusade'
	   Check = 'Indiana Jones and the Last Crusade (experimental)'
	   Browse = 'Select your Indiana Jones and the Last Crusade folder (it contains its .exe and an INDY3 folder, or 00.LFL)'
	   Missing = 'No Last Crusade game data (Indiana Jones and the Last Crusade.exe with an INDY3 folder, or 00.LFL and 01.LFL)' },
	@{ Id = 'loom'; Key = 'loom'; Param = 'LoomPath'; Path = $LoomPath; Title = 'Loom'; Check = 'Loom, VGA CD version (experimental)'
	   Browse = 'Select your Loom folder (the VGA CD version: it contains 000.LFL and DISK01.LEC, or Loom.exe)'
	   Missing = 'No Loom VGA CD game data (000.LFL and DISK01.LEC, or Loom.exe and DISK01.LEC)' },
	@{ Id = 'samnmax'; Key = 'samnmax'; Param = 'SamnmaxPath'; Path = $SamnmaxPath; Title = 'Sam & Max Hit the Road'
	   Check = 'Sam && Max Hit the Road, CD version (experimental)'
	   Browse = 'Select your Sam & Max Hit the Road folder (the CD version: it contains SAMNMAX.000, or a SAMNMAX folder)'
	   Missing = 'No Sam & Max game data (SAMNMAX.000 and SAMNMAX.001)' }
)

function Join-Names([string[]]$names) {   # "A", "A and B", "A, B and C"
	if ($names.Count -le 1) { return ($names -join '') }
	return ($names[0..($names.Count - 2)] -join ', ') + ' and ' + $names[-1]
}

function Find-Game([hashtable]$paths, [string]$games) {
	# install.ps1 decides what a game folder is (Steam, GOG, or a folder you chose). Returns Id -> @{ Path; Store }.
	$a = @('-FindGame')
	if ($games) { $a += @('-Games', $games) }
	foreach ($gd in $GameDefs) { if ($paths[$gd.Id]) { $a += @("-$($gd.Param)", (Quote-Arg $paths[$gd.Id])) } }
	$p = New-ChildProcess $installPs1 $a $null
	$out = $p.StandardOutput.ReadToEnd()
	$p.WaitForExit()
	$r = @{}
	foreach ($gd in $GameDefs) { $r[$gd.Id] = @{ Path = $null; Store = $null } }
	foreach ($l in ($out -split "`r?`n")) {
		foreach ($gd in $GameDefs) {
			if ($l -match "^$($gd.Key)=(.+)$") { $r[$gd.Id].Path = $Matches[1].Trim() }
			if ($gd.Id -eq 'comi') { if ($l -match 'Found (Steam|GOG) copy') { $r.comi.Store = $Matches[1] } }
			elseif ($l -match ('Found ' + [regex]::Escape($gd.Title) + ' \((Steam|GOG) copy\)')) { $r[$gd.Id].Store = $Matches[1] }
		}
	}
	return $r
}

$script:DpiScale = if ($UiScale -gt 0) { $UiScale } else {
	$g = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $d = $g.DpiX / 96.0; $g.Dispose(); $d
}
function P([double]$v) { return [int][math]::Round($v * $script:DpiScale) }

$FontName = 'Segoe UI'
$fBody = New-Object System.Drawing.Font($FontName, 10)
$fSmall = New-Object System.Drawing.Font($FontName, 9)
$fTitle = New-Object System.Drawing.Font('Segoe UI Semibold', 16)
$fCard = New-Object System.Drawing.Font('Segoe UI Semibold', 11.5)
$fBold = New-Object System.Drawing.Font('Segoe UI Semibold', 10)
$fMono = New-Object System.Drawing.Font('Consolas', 9)
$cAccent = [System.Drawing.Color]::FromArgb(0, 103, 192)
$cText = [System.Drawing.Color]::FromArgb(32, 32, 32)
$cMuted = [System.Drawing.Color]::FromArgb(96, 96, 96)
$cSide = [System.Drawing.Color]::FromArgb(243, 243, 243)
$cLine = [System.Drawing.Color]::FromArgb(218, 218, 218)
$cSel = [System.Drawing.Color]::FromArgb(232, 241, 251)
$cError = [System.Drawing.Color]::FromArgb(180, 30, 30)
$cOk = [System.Drawing.Color]::FromArgb(16, 124, 16)

function Measure-Height([string]$text, $font, [int]$w) {
	$flags = [System.Windows.Forms.TextFormatFlags]::WordBreak -bor [System.Windows.Forms.TextFormatFlags]::NoPrefix
	return ([System.Windows.Forms.TextRenderer]::MeasureText($text, $font, (New-Object System.Drawing.Size(($w - 2), 0)), $flags)).Height
}

function Add-Text($parent, [string]$text, [int]$x, [int]$y, [int]$w, $font = $fBody, $color = $cText) {
	$l = New-Object System.Windows.Forms.Label
	$l.Font = $font; $l.ForeColor = $color; $l.UseMnemonic = $false; $l.AutoSize = $false; $l.Text = $text
	$l.BackColor = [System.Drawing.Color]::Transparent
	$l.SetBounds($x, $y, $w, (Measure-Height $text $font $w) + (P 2))
	$parent.Controls.Add($l)
	return $l
}
function Set-Text($label, [string]$text) {
	$label.Text = $text
	$label.Height = (Measure-Height $text $label.Font $label.Width) + (P 2)
}

function Add-Button($parent, [string]$text, [int]$x, [int]$y, [int]$w = (P 100)) {
	$b = New-Object System.Windows.Forms.Button
	$b.Text = $text; $b.Font = $fBody; $b.SetBounds($x, $y, $w, (P 32)); $b.FlatStyle = 'System'
	$parent.Controls.Add($b)
	return $b
}

# -- Window ----------------------------------------------------------------------------------------------
$W = P 840; $H = P 630; $sideW = P 196; $barH = P 60
$form = New-Object System.Windows.Forms.Form
$form.Text = 'scummvm-ai-upscale setup (The Curse of Monkey Island, The Dig, Indiana Jones, Loom, Sam & Max)'
$form.AutoScaleMode = 'None'
$form.ClientSize = New-Object System.Drawing.Size($W, $H)
$form.FormBorderStyle = 'FixedSingle'; $form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.BackColor = [System.Drawing.Color]::White
$form.Font = $fBody
try { $form.Icon = [System.Drawing.Icon]::ExtractAssociatedIcon((Join-Path $pkg 'scummvm.exe')) } catch { }

$side = New-Object System.Windows.Forms.Panel
$side.SetBounds(0, 0, $sideW, $H - $barH); $side.BackColor = $cSide
$form.Controls.Add($side)
[void](Add-Text $side 'scummvm-ai-upscale' (P 20) (P 22) ($sideW - (P 30)) $fBold $cText)
[void](Add-Text $side 'Setup' (P 20) (P 42) ($sideW - (P 30)) $fSmall $cMuted)
$stepNames = 'Welcome', 'Your games', 'Install folder', 'How to play', 'Extras', 'Installing', 'Done'
$stepLabels = @()
for ($i = 0; $i -lt $stepNames.Count; $i++) {
	$stepLabels += Add-Text $side $stepNames[$i] (P 20) (P (90 + 32 * $i)) ($sideW - (P 30)) $fBody $cMuted
}

$bar = New-Object System.Windows.Forms.Panel
$bar.SetBounds(0, $H - $barH, $W, $barH); $bar.BackColor = $cSide
$form.Controls.Add($bar)
$sep = New-Object System.Windows.Forms.Label
$sep.SetBounds(0, 0, $W, 1); $sep.BackColor = $cLine
$bar.Controls.Add($sep)
$by = [int](($barH - (P 32)) / 2)
$btnCancel = Add-Button $bar 'Cancel' ($W - (P 120)) $by
$btnNext = Add-Button $bar 'Next >' ($W - (P 236)) $by
$btnBack = Add-Button $bar '< Back' ($W - (P 344)) $by
$btnPlay = Add-Button $bar 'Play now' ($W - (P 380)) $by (P 120)
$btnFolder = Add-Button $bar 'Open install folder' ($W - (P 252)) $by (P 124)
$btnPlay.Visible = $false; $btnFolder.Visible = $false
$form.AcceptButton = $btnNext

$cx = $sideW + (P 32); $cw = $W - $cx - (P 32)   # content column
$pages = @()
function New-Page([string]$title, [string]$subtitle) {
	$p = New-Object System.Windows.Forms.Panel
	$p.SetBounds($sideW, 0, $W - $sideW, $H - $barH); $p.Visible = $false; $p.AutoScroll = $true
	$p.BackColor = [System.Drawing.Color]::White
	$form.Controls.Add($p)
	$t = Add-Text $p $title (P 32) (P 24) $cw $fTitle $cText
	$y = $t.Bottom + (P 6)
	if ($subtitle) { $y = (Add-Text $p $subtitle (P 32) $y $cw $fBody $cMuted).Bottom + (P 6) }
	$p.Tag = $y + (P 10)    # where the page content starts
	$script:pages += $p
	return $p
}
$px = P 32   # left edge inside a page

# -- Page 0: welcome -------------------------------------------------------------------------------------
$sizeMB = [math]::Round(((Get-ChildItem $pkg -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1MB) / 10) * 10
$pg = New-Page 'Welcome' ''
$y = $pg.Tag
$y = (Add-Text $pg ('This sets up The Curse of Monkey Island with real-time AI upscaling: an unofficial, free and open-source ' +
	'version of ScummVM that redraws the game sharp at your screen''s resolution, in 16:9, with the original look ' +
	'one key away. It can also set up The Dig, Indiana Jones and the Fate of Atlantis, Indiana Jones and the Last ' +
	'Crusade, Loom and Sam & Max Hit the Road (experimental).') $px $y $cw).Bottom + (P 14)
$y = (Add-Text $pg 'You need your own copy of the game' $px $y $cw $fBold).Bottom + (P 2)
$y = (Add-Text $pg ('The Curse of Monkey Island (or one of the others) from Steam or GOG. Nothing from the game is included in this download or ' +
	'distributed by this project: the installer only reads your game files where they are, and never changes them.') $px $y $cw).Bottom + (P 14)
$y = (Add-Text $pg 'What happens' $px $y $cw $fBold).Bottom + (P 2)
$y = (Add-Text $pg ("The next pages ask where your game is, where to install (about $sizeMB MB), how you want to play and " +
	'which optional extras you want. Nothing is installed until you click Install. Run this again any time to ' +
	'update: your settings and saved games are kept.') $px $y $cw).Bottom + (P 14)
[void](Add-Text $pg ('Your PC needs 64-bit Windows 10 or 11 and a graphics card with an up-to-date driver (any recent ' +
	'NVIDIA, AMD or Intel). Without one the game still runs, just without AI upscaling.') $px $y $cw $fSmall $cMuted)

# -- Page 1: game location -------------------------------------------------------------------------------
$pg = New-Page 'Where are your games?' 'Tick the games to set up. Your copies are only read, never copied or changed.'
$y = $pg.Tag
$gx = $px + (P 22); $gw = $cw - (P 22)   # indented under each game's checkbox
foreach ($gd in $GameDefs) {
	$cb = New-Object System.Windows.Forms.CheckBox
	$cb.Text = $gd.Check; $cb.Font = $fCard; $cb.SetBounds($px, $y, $cw, (P 26)); $cb.Checked = $gd.Id -eq 'comi'
	$pg.Controls.Add($cb)
	$y += P 27
	$gd.Status = Add-Text $pg $(if ($gd.Id -eq 'comi') { 'Looking for your game...' } else { "Looking for $($gd.Title)..." }) $gx $y $gw $fBold
	$y = $gd.Status.Bottom + (P 3)
	$tb = New-Object System.Windows.Forms.TextBox
	$tb.Font = $fBody; $tb.SetBounds($gx, $y + (P 3), $gw - (P 112), (P 26))
	$pg.Controls.Add($tb)
	$btn = Add-Button $pg 'Browse...' ($gx + $gw - (P 100)) $y
	$y += P 42
	$gd.CheckBox = $cb; $gd.TextBox = $tb; $gd.BrowseButton = $btn
	$cb.Tag = $gd; $btn.Tag = $gd
	$cb.add_CheckedChanged({ param($sender, $e) $sender.Tag.TextBox.Enabled = $sender.Checked; $sender.Tag.BrowseButton.Enabled = $sender.Checked })
	$btn.add_Click({
		param($sender, $e)
		$g = $sender.Tag
		$dlg = New-Object System.Windows.Forms.FolderBrowserDialog
		$dlg.Description = $g.Browse
		if ($g.TextBox.Text -and (Test-Path $g.TextBox.Text)) { $dlg.SelectedPath = $g.TextBox.Text }
		if ($dlg.ShowDialog($form) -eq 'OK') {
			$g.TextBox.Text = $dlg.SelectedPath; $g.CheckBox.Checked = $true
			Set-Text $g.Status 'Folder chosen (checked when you click Next).'; $g.Status.ForeColor = $cText
		}
	})
}
$y += P 2
$y = (Add-Text $pg ('The others are experimental: AI upscaling, HD backgrounds and 16:9 work; ' +
	'Monkey Island''s room-by-room extras are not made for them.') $px $y $cw $fSmall $cMuted).Bottom + (P 6)
[void](Add-Text $pg ('Not found? Choose the folder you installed the game to (Steam: right-click the game > Manage > Browse ' +
	'local files).') $px $y $cw $fSmall $cMuted)

# -- Page 2: install folder ------------------------------------------------------------------------------
$pg = New-Page 'Where should it be installed?' "The program, your settings and your saved games go here (about $sizeMB MB)."
$y = $pg.Tag
$txtDir = New-Object System.Windows.Forms.TextBox
$txtDir.Font = $fBody; $txtDir.SetBounds($px, $y + (P 3), $cw - (P 112), (P 26))
$pg.Controls.Add($txtDir)
$btnDirBrowse = Add-Button $pg 'Browse...' ($px + $cw - (P 100)) $y
$y += P 46
$lblDirNote = Add-Text $pg ' ' $px $y $cw $fBody $cMuted
$y += P 58
$chkShortcuts = New-Object System.Windows.Forms.CheckBox
$chkShortcuts.Text = 'Create shortcuts on the desktop and in the Start menu'
$chkShortcuts.Font = $fBody; $chkShortcuts.SetBounds($px, $y, $cw, (P 26)); $chkShortcuts.Checked = -not $NoShortcuts
$pg.Controls.Add($chkShortcuts)
$y += P 28
[void](Add-Text $pg '(The Start menu also gets a version that starts in a window. Alt+Enter switches any time.)' ($px + (P 20)) $y ($cw - (P 20)) $fSmall $cMuted)

# -- Page 3: preset --------------------------------------------------------------------------------------
$pg = New-Page 'How do you want to play?' ('This is only a starting point: everything can be changed in game, and F10 switches between ' +
	'the original look and the upscaled one at any time.')
$y = $pg.Tag - (P 4)
$chkKeep = New-Object System.Windows.Forms.CheckBox
$chkKeep.Text = 'Keep my current settings (only update the program)'
$chkKeep.Font = $fBold; $chkKeep.SetBounds($px, $y, $cw, (P 26)); $chkKeep.Visible = $false
$pg.Controls.Add($chkKeep)
$presetTop = $y
$Cards = [ordered]@{
	original     = @('Original', 'No AI', ('The game as it looked in 1997 (original pixels, 4:3), with the extras that need no AI: ' +
		'controller support, photo mode, VGA/EGA/Amiga looks and CRT scanlines (F10, Ctrl+F10). Also the best choice ' +
		'for older or low-power PCs.'))
	enhanced     = @('Enhanced', 'Recommended', ('The game redrawn sharp at your screen''s resolution by AI on your graphics ' +
		'card, filling a modern 16:9 screen: wide rooms show more of the room, narrow ones get a soft glow at the sides. ' +
		'Characters and backgrounds get extra detail.'))
	experimental = @('Experimental', 'Work in progress', ('Enhanced, plus the work-in-progress extras: painted scenery beside ' +
		'the narrow rooms and full-width cutscenes, if you make or generate them (offered next), and smoother camera movement.'))
}
$cardPanels = @{}
$cardRadios = @{}
$State.Preset = 'enhanced'
function Update-Cards {
	foreach ($k in $cardPanels.Keys) {
		$sel = $cardRadios[$k].Checked
		$cardPanels[$k].BackColor = if (-not $cardPanels[$k].Enabled) { [System.Drawing.Color]::White } elseif ($sel) { $cSel } else { [System.Drawing.Color]::White }
		$cardPanels[$k].Invalidate()
	}
}
$cardClick = {
	param($sender, $e)
	$c = $sender; while ($c -and -not ($c.Tag -is [string] -and $cardRadios.ContainsKey($c.Tag))) { $c = $c.Parent }
	if ($c -and $c.Enabled) { $cardRadios[$c.Tag].Checked = $true }
}
$cardPaint = {
	param($sender, $e)
	$sel = $cardRadios[$sender.Tag].Checked -and $sender.Enabled
	$pen = New-Object System.Drawing.Pen($(if ($sel) { $cAccent } else { $cLine }), $(if ($sel) { [float](P 2) } else { [float]1 }))
	$off = [int][math]::Floor($pen.Width / 2)
	$e.Graphics.DrawRectangle($pen, $off, $off, $sender.ClientSize.Width - 1 - 2 * $off + ($(if ($sel) { 1 } else { 0 })), $sender.ClientSize.Height - 1 - 2 * $off + ($(if ($sel) { 1 } else { 0 })))
	$pen.Dispose()
}
$cardTop = $y + (P 34)
function Layout-Cards([int]$top) {
	$yy = $top
	foreach ($k in $Cards.Keys) { $cardPanels[$k].Top = $yy; $yy = $cardPanels[$k].Bottom + (P 10) }
}
foreach ($k in $Cards.Keys) {
	$c = $Cards[$k]
	$card = New-Object System.Windows.Forms.Panel
	$card.Tag = $k; $card.Cursor = 'Hand'; $card.BackColor = [System.Drawing.Color]::White
	$pg.Controls.Add($card)
	$rb = New-Object System.Windows.Forms.RadioButton
	$rb.Text = $c[0]; $rb.Font = $fCard; $rb.AutoSize = $true; $rb.Location = New-Object System.Drawing.Point((P 14), (P 10))
	$rb.BackColor = [System.Drawing.Color]::Transparent; $rb.Tag = $k
	$card.Controls.Add($rb)
	$tag = Add-Text $card $c[1] (P 200) (P 15) ($cw - (P 214)) $fSmall $(if ($k -eq 'enhanced') { $cAccent } else { $cMuted })
	$tag.TextAlign = 'TopRight'
	$desc = Add-Text $card $c[2] (P 34) (P 40) ($cw - (P 52)) $fBody $cText
	$card.SetBounds($px, 0, $cw, $desc.Bottom + (P 12))
	foreach ($ctl in @($card, $tag, $desc)) { $ctl.add_Click($cardClick) }
	$card.add_Paint($cardPaint)
	$rb.add_CheckedChanged({
		param($sender, $e)
		if ($sender.Checked) {   # each card is its own container: keep the three choices exclusive by hand
			$State.Preset = $sender.Tag
			foreach ($o in $cardRadios.Values) { if ($o -ne $sender -and $o.Checked) { $o.Checked = $false } }
		}
		Update-Cards
	})
	$cardPanels[$k] = $card; $cardRadios[$k] = $rb
}
Layout-Cards $presetTop
$chkKeep.add_CheckedChanged({
	foreach ($k in $cardPanels.Keys) { $cardPanels[$k].Enabled = -not $chkKeep.Checked }
	Update-Cards
})

# -- Page 4: extras --------------------------------------------------------------------------------------
$pg = New-Page 'Optional extras' 'All of these can also be done later. If unsure, keep the defaults.'
$pgExtras = $pg
$y = $pg.Tag
# side art
$lblSideHead = Add-Text $pg 'Painted scenery beside the narrow rooms (side art)' $px $y $cw $fBold
$y = $lblSideHead.Bottom + (P 2)
$lblSideInfo = Add-Text $pg ('Made now on this PC from your copy of the game by an AI model you choose (nothing like it is ' +
	'distributed), for all 55 narrow rooms in one go. Without it they show a soft glow at the sides; F12 in game switches. ' +
	'Later: generate_sides.cmd in the install folder.') $px $y $cw $fSmall $cMuted
$y = $lblSideInfo.Bottom + (P 6)
$rbSideNone = New-Object System.Windows.Forms.RadioButton
$rbSideNone.Text = 'Not now'; $rbSideNone.Font = $fBody; $rbSideNone.SetBounds($px, $y, $cw, (P 24)); $rbSideNone.Checked = $true
$pg.Controls.Add($rbSideNone)
$y += P 26
$rbSideCloud = New-Object System.Windows.Forms.RadioButton
$rbSideCloud.Text = 'Cloud: your own OpenRouter account (best results, about 4 US dollars, a few minutes)'
$rbSideCloud.Font = $fBody; $rbSideCloud.SetBounds($px, $y, $cw, (P 24))
$pg.Controls.Add($rbSideCloud)
$y += P 25
$lblCloudInfo = Add-Text $pg ('The room pictures are sent to OpenRouter and Google (Gemini). Downloads about 150 MB of tools. ' +
	'Get a key at openrouter.ai/keys; it is used for this run only and not saved.') ($px + (P 20)) $y ($cw - (P 20)) $fSmall $cMuted
$y = $lblCloudInfo.Bottom + (P 4)
$lblKey = Add-Text $pg 'OpenRouter key:' ($px + (P 20)) ($y + (P 4)) (P 112) $fSmall $cText
$txtKey = New-Object System.Windows.Forms.TextBox
$txtKey.Font = $fSmall; $txtKey.UseSystemPasswordChar = $true; $txtKey.SetBounds($px + (P 134), $y, (P 230), (P 24))
$pg.Controls.Add($txtKey)
$lblCost = Add-Text $pg 'Spending limit (US$):' ($px + (P 380)) ($y + (P 4)) (P 130) $fSmall $cText
$numCost = New-Object System.Windows.Forms.NumericUpDown
$numCost.Font = $fSmall; $numCost.Minimum = 1; $numCost.Maximum = 50; $numCost.Value = 5; $numCost.SetBounds($px + (P 512), $y, (P 56), (P 24))
$pg.Controls.Add($numCost)
$y += P 32
$rbSideLocal = New-Object System.Windows.Forms.RadioButton
$rbSideLocal.Text = 'Local: Stable Diffusion XL on your NVIDIA card (free and offline)'
$rbSideLocal.Font = $fBody; $rbSideLocal.SetBounds($px, $y, $cw, (P 24))
$pg.Controls.Add($rbSideLocal)
$y += P 25
$lblLocalInfo = Add-Text $pg ('Needs an NVIDIA card with 8 GB or more. About 10 GB of downloads (tools and model, removed again ' +
	'afterwards), roughly 15-30 minutes; simpler results than the cloud.') ($px + (P 20)) $y ($cw - (P 20)) $fSmall $cMuted
$y = $lblLocalInfo.Bottom + (P 16)
# TensorRT
$lblTrtHead = Add-Text $pg 'TensorRT upscaler (NVIDIA cards)' $px $y $cw $fBold
$y = $lblTrtHead.Bottom + (P 2)
$chkTrt = New-Object System.Windows.Forms.CheckBox
$chkTrt.Font = $fBody; $chkTrt.SetBounds($px, $y, $cw, (P 24))
$chkTrt.Text = 'Download the TensorRT upscaler and accept NVIDIA''s licences (about 1.5 GB, 2 GB installed)'
$pg.Controls.Add($chkTrt)
$y += P 25
$lblTrtInfo = Add-Text $pg ('NVIDIA''s own AI engine, the fastest way to run the upscaling on NVIDIA cards: another choice for ' +
	'Ctrl+F9 in game; everything else works without it. Official Microsoft and NVIDIA packages, checked before use. The first ' +
	'time you choose it in game it prepares itself for your card (a few minutes).') ($px + (P 20)) $y ($cw - (P 20)) $fSmall $cMuted
$y = $lblTrtInfo.Bottom + (P 2)
$lnkTrt = New-Object System.Windows.Forms.LinkLabel
$lnkTrt.Font = $fSmall; $lnkTrt.Text = 'TensorRT licence    cuDNN licence'; $lnkTrt.AutoSize = $true
$lnkTrt.Location = New-Object System.Drawing.Point(($px + (P 20)), $y)
[void]$lnkTrt.Links.Add(0, 16, 'https://docs.nvidia.com/deeplearning/tensorrt/sla/index.html')
[void]$lnkTrt.Links.Add(20, 13, 'https://docs.nvidia.com/deeplearning/cudnn/latest/reference/eula.html')
$lnkTrt.add_LinkClicked({ param($s, $e) Start-Process $e.Link.LinkData })
$pg.Controls.Add($lnkTrt)
$y += P 30
# prewarm
$lblPreHead = Add-Text $pg 'HD backgrounds' $px $y $cw $fBold
$y = $lblPreHead.Bottom + (P 2)
$chkPrewarm = New-Object System.Windows.Forms.CheckBox
$chkPrewarm.Font = $fBody; $chkPrewarm.SetBounds($px, $y, $cw, (P 24)); $chkPrewarm.Checked = $true
$chkPrewarm.Text = 'Prepare the HD backgrounds of every room now (a few minutes)'
$pg.Controls.Add($chkPrewarm)
$y += P 25
$lblPreInfo = Add-Text $pg ('Each room''s background is upscaled once with a heavier AI model and kept, so every room is HD from ' +
	'the first frame. The game opens and closes minimized meanwhile. Otherwise each room is prepared the first time ' +
	'you enter it.') ($px + (P 20)) $y ($cw - (P 20)) $fSmall $cMuted
[void](Add-Text $pg ' ' $px ($lblPreInfo.Bottom + (P 4)) $cw $fSmall)   # bottom margin when scrolled

# -- Page 5: progress ------------------------------------------------------------------------------------
$pg = New-Page 'Installing' ''
$y = $pg.Tag - (P 6)
$lblStatus = Add-Text $pg 'Starting...' $px $y $cw $fBold
$lblStatus.Height = P 44
$y += P 46
$prog = New-Object System.Windows.Forms.ProgressBar
$prog.SetBounds($px, $y, $cw, (P 20)); $prog.Minimum = 0; $prog.Maximum = 1000
$pg.Controls.Add($prog)
$y += P 32
$txtLog = New-Object System.Windows.Forms.TextBox
$txtLog.Multiline = $true; $txtLog.ReadOnly = $true; $txtLog.ScrollBars = 'Vertical'; $txtLog.WordWrap = $true
$txtLog.Font = $fMono; $txtLog.BackColor = [System.Drawing.Color]::FromArgb(250, 250, 250); $txtLog.MaxLength = 0
$txtLog.SetBounds($px, $y, $cw, ($H - $barH) - $y - (P 20))
$pg.Controls.Add($txtLog)
$pgProgress = $pg

# -- Page 6: done ----------------------------------------------------------------------------------------
$pg = New-Page 'Ready to play' ''
$y = $pg.Tag
$lblDone = Add-Text $pg ' ' $px $y $cw
$lblDoneKeys = Add-Text $pg ('In game:  F9 (controller: Back) = remaster settings,  F10 = display mode (original / AI),  ' +
	'F11 = AI on/off,  F12 = what fills the sides of narrow rooms,  Shift+F9 = photo,  Alt+Enter = window / fullscreen.') $px ($y + (P 70)) $cw $fBody $cMuted
$lblDoneNote = Add-Text $pg ' ' $px ($lblDoneKeys.Bottom + (P 14)) $cw $fSmall $cMuted

# -- State of the PC and of the chosen folder ------------------------------------------------------------
$HasNvidia = [bool](Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'NVIDIA' })
$HasNvidiaSmi = [bool](Get-Command nvidia-smi -ErrorAction SilentlyContinue)
$HasVulkan = Test-Path (Join-Path $env:WINDIR 'System32\vulkan-1.dll')
$PkgHasSides = [bool](@((Join-Path $pkg 'sides'), (Join-Path (Split-Path $pkg -Parent) 'sides')) | Where-Object { Get-ChildItem $_ -Filter '*.png' -ErrorAction SilentlyContinue | Select-Object -First 1 })
function Get-DirState {
	$d = $txtDir.Text.Trim()
	$st = @{ Dir = $d; Existing = $false; Trt = $false; Sides = $PkgHasSides }
	if ($d) {
		$st.Existing = Test-Path (Join-Path $d 'scummvm.ini')
		$st.Trt = Test-Path (Join-Path $d 'tensorrt\nvinfer_10.dll')
		if (Get-ChildItem (Join-Path $d 'sides') -Filter '*.png' -ErrorAction SilentlyContinue | Select-Object -First 1) { $st.Sides = $true }
	}
	return $st
}
function Update-DirNote {
	$st = Get-DirState
	if ($st.Existing) { Set-Text $lblDirNote 'An installation is already in this folder: it will be updated. Your settings and saved games are kept.' }
	elseif ($st.Dir -and (Test-Path $st.Dir) -and @(Get-ChildItem $st.Dir -Force -ErrorAction SilentlyContinue).Count -gt 0) {
		Set-Text $lblDirNote 'This folder is not empty. The program''s files are added to it.'
	} else { Set-Text $lblDirNote 'The folder is created if needed.' }
}
$txtDir.add_TextChanged({ Update-DirNote })

function Update-Extras {
	$st = Get-DirState
	$preset = if ($State.Existing -and $chkKeep.Checked) { 'keep' } else { $State.Preset }
	# side art: an AI extra for Monkey Island, not offered with the Original preset; nothing to do when it is already there
	$comi = [bool]$State.Game
	$sideOk = $comi -and $preset -ne 'original' -and -not $st.Sides
	foreach ($c in $rbSideNone, $rbSideCloud, $rbSideLocal) { $c.Enabled = $sideOk }
	if (-not $sideOk) { $rbSideNone.Checked = $true }
	$rbSideNone.Text = if (-not $comi) { 'Only for The Curse of Monkey Island' } elseif ($st.Sides) { 'Side art is already installed' } elseif ($preset -eq 'original') { 'Not used with the Original preset (no AI)' } else { 'Not now' }
	if (-not $HasNvidiaSmi) { $rbSideLocal.Enabled = $false; if ($rbSideLocal.Checked) { $rbSideNone.Checked = $true } }
	$cloud = $rbSideCloud.Checked
	foreach ($c in $txtKey, $numCost, $lblKey, $lblCost) { $c.Enabled = $cloud }
	# TensorRT: NVIDIA only; already installed: kept
	foreach ($c in $lblTrtHead, $chkTrt, $lblTrtInfo, $lnkTrt) { $c.Visible = $HasNvidia }
	if ($st.Trt) { $chkTrt.Checked = $false; $chkTrt.Enabled = $false; $chkTrt.Text = 'The TensorRT upscaler is already installed (kept)' }
	else { $chkTrt.Enabled = $true; $chkTrt.Text = 'Download the TensorRT upscaler and accept NVIDIA''s licences (about 1.5 GB, 2 GB installed)' }
	# HD backgrounds: need Vulkan (the AI); with the Original preset they are not shown, so not prepared by default
	foreach ($c in $lblPreHead, $chkPrewarm, $lblPreInfo) { $c.Enabled = $HasVulkan -and $comi }
	if (-not $HasVulkan) { $chkPrewarm.Checked = $false; $chkPrewarm.Text = 'Needs an up-to-date graphics driver (Vulkan), not found' }
	elseif (-not $comi) {
		$chkPrewarm.Checked = $false
		$others = @($GameDefs | Where-Object { $State.Extra.Contains($_.Id) })
		$who = if ($others.Count -eq 1) { "$($others[0].Title) prepares" } else { 'the others prepare' }
		$chkPrewarm.Text = "Only for The Curse of Monkey Island ($who each room as you play)"
	}
	elseif ($State.Extra.Count) { $chkPrewarm.Text = 'Prepare the HD backgrounds of every Monkey Island room now (a few minutes)' }
	else { $chkPrewarm.Text = 'Prepare the HD backgrounds of every room now (a few minutes)' }
	if ($HasNvidia) { $top = $lnkTrt.Bottom + (P 14) } else { $top = $lblLocalInfo.Bottom + (P 16) }
	$dy = $top - $lblPreHead.Top
	if ($dy) { foreach ($c in $lblPreHead, $chkPrewarm, $lblPreInfo) { $c.Top += $dy } }
}
$rbSideCloud.add_CheckedChanged({ Update-Extras })
$State.PrewarmTouched = $false
$chkPrewarm.add_Click({ $State.PrewarmTouched = $true })

# -- Navigation ------------------------------------------------------------------------------------------
$State.Page = -1
function Show-Page([int]$n) {
	for ($i = 0; $i -lt $pages.Count; $i++) { $pages[$i].Visible = ($i -eq $n) }
	for ($i = 0; $i -lt $stepLabels.Count; $i++) {
		$stepLabels[$i].Font = if ($i -eq $n) { $fBold } else { $fBody }
		$stepLabels[$i].ForeColor = if ($i -eq $n) { $cAccent } elseif ($i -lt $n) { $cText } else { $cMuted }
	}
	$State.Page = $n
	$btnBack.Visible = $n -in 1..4
	$btnNext.Visible = $n -le 4
	$btnNext.Text = if ($n -eq 4) { 'Install' } else { 'Next >' }
	$btnNext.Enabled = $true
	$btnPlay.Visible = $n -eq 6 -and -not $State.Failed
	$btnFolder.Visible = $n -eq 6 -and -not $State.Failed
	$btnCancel.Text = if ($n -eq 6) { 'Close' } elseif ($n -eq 5) { 'Stop' } else { 'Cancel' }
	if ($n -eq 3) {
		$st = Get-DirState
		$chkKeep.Visible = $st.Existing; $State.Existing = $st.Existing   # (Visible reads false while the page is hidden)
		if ($State.KeepFor -ne $st.Dir) {   # an update keeps its settings unless a preset is chosen
			$State.KeepFor = $st.Dir
			$chkKeep.Checked = $st.Existing -and $Preset -notin 'original', 'enhanced', 'experimental'
		}
		Layout-Cards $(if ($st.Existing) { $presetTop + (P 34) } else { $presetTop })
		Update-Cards
	}
	if ($n -eq 4) {
		if (-not $State.PrewarmTouched) {
			$chkPrewarm.Checked = if ($Prewarm) { $Prewarm -eq 'yes' } else { $State.Preset -ne 'original' -or ($State.Existing -and $chkKeep.Checked) }
		}
		Update-Extras
	}
	[void]$pages[$n].Focus()
}

$btnDirBrowse.add_Click({
	$dlg = New-Object System.Windows.Forms.FolderBrowserDialog
	$dlg.Description = 'Select the folder to install to (a new folder inside it is not created: choose or make the folder itself)'
	$dlg.ShowNewFolderButton = $true
	if ($txtDir.Text -and (Test-Path $txtDir.Text)) { $dlg.SelectedPath = $txtDir.Text }
	if ($dlg.ShowDialog($form) -eq 'OK') { $txtDir.Text = $dlg.SelectedPath }
})

function Show-Problem([string]$m) {
	if ($AutoTest) { throw "AutoTest: $m" }
	[void][System.Windows.Forms.MessageBox]::Show($form, $m, 'Setup', 'OK', 'Warning')
}

function Test-Page([int]$n) {
	if ($n -eq 1) {
		$sel = @($GameDefs | Where-Object { $_.CheckBox.Checked })
		if (-not $sel.Count) { Show-Problem 'Tick at least one game to set up.'; return $false }
		$paths = @{}
		foreach ($gd in $sel) {
			$paths[$gd.Id] = $gd.TextBox.Text.Trim()
			if (-not $paths[$gd.Id]) {
				Show-Problem "Choose the folder of your copy of $($gd.Title) first (Browse...)$(if ($gd.Id -ne 'comi') { ', or untick it' })."; return $false
			}
		}
		$form.Cursor = 'WaitCursor'
		$r = Find-Game $paths (@($sel | ForEach-Object { $_.Id }) -join ',')
		$form.Cursor = 'Default'
		foreach ($gd in $sel) {
			if (-not $r[$gd.Id].Path) { Show-Problem "$($gd.Missing) was found in:`n$($paths[$gd.Id])`n`nChoose the folder you installed the game to."; return $false }
		}
		$State.Game = if ($paths.ContainsKey('comi')) { $r.comi.Path } else { $null }
		$State.Extra = [ordered]@{}   # the other games chosen: Id -> folder
		foreach ($gd in $sel) { if ($gd.Id -ne 'comi') { $State.Extra[$gd.Id] = $r[$gd.Id].Path } }
	}
	if ($n -eq 2) {
		$d = $txtDir.Text.Trim()
		if (-not $d -or -not [System.IO.Path]::IsPathRooted($d) -or $d -match '[<>"|?*]') { Show-Problem 'Enter a full folder path, for example C:\Games\Monkey Island AI.'; return $false }
		$full = [System.IO.Path]::GetFullPath($d)
		foreach ($g in @(@($State.Game) + @($State.Extra.Values)) | Where-Object { $_ }) {
			$gf = ([System.IO.Path]::GetFullPath($g)).TrimEnd('\')
			if ($full.TrimEnd('\') -eq $gf -or $full.StartsWith($gf + '\')) {
				Show-Problem 'Please choose a folder outside the game''s own folder: the game files are never changed.'; return $false
			}
		}
		$txtDir.Text = $full
	}
	if ($n -eq 4 -and $rbSideCloud.Checked -and -not $txtKey.Text.Trim() -and -not $env:OPENROUTER_API_KEY -and
		-not (Test-Path (Join-Path $env:USERPROFILE '.openrouter_key'))) {
		Show-Problem 'Enter your OpenRouter key for the cloud side art (openrouter.ai/keys), or choose "Not now".'
		return $false
	}
	return $true
}

$btnNext.add_Click({
	$n = $State.Page
	if (-not (Test-Page $n)) { return }
	if ($n -eq 4) { Start-Install } else { Show-Page ($n + 1) }
})
$btnBack.add_Click({ if ($State.Page -gt 0) { Show-Page ($State.Page - 1) } })
$btnCancel.add_Click({
	if ($State.Page -eq 5) {
		if ($State.Proc -and -not $State.Proc.HasExited) {
			if ($AutoTest -or [System.Windows.Forms.MessageBox]::Show($form, 'Stop the installation? You can run setup again any time; it continues where it can.', 'Setup', 'YesNo', 'Question') -eq 'Yes') {
				$State.Cancelled = $true
				& taskkill.exe /T /F /PID $State.Proc.Id 2>&1 | Out-Null   # with the game or tools it started
			}
		} else { $form.Close() }
		return
	}
	$form.Close()
})
$form.add_FormClosing({
	param($s, $e)
	if ($State.Proc -and -not $State.Proc.HasExited) {
		if ($AutoTest) { $e.Cancel = $true; return }
		if ([System.Windows.Forms.MessageBox]::Show($form, 'Setup is still running. Stop it and close?', 'Setup', 'YesNo', 'Question') -ne 'Yes') { $e.Cancel = $true; return }
		& taskkill.exe /T /F /PID $State.Proc.Id 2>&1 | Out-Null
	}
})

$btnPlay.add_Click({
	$d = $State.InstallDir
	$ini = Join-Path $d 'scummvm.ini'
	Start-Process (Join-Path $d 'scummvm.exe') -WorkingDirectory (Join-Path $d 'logs') -ArgumentList "--config=`"$ini`"", "--logfile=`"$d\logs\scummvm.log`"", $(if ($State.Game) { 'comi' } else { @($State.Extra.Keys)[0] })
	$form.Close()
})
$btnFolder.add_Click({ Start-Process explorer.exe -ArgumentList (Quote-Arg $State.InstallDir) })

# -- Running install.ps1 (and generate_sides.ps1) --------------------------------------------------------
function Add-Log([string]$line) {
	[void]$State.Log.AppendLine($line)
	$txtLog.AppendText($line + "`r`n")
}

function Start-Install {
	$st = Get-DirState
	$State.InstallDir = $st.Dir
	$preset = if ($State.Existing -and $chkKeep.Checked) { 'keep' } else { $State.Preset }
	$State.SideMethod = if ($rbSideCloud.Checked -and $rbSideCloud.Enabled) { 'cloud' } elseif ($rbSideLocal.Checked -and $rbSideLocal.Enabled) { 'local' } else { 'no' }
	$State.Trt = $HasNvidia -and $chkTrt.Enabled -and $chkTrt.Checked
	$State.Prewarm = $chkPrewarm.Enabled -and $chkPrewarm.Checked
	$games = @()
	if ($State.Game) { $games += 'comi' }
	$games += @($State.Extra.Keys)
	$a = @('-InstallDir', (Quote-Arg $st.Dir), '-Games', ($games -join ','))
	if ($State.Game) { $a += @('-GamePath', (Quote-Arg $State.Game)) }
	foreach ($gd in $GameDefs) { if ($State.Extra.Contains($gd.Id)) { $a += @("-$($gd.Param)", (Quote-Arg $State.Extra[$gd.Id])) } }
	$a += @('-Preset', $preset,
		'-TensorRT', $(if ($State.Trt) { 'yes' } else { 'no' }), '-Prewarm', $(if ($State.Prewarm) { 'yes' } else { 'no' }),
		'-GenerateSideArt', 'no')   # side art runs below, as its own step (it is given the choices made here)
	if (-not $chkShortcuts.Checked) { $a += '-NoShortcuts' }
	# progress: rough weights of the steps that will run
	$State.Steps = @(@{ Name = 'files'; W = 15 })
	if ($State.Trt) { $State.Steps += @{ Name = 'trt'; W = 35 } }
	$State.Steps += @{ Name = 'config'; W = 5 }
	if ($State.Prewarm) { $State.Steps += @{ Name = 'prewarm'; W = 45 } }
	if ($State.SideMethod -ne 'no') { $State.Steps += @{ Name = 'sides'; W = 45 } }
	Show-Page 5
	Add-Log "Running install.ps1 $($a -join ' ')"
	Add-Log ''
	Set-Phase 'files' 0
	Start-Child $installPs1 $a $null 'install'
}

function Set-Phase([string]$name, [double]$frac) {
	$State.Phase = $name
	$total = ($State.Steps | ForEach-Object { $_.W } | Measure-Object -Sum).Sum
	$before = 0; $w = 0
	foreach ($s in $State.Steps) { if ($s.Name -eq $name) { $w = $s.W; break }; $before += $s.W }
	if ($w -gt 0) {
		$v = [int](1000 * ($before + $w * [math]::Min(1, [math]::Max(0, $frac))) / $total)
		$prog.Value = [math]::Max($prog.Minimum, [math]::Min($prog.Maximum, $v))
	}
	$text = switch ($name) {
		'files' { 'Copying the program files...' }
		'trt' { "Downloading the TensorRT upscaler (package $([math]::Max(1, $State.Downloads)) of 5, about 1.5 GB in all)..." }
		'config' { 'Writing your settings...' }
		'prewarm' { $State.PrewarmText }
		'sides' { 'Making the side art from your copy of the game (this can take a while; the game opens and closes minimized at first)...' }
		'done' { 'Finished.' }
		default { $name }
	}
	if ($text) { Set-Text $lblStatus $text }
}

function Start-Child([string]$script, [string[]]$argList, [hashtable]$envVars, [string]$step) {
	$State.Step = $step
	$State.Start = Get-Date
	$State.Proc = New-ChildProcess $script $argList $envVars
	$State.OutTask = $State.Proc.StandardOutput.ReadLineAsync()
	$State.ErrTask = $State.Proc.StandardError.ReadLineAsync()
	$timer.Start()
}

function On-Line([string]$line) {
	Add-Log $line
	if ($State.Step -ne 'install') { return }
	if ($line -match 'Installed files to|Installed your|Kept your installed|Narrow rooms use') { if ($State.Phase -eq 'files') { Set-Phase 'files' 1 } }
	if ($line -match '\] Downloading ') { $State.Downloads++; Set-Phase 'trt' (($State.Downloads - 1) / 5) }
	if ($line -match 'Installed the TensorRT|TensorRT skipped') { Set-Phase 'trt' 1 }
	if ($line -match 'Wrote scummvm\.ini|Kept your existing settings') { Set-Phase 'config' 0.5 }
	if ($line -match '\] Preset: |Created desktop and Start menu') { Set-Phase 'config' 1 }
}

$PrewarmWork = Join-Path $env:TEMP 'scummvm-ai-upscale-prewarm'   # prewarm.ps1 writes a log per room there
function Update-Prewarm {
	if (-not $State.Prewarm -or $State.Step -ne 'install' -or $State.Phase -notin 'config', 'prewarm') { return }
	$since = $State.Start
	$rooms = @(Get-ChildItem $PrewarmWork -Filter 'room*.log' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $since }).Count
	$trt = Get-Item (Join-Path $PrewarmWork 'tensorrt.log') -ErrorAction SilentlyContinue
	if ($rooms -gt 0) {
		$State.PrewarmText = "Preparing HD backgrounds: room $rooms of 93 (the game opens and closes minimized)..."
		Set-Phase 'prewarm' (($rooms - 1) / 93)
	} elseif ($trt -and $trt.LastWriteTime -ge $since) {
		$State.PrewarmText = 'Preparing the TensorRT upscaler for your graphics card (a few minutes the first time)...'
		Set-Phase 'prewarm' 0
	}
}

function On-ChildExit([int]$code) {
	$timer.Stop()
	$State.Proc = $null
	if ($State.Step -eq 'install') {
		if ($code -ne 0 -or $State.Cancelled) { Finish-Install $false $code; return }
		if ($State.SideMethod -ne 'no') {
			Add-Log ''
			$gs = Join-Path $State.InstallDir 'generate_sides.ps1'
			$a = @('-InstallDir', (Quote-Arg $State.InstallDir), '-Method', $State.SideMethod, '-NoPreview', '-RemoveTools', 'yes')
			if ($State.SideMethod -eq 'cloud') { $a += @('-MaxCost', [string][int]$numCost.Value) }
			$envVars = @{}
			if ($txtKey.Text.Trim()) { $envVars['OPENROUTER_API_KEY'] = $txtKey.Text.Trim() }
			Add-Log "Running generate_sides.ps1 $($a -join ' ')"
			Set-Phase 'sides' 0
			$prog.Style = 'Marquee'
			Start-Child $gs $a $envVars 'sides'
			return
		}
		Finish-Install $true 0
		return
	}
	# side art: the game is installed either way; report how it went
	$prog.Style = 'Continuous'
	$State.SideArtResult = $code
	Finish-Install $true 0
}

function Finish-Install([bool]$ok, [int]$code) {
	$State.Failed = -not $ok
	try {
		$logs = Join-Path $State.InstallDir 'logs'
		if (Test-Path $logs) { [System.IO.File]::WriteAllText((Join-Path $logs 'install.log'), $State.Log.ToString()) }
	} catch { }
	if (-not $ok) {
		$prog.Style = 'Continuous'
		$msg = if ($State.Cancelled) { 'Stopped. Run setup again any time.' } else { 'The installation did not finish (see the messages below). Fix what they say, then click Back to try again.' }
		Set-Text $lblStatus $msg
		$lblStatus.ForeColor = $cError
		$stepLabels[5].ForeColor = $cError
		$btnBack.Visible = $true; $btnBack.Enabled = $true
		$btnCancel.Text = 'Close'
		$State.Cancelled = $false
		if ($AutoTest) { $State.AutoDone = $true }
		return
	}
	$prog.Value = $prog.Maximum
	Set-Phase 'done' 1
	$lblStatus.ForeColor = $cOk
	$titles = @()
	if ($State.Game) { $titles += 'The Curse of Monkey Island' }
	foreach ($gd in $GameDefs) { if ($State.Extra.Contains($gd.Id)) { $titles += $gd.Title } }
	$lines = @("$(Join-Names $titles) with AI upscaling $(if ($titles.Count -gt 1) { 'are' } else { 'is' }) installed in:`n$($State.InstallDir)")
	if ($chkShortcuts.Checked) {
		$lines += "Start $(if ($titles.Count -gt 1) { 'them' } else { 'it' }) any time from the desktop or the Start menu: " + (Join-Names @($titles | ForEach-Object { '"' + $_ + ' (AI upscaling)"' })) + '.'
	}
	Set-Text $lblDone ($lines -join "`n`n")
	$lblDoneKeys.Top = $lblDone.Bottom + (P 16)
	$note = 'Your settings are in scummvm.ini in the install folder; to update or change the preset, run setup again. A log of this installation is in the logs folder.'
	if ($null -ne $State.SideArtResult) {
		$note = switch ($State.SideArtResult) {
			0 { 'Side art is ready. ' + $note }
			2 { 'Some rooms got no side art (see logs\install.log); run generate_sides.cmd in the install folder to retry them. ' + $note }
			default { 'The side art was not made (see logs\install.log); run generate_sides.cmd in the install folder any time. ' + $note }
		}
	}
	Set-Text $lblDoneNote $note
	$lblDoneNote.Top = $lblDoneKeys.Bottom + (P 14)
	if ($AutoTest) { Save-Shot 'progress-finished'; $State.AutoDone = $true }
	Show-Page 6
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 100
$timer.add_Tick({
	$p = $State.Proc
	if (-not $p) { $timer.Stop(); return }
	$n = 0
	while ($n -lt 300) {
		$got = $false
		if ($State.OutTask -and $State.OutTask.IsCompleted) {
			$l = $State.OutTask.Result
			if ($null -eq $l) { $State.OutTask = $null } else { On-Line $l; $State.OutTask = $p.StandardOutput.ReadLineAsync(); $got = $true }
		}
		if ($State.ErrTask -and $State.ErrTask.IsCompleted) {
			$l = $State.ErrTask.Result
			if ($null -eq $l) { $State.ErrTask = $null } else { On-Line $l; $State.ErrTask = $p.StandardError.ReadLineAsync(); $got = $true }
		}
		if (-not $got) { break }
		$n++
	}
	if ((++$State.Ticks % 10) -eq 0) { Update-Prewarm }
	if (-not $State.OutTask -and -not $State.ErrTask -and $p.HasExited) {
		$p.WaitForExit()
		$code = $p.ExitCode
		Add-Log ''
		On-ChildExit $code
	}
})

# -- Test mode: click through with the pre-filled answers ------------------------------------------------
function Save-Shot([string]$name) {
	if (-not $ScreenshotDir) { return }
	New-Item -ItemType Directory -Force $ScreenshotDir | Out-Null
	$form.Refresh()
	[System.Windows.Forms.Application]::DoEvents()
	$r = $form.Bounds
	$bmp = New-Object System.Drawing.Bitmap($r.Width, $r.Height)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.CopyFromScreen($r.Location, [System.Drawing.Point]::Empty, $r.Size)
	$g.Dispose()
	$bmp.Save((Join-Path $ScreenshotDir "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
	$bmp.Dispose()
}
if ($AutoTest) {
	$State.AutoStep = 0
	$auto = New-Object System.Windows.Forms.Timer
	$auto.Interval = 700
	$auto.add_Tick({
		try {
			$names = 'welcome', 'game', 'installdir', 'preset', 'extras'
			$n = $State.Page
			if ($n -le 4) {
				if ($n -eq 3 -and $State.AutoStep -eq 3) {
					Save-Shot 'preset'
					if ($cardPanels['experimental'].Enabled) {   # also show another choice, then the one asked for
						$cardRadios['experimental'].Checked = $true; Save-Shot 'preset-experimental'
						$cardRadios[$(if ($Preset -and $Preset -ne 'keep') { $Preset } else { 'enhanced' })].Checked = $true
					}
				} else { Save-Shot ('{0}-{1}' -f $n, $names[$n]) }
				if ($n -eq 4) { $rbSideCloud.Checked = $true; Save-Shot '4-extras-cloud'; $rbSideNone.Checked = $true }
				$State.AutoStep++
				$btnNext.PerformClick()
				if ($State.Page -eq 5) { Save-Shot '5-progress-start' }
			} elseif ($n -eq 5 -and -not $State.ShotRunning -and ((Get-Date) - $State.Start).TotalSeconds -gt 3) {
				Save-Shot '5-progress-running'; $State.ShotRunning = $true
			}
			if ($State.AutoDone) {
				$auto.Stop()
				Save-Shot $(if ($State.Failed) { '5-failed' } else { '6-done' })
				$script:AutoExit = if ($State.Failed) { 1 } else { 0 }
				$form.Close()
			}
		} catch {
			$auto.Stop(); $script:AutoExit = 2
			[Console]::Error.WriteLine("AutoTest error: $($_.Exception.Message)")
			$form.Close()
		}
	})
	$form.TopMost = $true
}

# -- Start -----------------------------------------------------------------------------------------------
$txtDir.Text = if ($InstallDir) { $InstallDir } else { Join-Path $env:LOCALAPPDATA 'ScummVM-AI-Upscale' }
if ($Preset -in 'original', 'enhanced', 'experimental') { $cardRadios[$Preset].Checked = $true } else { $cardRadios['enhanced'].Checked = $true }
Update-DirNote
$given = @{}
foreach ($gd in $GameDefs) { if ($gd.Path) { $given[$gd.Id] = $gd.Path } }
$found = Find-Game $given ''
foreach ($gd in $GameDefs) {
	$f = $found[$gd.Id]
	if ($f.Path) {
		$gd.TextBox.Text = $f.Path
		Set-Text $gd.Status $(if ($f.Store) { "Found your $($f.Store) copy:" } else { 'Game folder:' })
		$gd.Status.ForeColor = $cOk
	} else {
		if ($gd.Path) { $gd.TextBox.Text = $gd.Path }
		if ($gd.Id -eq 'comi') {
			Set-Text $gd.Status 'Your copy was not found automatically. Please choose its folder.'
			$gd.Status.ForeColor = $cError
		} else {
			Set-Text $gd.Status 'Not found automatically. If you have it, tick it and choose its folder.'
			$gd.Status.ForeColor = $cMuted
		}
	}
}
# ticked: the games found (Monkey Island also when none was found), or -Games
$gl = @($Games -split '[\s,]+' | Where-Object { $_ })
$anyOther = [bool]@($GameDefs | Where-Object { $_.Id -ne 'comi' -and $found[$_.Id].Path }).Count
foreach ($gd in $GameDefs) {
	$gd.CheckBox.Checked = if ($gl.Count) { $gl -contains $gd.Id } elseif ($gd.Id -eq 'comi') { [bool]$found.comi.Path -or -not $anyOther } else { [bool]$found[$gd.Id].Path }
	$gd.TextBox.Enabled = $gd.CheckBox.Checked; $gd.BrowseButton.Enabled = $gd.CheckBox.Checked
}
$form.add_Shown({
	$script:shown = $true
	$form.Activate()
	if ($AutoTest) { $auto.Start() }
})
Show-Page 0
$script:AutoExit = $null
try {
	[void]$form.ShowDialog()
} catch {
	if (-not $shown) { exit 10 }
	[void][System.Windows.Forms.MessageBox]::Show("Setup stopped with an error:`n$($_.Exception.Message)", 'Setup', 'OK', 'Error')
	exit 1
}
if ($null -ne $script:AutoExit) { exit $script:AutoExit }
if ($State.Failed) { exit 1 }
exit 0
