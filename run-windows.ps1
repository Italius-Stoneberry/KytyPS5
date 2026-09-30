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
#   .\run-windows.ps1 -DryRun                print environment and command only
param(
	[string]$Config = "$PSScriptRoot\_Build\release-stage1-20260927\launch.json",
	[string]$Game = "$env:USERPROFILE\Documents\PPSA01341-app0",
	[string]$Exe = "$PSScriptRoot\_Build\windows\kyty_emulator.exe",
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
	[switch]$DryRun
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path $Exe)) { throw "missing $Exe; build it with build-windows.cmd" }
if (!(Test-Path "$Game\eboot.bin")) { throw "no eboot.bin in $Game" }

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

# CPU affinity of the Linux config (it also leaves CPUs 4 and 5 out).
$mask = [int64]0
foreach ($cpu in $launch.cpu_affinity) { $mask = $mask -bor ([int64]1 -shl [int]$cpu) }
if ($mask -eq 0) { $mask = [int64]0xFFFFCF }
$cpus = 0
for ($bit = 0; $bit -lt 64; $bit++) { if ($mask -band ([int64]1 -shl $bit)) { $cpus++ } }

# The render thread on the recording worker's P-cores (without CPU 0, which takes most
# interrupts on Windows): +2.5% over free placement in the fixed scene. Pinning it to CPU 0
# alone, as on Linux, halved the frame rate here.
if (!$environment.Contains('KYTY_RENDER_CPUS')) {
	$renderCpus = if ($environment.Contains('KYTY_RECORDING_CPUS')) { $environment['KYTY_RECORDING_CPUS'] } else { '1,2,3,6,7' }
	$environment['KYTY_RENDER_CPUS'] = $renderCpus
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
# The executable's SHA-256 scopes the driver pipeline cache (_PipelineCache\local\<sha>) and
# enables the shader warmup; without it a modified source tree runs with both disabled.
$environment['KYTY_DRIVER_CACHE_KEY'] = (Get-FileHash -Algorithm SHA256 $Exe).Hash.ToLowerInvariant()
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

$quoted = @('--game', "`"$Game`"") + ($options | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
$logDir = "$PSScriptRoot\_Build\run-logs"
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
Write-Host "config:   $Config$(if ($Baseline) { ' (baseline: no switches)' })$(if ($Precompile) { ' (precompile)' })"
Write-Host ("affinity: 0x{0:X} ({1} CPUs)" -f $mask, $cpus)
Write-Host "switches: $($environment.Count)"
Write-Host "command:  $Exe $($quoted -join ' ')"
if ($DryRun) { $environment.GetEnumerator() | ForEach-Object { "  $($_.Key)=$($_.Value)" }; return }

foreach ($entry in $environment.GetEnumerator()) { Set-Item "env:$($entry.Key)" $entry.Value }
New-Item -ItemType Directory -Force $logDir | Out-Null
$out = "$logDir\$stamp.out.log"
$process = Start-Process -FilePath $Exe -ArgumentList $quoted -WorkingDirectory $PSScriptRoot -PassThru `
	-RedirectStandardOutput $out -RedirectStandardError "$logDir\$stamp.err.log"
$process.ProcessorAffinity = [IntPtr]$mask
$null = $process.Handle # keeps the exit code readable after the process ends
Write-Host "pid $($process.Id); logs: _Build\run-logs\$stamp.*.log"
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
