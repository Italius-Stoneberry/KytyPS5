# Static shader and pipeline precompile (tools\local\static-precompile), a program of its own:
#   .\precompile-windows.ps1              every shader and pipeline of the game into the static pipeline
#                                         cache _PipelineCache\static\<title>.bin, which the emulator looks
#                                         up before compiling (hours the first time: run it overnight)
#   .\precompile-windows.ps1 -Jobs 8      with 8 processes (default: 3 threads each on the allowed CPUs)
#   .\precompile-windows.ps1 -Coverage    no pipelines, only what the seeds compile to, for
#                                         tools\local\static-precompile\precompile.py coverage
#   .\precompile-windows.ps1 -InputsOnly  only the compiled inputs the emulator's shader prefetch
#                                         translates in the background (_PipelineCache\static\<title>.shaders;
#                                         every full run writes them too)
# The NVIDIA driver compiles big compute shaders nearly one at a time per process, so the work is split
# into shards, a below-normal-priority process each (kyty_shader_precompile --shard i/n), whose caches
# are merged into the static cache at the end. An interrupted run resumes: the shards' checkpoints
# (every ten minutes) are merged first, and what the static cache holds is not compiled again. Build the
# program with build-windows.cmd kyty_shader_precompile. A portable package (package-windows.ps1) has the
# program, the seed file and launch.json next to this script (precompile.cmd).
param(
	[string]$Game = '',
	[string]$Seeds = $(if (Test-Path "$PSScriptRoot\seeds.seeds") { "$PSScriptRoot\seeds.seeds" } else { "$PSScriptRoot\_Build\static-precompile\seeds.seeds" }),
	[int]$Jobs = 0,
	[int]$Threads = 3,
	[int64]$Affinity = 0, # 0: the launch config's CPUs (this PC's leave out 4 and 5, where the compiler crashes), else all
	[switch]$Coverage,
	[switch]$InputsOnly,
	[string]$Exe = $(if (Test-Path "$PSScriptRoot\kyty_shader_precompile.exe") { "$PSScriptRoot\kyty_shader_precompile.exe" } else { "$PSScriptRoot\_Build\windows\kyty_shader_precompile.exe" })
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path $Exe)) { throw "missing $Exe; build it with build-windows.cmd kyty_shader_precompile" }
# The game run-windows.ps1 was given last, else the default folder.
if (!$Game -and (Test-Path "$PSScriptRoot\game-path.txt")) { $Game = (Get-Content "$PSScriptRoot\game-path.txt" -Raw).Trim() }
if (!$Game) { $Game = "$env:USERPROFILE\Documents\PPSA01341-app0" }
if (!(Test-Path "$Game\sce_sys\param.json")) { throw "no sce_sys\param.json in $Game (-Game <folder>, or start the game once to choose it)" }
if (!(Test-Path $Seeds)) {
	# Every shader the game ships, with the pipelines it draws them with (from the game files).
	New-Item -ItemType Directory -Force (Split-Path $Seeds) | Out-Null
	python "$PSScriptRoot\tools\local\static-precompile\precompile.py" --game $Game seeds $Seeds
	if ($LASTEXITCODE) { throw 'precompile.py seeds failed' }
}
if ($Affinity -eq 0) {
	$config = if (Test-Path "$PSScriptRoot\launch.json") { "$PSScriptRoot\launch.json" } else { "$PSScriptRoot\_Build\release-stage1-20260927\launch.json" }
	foreach ($cpu in (Get-Content $config -Raw | ConvertFrom-Json).cpu_affinity) { $Affinity = $Affinity -bor ([int64]1 -shl [int]$cpu) }
}
$all = if ([Environment]::ProcessorCount -ge 64) { [int64]-1 } else { ([int64]1 -shl [Environment]::ProcessorCount) - 1 }
$mask = $Affinity -band $all
if ($mask -eq 0) { $mask = $all }
$cpus = 0
for ($bit = 0; $bit -lt 64; $bit++) { if ($mask -band ([int64]1 -shl $bit)) { $cpus++ } }
# A process scales to a few threads (4: 85%), and each translates the programs its share needs.
if ($Jobs -le 0) { $Jobs = [math]::Max(1, [math]::Ceiling($cpus / $Threads)) }
$logs = if (Test-Path "$PSScriptRoot\_Build") { "$PSScriptRoot\_Build\run-logs" } else { "$PSScriptRoot\logs" }
$logName = $logs.Substring($PSScriptRoot.Length + 1)
New-Item -ItemType Directory -Force $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$begin = Get-Date

function Start-Precompile([string]$name, [string[]]$arguments) {
	$process = Start-Process -FilePath $Exe -ArgumentList (@('--game', "`"$Game`"") + $arguments) -NoNewWindow -PassThru `
		-WorkingDirectory $PSScriptRoot -RedirectStandardOutput "$logs\$stamp-precompile-$name.out.log" `
		-RedirectStandardError "$logs\$stamp-precompile-$name.err.log"
	$process.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal
	$process.ProcessorAffinity = [IntPtr]$mask
	$null = $process.Handle # keeps the exit code readable after the process ends
	$process
}
function Wait-Precompile([System.Diagnostics.Process[]]$processes, [string]$what) {
	$shown = Get-Date
	while (($left = @($processes | Where-Object { !$_.HasExited }).Count)) {
		Start-Sleep -Seconds 1
		if (((Get-Date) - $shown).TotalSeconds -lt 60) { continue }
		$shown = Get-Date
		Write-Host ("  {0:hh\:mm\:ss} {1}: {2} of {3} running" -f ((Get-Date) - $begin), $what, $left, $processes.Count)
	}
	$failed = @($processes | Where-Object { $_.ExitCode -ne 0 }).Count
	if ($failed) { throw "$what`: $failed process(es) failed; logs: $logName\$stamp-precompile-*" }
}

Write-Host "precompile: $Seeds, $Jobs shards, affinity 0x$('{0:X}' -f $mask); logs $logName\$stamp-precompile-*"
if ($Coverage) {
	$out = [IO.Path]::ChangeExtension($Seeds, '.compiled.shaders')
	Wait-Precompile @(Start-Precompile 'coverage' @('--seeds', "`"$Seeds`"", '--no-pipelines', '--threads', "$cpus",
		'--out', "`"$out`"")) 'coverage'
	Write-Host "wrote $out"
	return
}
# The compiled inputs for the emulator's shader prefetch: the seeds translated as the full run does,
# without pipelines (half a minute).
$inputs = @('--seeds', "`"$Seeds`"", '--no-pipelines', '--threads', "$cpus", '--static-inputs')
if ($InputsOnly) {
	Wait-Precompile @(Start-Precompile 'inputs' $inputs) 'inputs'
	Write-Host 'wrote _PipelineCache\static\<title>.shaders'
	return
}
# Checkpoints of an interrupted run first: what they hold is not compiled again.
Wait-Precompile @(Start-Precompile 'merge-before' @('--merge')) 'merge'
$workers = for ($i = 0; $i -lt $Jobs; $i++) {
	Start-Precompile "shard$i" @('--seeds', "`"$Seeds`"", '--shard', "$i/$Jobs", '--threads', "$Threads")
}
Wait-Precompile $workers 'shards'
Wait-Precompile @(Start-Precompile 'merge' @('--merge')) 'merge'
Wait-Precompile @(Start-Precompile 'inputs' $inputs) 'inputs'
$cache = Get-ChildItem "$PSScriptRoot\_PipelineCache\static\*.bin" | Sort-Object LastWriteTime | Select-Object -Last 1
Write-Host ("done in {0:hh\:mm\:ss}: {1} ({2:N0} MB)" -f ((Get-Date) - $begin), $cache.FullName, ($cache.Length / 1MB))
