# Portable package for other PCs: the emulator built for x86-64-v3 instead of -march=native (which
# also used this CPU's GFNI and AVX-VNNI: an illegal instruction on CPUs without them), the DLLs it
# needs, the launcher and a launch config without this PC's CPU layout, and the static precompile
# (its program, the seed file, precompile.cmd): run.cmd makes the shader prefetch's inputs for the
# GPU it runs on at the first launch, precompile.cmd the whole static pipeline cache (overnight).
#   .\package-windows.ps1                    build _Build\windows-portable, fill _Build\portable\KytyPS5
#   .\package-windows.ps1 -Out <folder>      another folder
# Not in it: the game, saves, shader caches (they are per GPU and driver), NVIDIA Streamline.
param(
	[string]$Out = "$PSScriptRoot\_Build\portable\KytyPS5",
	[string]$Config = "$PSScriptRoot\_Build\release-stage1-20260927\launch.json"
)
$ErrorActionPreference = 'Stop'
$build = "$PSScriptRoot\_Build\windows-portable"
$saved = $env:KYTY_BUILD_DIR, $env:KYTY_CMAKE_ARGS
$env:KYTY_BUILD_DIR = $build
$env:KYTY_CMAKE_ARGS = '-DKYTY_MARCH=x86-64-v3'
# Not through PowerShell's streams: the VS environment script writes harmless lines to stderr,
# which would stop this script.
foreach ($target in 'kyty_emulator', 'kyty_shader_precompile') {
	$process = Start-Process cmd "/c `"$PSScriptRoot\build-windows.cmd`" $target" -Wait -NoNewWindow -PassThru
	if ($process.ExitCode -ne 0) { break }
}
$env:KYTY_BUILD_DIR, $env:KYTY_CMAKE_ARGS = $saved
if ($process.ExitCode -ne 0) { throw 'build failed' }

New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item "$build\kyty_emulator.exe", "$build\kyty_shader_precompile.exe", "$build\libwinpthread-1.dll",
	"$PSScriptRoot\run-windows.ps1", "$PSScriptRoot\precompile-windows.ps1" $Out
# The seed file: every shader of the game with the pipelines it draws them with, from the game's files
# (tools\local\static-precompile\precompile.py seeds; its compute keys assume NVIDIA's 32-wide subgroups).
$seeds = "$PSScriptRoot\_Build\static-precompile\seeds.seeds"
if (Test-Path $seeds) { Copy-Item $seeds $Out } else { Write-Host "$seeds is missing: the package has no precompile" }
# The game's own specializations from recorded play (precompile.py recorded-seeds), precompiled with them.
$recorded = "$PSScriptRoot\_Build\static-precompile\recorded.seeds"
if (Test-Path $recorded) { Copy-Item $recorded $Out }
# The VC++ runtime next to the exe (Microsoft's redistributable files), so nothing has to be installed.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-Item "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" | Where-Object { $_.Parent.Parent.Name -match '^\d+\.' } |
	Sort-Object { [version]$_.Parent.Parent.Name } | Select-Object -Last 1
Copy-Item "$($crt.FullName)\msvcp140*.dll", "$($crt.FullName)\vcruntime140*.dll" $Out
# The SRT AOT library (built for x86-64-v3 already): the newest one made from this game's plans.
$aot = Get-ChildItem "$PSScriptRoot\_Build\srt-aot\windows-libraries\*\srt-aot.dll" -ErrorAction SilentlyContinue |
	Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($aot) { Copy-Item $aot.FullName $Out }

# The launch config minus this PC's CPUs (its affinity list, its P-cores as the render and recording
# CPUs: "auto" lets the launcher find the performance cores of the PC it runs on) and the Linux paths.
$launch = Get-Content $Config -Raw | ConvertFrom-Json
$command = @($launch.command)
$options = New-Object System.Collections.Generic.List[string]
for ($i = [Array]::IndexOf($command, '--') + 2; $i -lt $command.Count; $i++) {
	if ($command[$i] -eq '--game') { $i++; continue }
	$options.Add($command[$i])
}
$environment = [ordered]@{}
foreach ($property in $launch.environment.PSObject.Properties) {
	if ($property.Name -eq 'KYTY_RECORDING_CPUS') { $environment[$property.Name] = 'auto'; continue }
	if ($property.Name -eq 'KYTY_RENDER_CPUS') { continue }
	if ($property.Name -eq 'KYTY_SRT_AOT_LIBRARY') { if ($aot) { $environment[$property.Name] = 'srt-aot.dll' }; continue }
	$environment[$property.Name] = $property.Value
}
[ordered]@{ checkpoint = $launch.checkpoint; environment = $environment; command = @('--', 'kyty_emulator.exe') + $options } |
	ConvertTo-Json -Depth 3 | Set-Content "$Out\launch.json" -Encoding UTF8
# Double-click to play (PowerShell's script policy would stop run-windows.ps1 itself).
Set-Content "$Out\run.cmd" -Encoding ASCII -Value @(
	'@echo off',
	'rem Starts the game; the options are those of run-windows.ps1 (e.g. run.cmd -Fullscreen).',
	'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-windows.ps1" -Prompt %*',
	'if errorlevel 1 pause')
Copy-Item "$PSScriptRoot\docs\PORTABLE-README.md" "$Out\README.md"
Set-Content "$Out\precompile.cmd" -Encoding ASCII -Value @(
	'@echo off',
	'rem Every shader and pipeline of the game into the static pipeline cache: hours on a few cores (run it',
	'rem overnight, it resumes when interrupted). Options: those of precompile-windows.ps1.',
	'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0precompile-windows.ps1" %*',
	'pause')

$files = Get-ChildItem $Out -File | Where-Object { $_.Name -ne 'game-path.txt' }
$files | ForEach-Object { '{0,12:N0}  {1}' -f $_.Length, $_.Name }
'{0:N0} MB in {1}' -f (($files | Measure-Object Length -Sum).Sum / 1MB), $Out
