@echo off
rem Windows build: clang-cl + Ninja in the Visual Studio 2022 x64 environment.
rem   build-windows.cmd            configure (first time) and build kyty_emulator
rem   build-windows.cmd <target>   build another target (for example all)
rem Release builds use -O3 -march=native and ThinLTO, like the Linux release; a PGO profile
rem is used when _Build\pgo\windows\kyty.profdata exists (see tools\local\PGO.md).
rem   KYTY_BUILD_DIR    build directory (default _Build\windows)
rem   KYTY_CMAKE_ARGS   extra arguments for the first configure (e.g. -DKYTY_PGO_GENERATE=ON)
setlocal
rem On this machine the compiler crashes at random when it runs on CPUs 4/5 (the Linux
rem launch configs leave them out as well): rerun the whole build without them.
rem Set KYTY_BUILD_AFFINITY=FFFFFF to use every CPU.
if not defined KYTY_BUILD_AFFINITY set "KYTY_BUILD_AFFINITY=FFFFCF"
if not defined KYTY_BUILD_PINNED (
	set "KYTY_BUILD_PINNED=1"
	start "" /wait /b /affinity %KYTY_BUILD_AFFINITY% cmd /c "%~f0" %*
	exit /b
)
set "ROOT=%~dp0"
set "BUILD=%ROOT%_Build\windows"
if defined KYTY_BUILD_DIR set "BUILD=%KYTY_BUILD_DIR%"
set "PROFILE=%ROOT%_Build\pgo\windows\kyty.profdata"
set "PGO_ARGS="
if exist "%PROFILE%" set "PGO_ARGS=-DKYTY_PGO_USE=%PROFILE%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
	echo Visual Studio 2022 not found.
	exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem clang-cl: LLVM from winget (C:\Program Files\LLVM) or the VS "C++ Clang tools" component.
if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang-cl >nul 2>nul || (
	echo clang-cl not found: install LLVM ^(winget install LLVM.LLVM^).
	exit /b 1
)

rem glslangValidator: Vulkan SDK (winget install KhronosGroup.VulkanSDK).
if not defined VULKAN_SDK for /d %%d in ("%SystemDrive%\VulkanSDK\*") do set "VULKAN_SDK=%%d"
if defined VULKAN_SDK set "PATH=%VULKAN_SDK%\Bin;%PATH%"
where glslangValidator >nul 2>nul || (
	echo glslangValidator not found: install the Vulkan SDK ^(winget install KhronosGroup.VulkanSDK^).
	exit /b 1
)

if not exist "%BUILD%\build.ninja" (
	cmake -S "%ROOT%." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
		-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DKYTY_BUILD_LAUNCHER=OFF ^
		-DFETCHCONTENT_SOURCE_DIR_XBYAK="%ROOT%_Build\deps\xbyak-src" ^
		-DFETCHCONTENT_SOURCE_DIR_ZYDIS="%ROOT%_Build\deps\zydis-src" %PGO_ARGS% %KYTY_CMAKE_ARGS% || exit /b 1
)

rem A profile that appeared after the first configure (an instrumented build never uses one).
if defined PGO_ARGS (
	findstr /c:"KYTY_PGO_GENERATE:BOOL=ON" "%BUILD%\CMakeCache.txt" >nul 2>nul || (
		findstr /c:"KYTY_PGO_USE:FILEPATH=%PROFILE:\=/%" "%BUILD%\CMakeCache.txt" >nul 2>nul || (
			cmake -B "%BUILD%" %PGO_ARGS% >nul || exit /b 1
		)
	)
)

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=kyty_emulator"
cmake --build "%BUILD%" --target %TARGET% || exit /b 1
