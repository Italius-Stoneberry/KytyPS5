# Windows launcher: runs the Windows build with the switches of a Linux launch config.
#   .\run-windows.ps1                        release-stage1 switches, 2560x1440
#   .\run-windows.ps1 -Precompile            compile every recorded shader and pipeline, then exit
#                                            (every shader of the game: precompile-windows.ps1)
#   .\run-windows.ps1 -Baseline              no performance switches
#   .\run-windows.ps1 -Config <launch.json>  another checkpoint's switches
#   .\run-windows.ps1 -Width 1920 -Height 1080 -Fullscreen
#   .\run-windows.ps1 -NoRedZone             without guest red-zone protection
#   .\run-windows.ps1 -Patch <cheat.json>    apply an etaHEN-style game patch
#   .\run-windows.ps1 -Fullscreen -AspectFit keep the game's 16:9 (black bars) instead of stretching
#   .\run-windows.ps1 -Set KEY=VALUE         override a switch of the config (KEY= removes it)
#   .\run-windows.ps1 -FrameGen 1            DLSS frame generation, 1 generated frame per rendered
#                                            frame (2x); needs _Build\deps\streamline\sdk
#   .\run-windows.ps1 -Vblank 240            another virtual vblank rate (default 60, the console's;
#                                            faster rates speed up the game's clock)
#   .\run-windows.ps1 -Game <folder>         the game (the folder with eboot.bin); remembered in
#                                            game-path.txt, a folder dialog when none is known
#   .\run-windows.ps1 -Affinity FFFFCF       only these CPUs (hex mask; default: the config's list, else all)
#   .\run-windows.ps1 -Prompt                a dialog first when the game version is untested or the
#                                            shaders are not precompiled for this GPU (run.cmd)
#   .\run-windows.ps1 -DryRun                print environment and command only
# A portable package (package-windows.ps1) has its kyty_emulator.exe, launch.json and srt-aot.dll
# next to this script: those are used instead of the build tree's.
param(
	[string]$Config = $(if (Test-Path "$PSScriptRoot\launch.json") { "$PSScriptRoot\launch.json" } else { "$PSScriptRoot\_Build\release-stage1-20260927\launch.json" }),
	[string]$Game = '',
	[string]$Exe = $(if (Test-Path "$PSScriptRoot\kyty_emulator.exe") { "$PSScriptRoot\kyty_emulator.exe" } else { "$PSScriptRoot\_Build\windows\kyty_emulator.exe" }),
	[int]$Width = 0,
	[int]$Height = 0,
	[switch]$Fullscreen,
	[switch]$Baseline,
	[switch]$Precompile,
	[int]$Threads = 0,
	[switch]$NoRedZone,
	[switch]$NoAot,
	[string]$PresentMode = '',
	[int]$Vblank = 0,
	[string[]]$Set = @(),
	[string]$Patch = '',
	[switch]$AspectFit,
	[int]$FrameGen = 0,
	[string]$Affinity = '',
	[switch]$Prompt,
	[switch]$DryRun
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path $Exe)) { throw "missing $Exe; build it with build-windows.cmd" }

# Dialogs: sharp on scaled displays (the process is DPI aware before its first window).
function Initialize-Dialogs {
	Add-Type -AssemblyName System.Windows.Forms, System.Drawing
	Add-Type -Namespace Kyty -Name Dpi -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
	[Kyty.Dpi]::SetProcessDPIAware() | Out-Null
	[System.Windows.Forms.Application]::EnableVisualStyles()
}
# A message with a button per choice and an optional check box: the chosen index (-1: closed) and
# whether the box was checked.
function Show-Choice([string]$message, [string[]]$choices, [string]$checkbox = '') {
	$form = New-Object System.Windows.Forms.Form -Property @{
		Text = 'KytyPS5'; FormBorderStyle = 'FixedDialog'; MaximizeBox = $false; MinimizeBox = $false; StartPosition = 'CenterScreen'
		AutoSize = $true; AutoSizeMode = 'GrowAndShrink'; Padding = (New-Object System.Windows.Forms.Padding 16)
		Font = (New-Object System.Drawing.Font 'Segoe UI', 10); TopMost = $true; Tag = -1 }
	$panel = New-Object System.Windows.Forms.FlowLayoutPanel -Property @{ FlowDirection = 'TopDown'; WrapContents = $false; AutoSize = $true }
	# Lines of about 70 characters (the font's height follows the display's scale).
	$panel.Controls.Add((New-Object System.Windows.Forms.Label -Property @{ Text = $message; AutoSize = $true
		MaximumSize = (New-Object System.Drawing.Size ($form.Font.Height * 30), 0) }))
	$check = New-Object System.Windows.Forms.CheckBox -Property @{ Text = $checkbox; AutoSize = $true; Visible = [bool]$checkbox }
	$panel.Controls.Add($check)
	$row = New-Object System.Windows.Forms.FlowLayoutPanel -Property @{ AutoSize = $true }
	for ($i = 0; $i -lt $choices.Count; $i++) {
		$button = New-Object System.Windows.Forms.Button -Property @{ Text = $choices[$i]; Tag = $i; AutoSize = $true; Padding = (New-Object System.Windows.Forms.Padding 8, 4, 8, 4) }
		$button.Add_Click({ $form.Tag = $this.Tag; $form.Close() })
		$row.Controls.Add($button)
	}
	$panel.Controls.Add($row)
	$form.Controls.Add($panel)
	[void]$form.ShowDialog()
	return $form.Tag, $check.Checked
}

# The game: -Game, else the last one given, else the default folder; a folder dialog when that has
# no eboot.bin (a portable package started by double-clicking run.cmd).
$gameFile = "$PSScriptRoot\game-path.txt"
$remember = [bool]$Game
if (!$Game -and (Test-Path $gameFile)) { $Game = (Get-Content $gameFile -Raw).Trim() }
if (!$Game) { $Game = "$env:USERPROFILE\Documents\PPSA01341-app0" }
if ($Prompt -or (!$remember -and !(Test-Path "$Game\eboot.bin"))) { Initialize-Dialogs }
if (!$remember -and !(Test-Path "$Game\eboot.bin")) {
	$dialog = New-Object System.Windows.Forms.FolderBrowserDialog -Property @{ Description = 'Choose the game folder (the one with eboot.bin and sce_sys)' }
	if ($dialog.ShowDialog() -eq 'OK') { $Game = $dialog.SelectedPath; $remember = $true }
}
if (!(Test-Path "$Game\eboot.bin")) { throw "no eboot.bin in $Game" }
if ($remember) { Set-Content $gameFile $Game -Encoding UTF8 }
# Its title and version (sce_sys\param.json): the emulator is tested with one of them.
$testedVersion = 'PPSA01341 01.007.000'
$param = if (Test-Path "$Game\sce_sys\param.json") { Get-Content "$Game\sce_sys\param.json" -Raw -Encoding UTF8 | ConvertFrom-Json }
$titleId = if ($param) { $param.titleId } else { '' }
$version = if ($param) { "$titleId $($param.contentVersion)" } else { 'unknown' }
$titleName = if ($param) { $param.localizedParameters.($param.localizedParameters.defaultLanguage).titleName }
$tested = $version -eq $testedVersion

$launch = Get-Content $Config -Raw | ConvertFrom-Json

# Emulator options: everything after the binary in the Linux command, minus --game.
$command = @($launch.command)
$start = [Array]::IndexOf($command, '--') + 2
$options = New-Object System.Collections.Generic.List[string]
for ($i = $start; $i -lt $command.Count; $i++) {
	if ($command[$i] -eq '--game') { $i++; continue }
	$options.Add($command[$i])
}
function Set-Option([string]$name, [string]$value) {
	$at = $options.IndexOf($name)
	if ($at -ge 0) { $options[$at + 1] = $value } else { $options.Add($name); $options.Add($value) }
}
if ($Width -gt 0) { Set-Option '--screen-width' "$Width" }
if ($Height -gt 0) { Set-Option '--screen-height' "$Height" }
if ($Fullscreen) { $options.Add('--fullscreen') }
if ($PresentMode) { Set-Option '--present-mode' $PresentMode }
# The game's clock assumes the console's 60 Hz vblank (its frame-rate target is 60 fps and the
# flip queue, one flip per vblank, is what paces it): with the Linux configs' 240 Hz vblank the
# cutscenes flipped at up to 230 fps and played about four times too fast, and gameplay started
# in slow motion. At 60 Hz both run at normal speed, and frames that miss a vblank still queue
# (37 fps average on the walk, not a 30 fps lock).
Set-Option '--vblank-frequency' "$(if ($Vblank -gt 0) { $Vblank } else { 60 })"
if ($Patch) { Set-Option '--game-patch' (Resolve-Path $Patch).Path }
# Windows dispatches exceptions on the faulting thread's stack, over the guest's SysV red
# zone (Linux signal delivery skips it). The write-tracking faults of the performance paths
# then corrupt guest locals; --redzone rewrites the guest's red-zone accesses at load time.
if (!$NoRedZone) { $options.Add('--redzone') }

# Environment switches. The ahead-of-time SRT library of a Linux config is a .so: use the DLL
# built from the same plan sources (tools\local\compile-srt-aot-windows.py <library dir>).
$environment = [ordered]@{}
if (!$Baseline) {
	foreach ($property in $launch.environment.PSObject.Properties) {
		if ($property.Name -eq 'KYTY_SRT_AOT_LIBRARY') {
			if ($NoAot) { continue }
			$relative = ([string]$property.Value -replace '^.*?/_Build/', '_Build/') -replace 'srt-aot\.so$', 'srt-aot.dll'
			$dll = Join-Path $PSScriptRoot $relative
			# A library built on Windows from this tree's own plan exports (KYTY_SRT_AOT_EXPORT, then
			# compile-srt-aot-windows.py) matches plans the Linux one misses: the walk's plans
			# changed since it was built (28% of them ran in the interpreter).
			$windows = Get-ChildItem "$PSScriptRoot\_Build\srt-aot\windows-libraries\*\srt-aot.dll" -ErrorAction SilentlyContinue |
				Sort-Object LastWriteTime -Descending | Select-Object -First 1
			if ($windows) { $dll = $windows.FullName }
			if (Test-Path $dll) {
				$environment[$property.Name] = (Resolve-Path $dll).Path
			} else {
				Write-Host "SRT AOT: $relative missing; build it with tools\local\compile-srt-aot-windows.py"
			}
			continue
		}
		$environment[$property.Name] = [string]$property.Value
	}
}

# CPU affinity: -Affinity, else the config's list (this PC's leaves CPUs 4 and 5 out), else every
# CPU; only CPUs this PC has.
$all = if ([Environment]::ProcessorCount -ge 64) { [int64]-1 } else { ([int64]1 -shl [Environment]::ProcessorCount) - 1 }
$mask = [int64]0
if ($Affinity) { $mask = [Convert]::ToInt64(($Affinity -replace '^0x'), 16) } else { foreach ($cpu in $launch.cpu_affinity) { $mask = $mask -bor ([int64]1 -shl [int]$cpu) } }
$mask = $mask -band $all
if ($mask -eq 0) { $mask = $all }
$cpus = 0
for ($bit = 0; $bit -lt 64; $bit++) { if ($mask -band ([int64]1 -shl $bit)) { $cpus++ } }

# The render thread on the recording worker's CPUs (this PC's P-cores without CPU 0, which takes
# most interrupts on Windows): +2.5% over free placement in the fixed scene. Pinning it to CPU 0
# alone, as on Linux, halved the frame rate here. A config without them leaves both to Windows;
# "auto" (the portable package's config) picks the performance cores of this PC (below).
if (!$environment.Contains('KYTY_RENDER_CPUS') -and $environment.Contains('KYTY_RECORDING_CPUS')) {
	$environment['KYTY_RENDER_CPUS'] = $environment['KYTY_RECORDING_CPUS']
}

# Image staging copies on the upload worker too (KYTY_ASYNC_UPLOAD=2): streamed textures were
# up to 4 MB of backing reads a frame on the render thread while walking.
if ($environment['KYTY_ASYNC_UPLOAD'] -eq '1') { $environment['KYTY_ASYNC_UPLOAD'] = '2' }
# Retired buffers' memory is freed on a worker (a kernel call per dedicated allocation).
if (!$environment.Contains('KYTY_BUFFER_RECLAIM') -and !$Baseline) { $environment['KYTY_BUFFER_RECLAIM'] = '1' }
# Ordinary indexed/auto draws recorded as packets too (about 1.3 ms a frame less render time).
if (!$environment.Contains('KYTY_DRAW_PACKETS') -and !$Baseline) { $environment['KYTY_DRAW_PACKETS'] = '1' }
# A CPU write into a large image re-uploads only the written part: the game streams textures one
# layer at a time into 320-352 MiB arrays, and each layer re-uploaded the whole array (20-70 ms).
if (!$environment.Contains('KYTY_PARTIAL_IMAGE_DIRTY') -and !$Baseline) { $environment['KYTY_PARTIAL_IMAGE_DIRTY'] = '1' }
# ... and inside a large subresource only the rows of tile blocks over the written ranges: an
# 8192x8192 streamed texture's mip 0 is 64 MiB of 85, and picking it whole re-uploaded all of it.
if (!$environment.Contains('KYTY_PARTIAL_ROW_BANDS') -and !$Baseline) { $environment['KYTY_PARTIAL_ROW_BANDS'] = '1' }
# Native XPR pipeline variants compile on worker threads; their draws take the normal path until
# then. Entering a new area compiled dozens at once on the render thread (a 200 ms frame).
if (!$environment.Contains('KYTY_ASYNC_XPR_PIPELINES') -and !$Baseline) { $environment['KYTY_ASYNC_XPR_PIPELINES'] = '1' }
# Memory the game releases (texture pool layers, 64 KiB mappings each) is not unprotected mapping
# by mapping before its unmap: VirtualProtect was ~15% of the render thread in the open area.
if (!$environment.Contains('KYTY_UNMAP_PROTECT_SKIP') -and !$Baseline) { $environment['KYTY_UNMAP_PROTECT_SKIP'] = '1' }
# The key of the driver pipeline cache (_PipelineCache\local\<key>); it also enables the shader
# warmup, which a modified source tree runs without otherwise. One key for every build: the driver
# finds a pipeline by its whole input (SPIR-V and state), so a new build reuses the pipelines of the
# shaders it left alone. Keyed by the executable's SHA-256, every update compiled the ~3700 warmup
# pipelines again (2-2.5 minutes of "Preparing pipelines").
$environment['KYTY_DRIVER_CACHE_KEY'] = '1f3c29b70e536c8f5a5e812e793b33ac2eb6858b4810ed98ca2a51b9fa5eed97'
if ($Precompile) {
	# Translate every recorded shader and create every recorded pipeline on all allowed CPUs,
	# save the driver cache, exit. Later launches warm up from that cache in seconds.
	$environment['KYTY_SHADER_WARMUP'] = '1'
	$environment['KYTY_SHADER_WARMUP_ONLY'] = '1'
	$environment['KYTY_SHADER_WARMUP_THREADS'] = "$cpus"
	$environment.Remove('KYTY_SHADER_WARMUP_SECONDS')
} else {
	# An earlier -Precompile in the same shell leaves this set; the game must not exit after warmup.
	Remove-Item env:KYTY_SHADER_WARMUP_ONLY -ErrorAction SilentlyContinue
	# At most a minute of start-up warmup: with an empty driver cache (the first launch, a driver
	# update) its ~4400 pipelines took 4.5 minutes; the rest are built at their first use (a fast
	# unoptimized build at once, the optimized one in the background) and cached from then on.
	if (!$environment.Contains('KYTY_SHADER_WARMUP_SECONDS')) { $environment['KYTY_SHADER_WARMUP_SECONDS'] = '60' }
}
if ($Threads -gt 0) { $environment['KYTY_SHADER_WARMUP_THREADS'] = "$Threads" }
if ($AspectFit) { $environment['KYTY_PRESENT_ASPECT'] = 'fit' }
if ($FrameGen -gt 0) { $environment['KYTY_FRAMEGEN'] = "$FrameGen" }
# -Set KEY=VALUE overrides a switch of the config; KEY= drops it. Several: -Set A=1,B=2 (a comma
# starts a new pair only before KEY=, so values such as 1,2,3,6,7 stay whole).
foreach ($pair in ($Set | ForEach-Object { $_ -split ',(?=[A-Za-z_][A-Za-z0-9_]*=)' })) {
	$key, $value = $pair -split '=', 2
	if ($value) { $environment[$key] = $value } else { $environment.Remove($key); Remove-Item "env:$key" -ErrorAction SilentlyContinue }
}

# The performance cores of a hybrid CPU (Windows' CPU set efficiency classes: an Intel Core with
# P- and E-cores), without the core of CPU 0, within the launch's CPUs: a list such as "1,2,3,4,5",
# or nothing when every core is alike (the threads are left to Windows there).
function Get-PerformanceCpus([int64]$allowed) {
	Add-Type -Namespace Kyty -Name CpuSets -MemberDefinition @'
[DllImport("kernel32.dll")]
static extern bool GetSystemCpuSetInformation(IntPtr information, uint length, out uint returned, IntPtr process, uint flags);
// Group 0's logical processors as (index, core, efficiency class) triples.
public static int[] Query() {
	var result = new System.Collections.Generic.List<int>();
	uint length;
	GetSystemCpuSetInformation(IntPtr.Zero, 0, out length, IntPtr.Zero, 0);
	if (length == 0) return result.ToArray();
	IntPtr buffer = Marshal.AllocHGlobal((int)length);
	try {
		if (!GetSystemCpuSetInformation(buffer, length, out length, IntPtr.Zero, 0)) return result.ToArray();
		for (int offset = 0; offset < length; offset += Marshal.ReadInt32(buffer, offset)) {
			if (Marshal.ReadInt32(buffer, offset + 4) != 0 || Marshal.ReadInt16(buffer, offset + 12) != 0) continue;
			result.Add(Marshal.ReadByte(buffer, offset + 14));
			result.Add(Marshal.ReadByte(buffer, offset + 15));
			result.Add(Marshal.ReadByte(buffer, offset + 18));
		}
	} finally {
		Marshal.FreeHGlobal(buffer);
	}
	return result.ToArray();
}
'@
	$values = [Kyty.CpuSets]::Query()
	$sets = for ($i = 0; $i + 2 -lt $values.Count; $i += 3) { [pscustomobject]@{ Cpu = $values[$i]; Core = $values[$i + 1]; Class = $values[$i + 2] } }
	if (@($sets | ForEach-Object Class | Sort-Object -Unique).Count -lt 2) { return '' }
	$fastest = ($sets | Measure-Object Class -Maximum).Maximum
	$interrupts = ($sets | Where-Object Cpu -eq 0).Core
	return (($sets | Where-Object { $_.Class -eq $fastest -and $_.Core -ne $interrupts -and $_.Cpu -lt 64 -and
		($allowed -band ([int64]1 -shl $_.Cpu)) } | ForEach-Object Cpu) -join ',')
}
if ('KYTY_RECORDING_CPUS', 'KYTY_RENDER_CPUS' | Where-Object { $environment[$_] -eq 'auto' }) {
	$performance = Get-PerformanceCpus $mask
	foreach ($key in 'KYTY_RECORDING_CPUS', 'KYTY_RENDER_CPUS') {
		if ($environment[$key] -ne 'auto') { continue }
		if ($performance) { $environment[$key] = $performance } else { $environment.Remove($key) }
	}
}

$quoted = @('--game', "`"$Game`"") + ($options | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
$logDir = if (Test-Path "$PSScriptRoot\_Build") { "$PSScriptRoot\_Build\run-logs" } else { "$PSScriptRoot\logs" }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
Write-Host "game:     $(if ($titleName) { "$titleName, " })$version$(if (!$tested) { " (untested: the emulator is tested with $testedVersion)" })"
Write-Host "config:   $Config$(if ($Baseline) { ' (baseline: no switches)' })$(if ($Precompile) { ' (precompile)' })"
Write-Host ("affinity: 0x{0:X} ({1} CPUs){2}" -f $mask, $cpus, $(if ($environment['KYTY_RENDER_CPUS']) { "; render threads on CPUs $($environment['KYTY_RENDER_CPUS'])" }))
Write-Host "switches: $($environment.Count)"
Write-Host "command:  $Exe $($quoted -join ' ')"
if ($DryRun) { $environment.GetEnumerator() | ForEach-Object { "  $($_.Key)=$($_.Value)" }; return }

# Shaders, from the seed file (every shader of the game) by the precompile program: the shader
# prefetch's inputs for this GPU and driver (_PipelineCache\static\<title>.shaders; new shaders are
# then translated in the background while playing), made when they are missing or stale (the first
# launch, a driver update: half a minute on 22 CPUs); and the static pipeline cache, the whole game
# compiled ahead by precompile-windows.ps1 (44 minutes on 22 CPUs), offered by -Prompt.
$tool = Join-Path (Split-Path $Exe) 'kyty_shader_precompile.exe'
$seeds = @("$PSScriptRoot\seeds.seeds", "$PSScriptRoot\_Build\static-precompile\seeds.seeds") | Where-Object { Test-Path $_ } | Select-Object -First 1
$shaders = !$Precompile -and $seeds -and (Test-Path $tool)
# The precompile program on the launch's CPUs, its output in the console or a file: its exit code.
function Invoke-Tool([string[]]$arguments, [string]$output = '') {
	$parameters = @{ FilePath = $tool; WorkingDirectory = $PSScriptRoot; NoNewWindow = $true; PassThru = $true
		ArgumentList = @('--game', "`"$Game`"", '--seeds', "`"$seeds`"") + $arguments }
	if ($output) { $parameters['RedirectStandardOutput'] = $output }
	$process = Start-Process @parameters
	$null = $process.Handle # keeps the exit code readable
	if ($mask -ne $all) { $process.ProcessorAffinity = [IntPtr]$mask }
	$process.WaitForExit()
	return $process.ExitCode
}
$inputsReady = $cacheReady = $false
if ($shaders) {
	$statusFile = [IO.Path]::GetTempFileName()
	$shaders = (Invoke-Tool @('--status') $statusFile) -eq 0
	$status = Get-Content $statusFile -Raw
	Remove-Item $statusFile
	$inputsReady = $status -match 'inputs current'
	# A precompile that stopped before its last merge keeps its shards' checkpoints (small ones stay
	# behind after a merge).
	$cacheReady = $status -match 'static cache current' -and
		!(Get-ChildItem "$PSScriptRoot\_PipelineCache\static\*.shard*" -ErrorAction SilentlyContinue | Where-Object Length -gt 1MB)
	Write-Host "shaders:  prefetch inputs $(if ($inputsReady) { 'ready' } else { 'to be made' }), static pipeline cache $(if ($cacheReady) { 'ready' } else { 'not made (precompile-windows.ps1)' })"
}

# -Prompt: the game version and the precompile, before anything starts.
$noPrompt = "$PSScriptRoot\no-precompile-prompt.txt"
$offer = $shaders -and !$cacheReady -and !(Test-Path $noPrompt)
if ($Prompt -and ($offer -or !$tested)) {
	$info = "Game: $(if ($titleName) { $titleName } else { 'unknown' }) ($version)"
	$info += if ($tested) { ', a tested version.' } else { "`n⚠ This version is untested (the emulator was tested with $testedVersion): it may not run or may fail." }
	if ($offer) {
		$minutes = [math]::Ceiling(44 * 22 / $cpus / 10) * 10
		$time = if ($minutes -lt 90) { "about $minutes minutes" } else { 'about {0:N1} hours' -f ($minutes / 60) }
		$message = "$info`n`nThe shaders are not precompiled for this graphics card yet: scenes and effects stutter the first time they appear in the game (a fraction of a second to a few seconds).`n" +
			"The precompile compiles every shader of the game once: $time on this PC (with the CPU fully loaded; closing its window stops it, and the next run continues). " +
			"It is needed only once, and again after a graphics driver update."
		$choice, $never = Show-Choice $message @('Precompile first, then start the game', 'Start the game now', 'Quit') "Don't ask about precompiling again"
		if ($never) { Set-Content $noPrompt 'run-windows.ps1 -Prompt: no precompile dialog (delete this file to get it back)' }
		if ($choice -ne 0 -and $choice -ne 1) { Write-Host 'cancelled'; return }
		if ($choice -eq 0) {
			try {
				& "$PSScriptRoot\precompile-windows.ps1" -Game $Game -Affinity $mask
				$inputsReady = $true # the full run ends with them
			} catch {
				Write-Host "precompile failed ($($_.Exception.Message)); starting the game anyway"
			}
		}
	} else {
		$choice, $never = Show-Choice $info @('Start the game', 'Quit')
		if ($choice -ne 0) { Write-Host 'cancelled'; return }
	}
}
# Memory: Windows ends a program that asks for more than RAM and the page file can hold. The emulator
# commits about 34 GB (the game's 13.5 GiB of PS5 memory, about 11 GB Windows sets aside to back the
# video memory in use, the emulator's own) and keeps about 20 GB in RAM.
$os = Get-CimInstance Win32_OperatingSystem
[double]$ram = $os.TotalVisibleMemorySize * 1KB
[double]$commit = $os.FreeVirtualMemory * 1KB
if ((Get-CimInstance Win32_ComputerSystem).AutomaticManagedPagefile) {
	# A system-managed page file grows to 3 x RAM (at most an eighth of its disk) while the disk has room.
	$disk = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$env:SystemDrive'"
	[double]$paged = (Get-CimInstance Win32_PageFileUsage | Measure-Object AllocatedBaseSize -Sum).Sum * 1MB
	[double]$room = [math]::Min([math]::Min(3 * $ram, [double]$disk.Size / 8) - $paged, [double]$disk.FreeSpace - 5GB)
	$commit += [math]::Max([double]0, $room)
}
$memory = @()
if ($ram -lt 24GB) {
	$memory += "This PC has {0:N0} GB of RAM and the emulator keeps about 20 GB in use (32 GB recommended): expect long stutters." -f ($ram / 1GB)
}
if ($commit -lt 34GB) {
	$memory += ("Windows can give programs only {0:N0} GB more memory (RAM plus page file) and the emulator needs about 34 GB: " +
		"it may close in the middle of the game. Close other programs, or set the page file to 'System managed size' " +
		"(Settings > System > About > Advanced system settings > Performance > Settings > Advanced > Virtual memory).") -f ($commit / 1GB)
}
foreach ($line in $memory) { Write-Host "memory:   $line" }
if ($memory -and $Prompt) {
	$choice, $never = Show-Choice ($memory -join "`n`n") @('Start the game anyway', 'Quit')
	if ($choice -ne 0) { Write-Host 'cancelled'; return }
}

if ($shaders -and !$inputsReady) {
	Write-Host 'shaders:  making the background shader preparation inputs for this graphics card (first run or after a driver update: about a minute)'
	if ((Invoke-Tool @('--no-pipelines', '--static-inputs', '--threads', "$cpus")) -ne 0) {
		Write-Host 'shaders:  making them failed; no background shader preparation this time'
	}
}

foreach ($entry in $environment.GetEnumerator()) { Set-Item "env:$($entry.Key)" $entry.Value }
New-Item -ItemType Directory -Force $logDir | Out-Null
$out = "$logDir\$stamp.out.log"
$process = Start-Process -FilePath $Exe -ArgumentList $quoted -WorkingDirectory $PSScriptRoot -PassThru `
	-RedirectStandardOutput $out -RedirectStandardError "$logDir\$stamp.err.log"
if ($mask -ne $all) { $process.ProcessorAffinity = [IntPtr]$mask }
$null = $process.Handle # keeps the exit code readable after the process ends
Write-Host "pid $($process.Id); logs: $($logDir.Substring($PSScriptRoot.Length + 1))\$stamp.*.log"
if (!$Precompile) { return }

# Precompile: follow the warmup progress until the emulator exits.
$begin = Get-Date
$shown = 0
while (!$process.HasExited) {
	Start-Sleep -Milliseconds 500
	$lines = @(Get-Content $out -ErrorAction SilentlyContinue | Where-Object { $_ -match 'warmup|precompile|pipeline cache' })
	for (; $shown -lt $lines.Count; $shown++) { Write-Host "  $($lines[$shown])" }
}
$lines = @(Get-Content $out -ErrorAction SilentlyContinue | Where-Object { $_ -match 'warmup|precompile|pipeline cache' })
for (; $shown -lt $lines.Count; $shown++) { Write-Host "  $($lines[$shown])" }
Write-Host ("exit code {0} after {1:N0} s" -f $process.ExitCode, ((Get-Date) - $begin).TotalSeconds)
exit $process.ExitCode
