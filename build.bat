@echo off
rem Builds s3tcmd.wfx64, unit tests, integration tests and tools with MSVC (x64), runs unit tests.
rem A release build also writes the install package build\s3tcmd-<version>.zip (TC auto-install).
rem Usage: build.bat [debug] [it] [itw]
rem   debug  - unoptimized build with the debug CRT (no package)
rem   it     - also run the read-only integration tests against real AWS
rem   itw    - also run the write integration tests (unique prefix in the test bucket, cleaned up)
rem Integration tests read their settings from environment variables (see tests\integration*.cpp).
rem If tests\it.local.bat exists (not in git), it is called first and can set them.
rem MSVC: an environment from a Developer Command Prompt is used as is; otherwise the newest
rem Visual Studio (2019 or later, with the C++ x64 tools) is found with vswhere.
setlocal

rem Note: no parenthesized blocks around paths with "(x86)" - the parenthesis breaks them.
if defined VSCMD_VER goto have_env
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
set "VSDIR="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" goto no_vs
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto no_vs
call "%VCVARS%" >nul || exit /b 1
:have_env

cd /d "%~dp0"
if not exist build\obj\lib mkdir build\obj\lib
if not exist build\obj\bin mkdir build\obj\bin

set "MODE=release"
set "RUN_IT=0"
set "RUN_ITW=0"
:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="debug" set "MODE=debug"
if /i "%~1"=="it" set "RUN_IT=1"
if /i "%~1"=="itw" set "RUN_ITW=1"
shift
goto parse_args
:args_done

set "CFLAGS=/nologo /std:c++17 /EHsc /W4 /utf-8 /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Z7"
if "%MODE%"=="debug" goto debug_flags
set "CFLAGS=%CFLAGS% /O2 /MT"
goto flags_done
:debug_flags
set "CFLAGS=%CFLAGS% /Od /MTd"
:flags_done

set "SOURCES=src\util.cpp src\log.cpp src\process.cpp src\config.cpp src\aws_cli.cpp src\aws_s3.cpp src\names.cpp src\vfs_path.cpp src\errors.cpp src\dir_cache.cpp src\dir_tree.cpp src\progress.cpp src\transfer.cpp src\disk_cache.cpp src\progress_window.cpp src\clip_paths.cpp src\s3_uri.cpp src\setup.cpp"
set "LIBS=user32.lib shell32.lib comctl32.lib gdi32.lib"
rem Embed only the PDB file name, not the local build path, in the binaries.
set "LFLAGS=/PDBALTPATH:%%_PDB%%"

echo === core
cl %CFLAGS% /c /Fobuild\obj\lib\ %SOURCES% || exit /b 1

echo === plugin
rc /nologo /fo build\obj\bin\s3tcmd.res res\s3tcmd.rc || exit /b 1
cl %CFLAGS% /LD /Fobuild\obj\bin\ src\plugin.cpp build\obj\lib\*.obj build\obj\bin\s3tcmd.res /link %LFLAGS% /DEF:src\s3tcmd.def /OUT:build\s3tcmd.wfx64 /IMPLIB:build\obj\bin\s3tcmd.lib %LIBS% || exit /b 1

echo === clipboard helper
cl %CFLAGS% /Fobuild\obj\bin\ src\clip_main.cpp build\obj\lib\*.obj build\obj\bin\s3tcmd.res /link %LFLAGS% /SUBSYSTEM:WINDOWS /OUT:build\s3tcmd-clip.exe %LIBS% || exit /b 1

echo === tests
cl %CFLAGS% /Fobuild\obj\bin\ tests\test_main.cpp build\obj\lib\*.obj /link /OUT:build\tests.exe %LIBS% || exit /b 1
cl %CFLAGS% /Fobuild\obj\bin\ tests\integration.cpp build\obj\lib\*.obj /link /OUT:build\integration.exe %LIBS% || exit /b 1
cl %CFLAGS% /Fobuild\obj\bin\ tests\integration_write.cpp build\obj\lib\*.obj /link /OUT:build\integration_write.exe %LIBS% || exit /b 1

echo === tools
cl %CFLAGS% /Fobuild\obj\bin\ tools\wfxhost.cpp /link /OUT:build\wfxhost.exe || exit /b 1

echo === unit tests
if exist tests\it.local.bat call tests\it.local.bat
build\tests.exe || exit /b 1
if "%RUN_IT%"=="0" goto skip_it
echo === integration tests (read-only, real AWS)
build\integration.exe || exit /b 1
:skip_it
if "%RUN_ITW%"=="0" goto done
echo === integration tests (write, real AWS, unique prefix)
build\integration_write.exe || exit /b 1

:done
call :pluginst_inf || exit /b 1
if "%MODE%"=="debug" goto built
call :package || exit /b 1
echo === OK: build\s3tcmd.wfx64, build\s3tcmd-%VERSION%.zip
exit /b 0
:built
echo === OK: build\s3tcmd.wfx64 (debug, no package)
exit /b 0

rem build\pluginst.inf from src\version.h. TC reads [plugininstall] (keys type, file, defaultdir,
rem description, version) when an archive with this file in its root is opened with Enter.
:pluginst_inf
set "VERSION="
for /f "tokens=3" %%v in ('findstr /b /c:"#define S3TCMD_VERSION_STR" src\version.h') do set "VERSION=%%~v"
if "%VERSION%"=="" goto no_version
> build\pluginst.inf echo [plugininstall]
>> build\pluginst.inf echo description=AWS S3 file system via AWS CLI: browse, copy, upload, delete, s3:// paths
>> build\pluginst.inf echo type=wfx
>> build\pluginst.inf echo file=s3tcmd.wfx64
>> build\pluginst.inf echo defaultdir=s3tcmd
>> build\pluginst.inf echo version=%VERSION%
exit /b 0

rem build\s3tcmd-<version>.zip, all files in the root. TC copies every file of the archive
rem (except pluginst.inf) into the plugin folder (README, LICENSE and notices included). tar.exe is part of Windows 10/11.
:package
echo === package %VERSION%
if exist build\pkg rmdir /s /q build\pkg
mkdir build\pkg || exit /b 1
copy /y build\pluginst.inf build\pkg\ >nul || exit /b 1
copy /y build\s3tcmd.wfx64 build\pkg\ >nul || exit /b 1
copy /y build\s3tcmd-clip.exe build\pkg\ >nul || exit /b 1
copy /y README.md build\pkg\ >nul || exit /b 1
copy /y LICENSE build\pkg\ >nul || exit /b 1
copy /y THIRD_PARTY_NOTICES.md build\pkg\ >nul || exit /b 1
if exist "build\s3tcmd-%VERSION%.zip" del "build\s3tcmd-%VERSION%.zip"
"%SystemRoot%\System32\tar.exe" -a -c -f "build\s3tcmd-%VERSION%.zip" -C build\pkg pluginst.inf s3tcmd.wfx64 s3tcmd-clip.exe README.md LICENSE THIRD_PARTY_NOTICES.md || exit /b 1
exit /b 0

:no_version
echo S3TCMD_VERSION_STR not found in src\version.h
exit /b 1

:no_vs
echo Visual Studio with the C++ x64 build tools not found (vswhere). Install VS 2019+ Build Tools
echo with "Desktop development with C++", or run this script from a Developer Command Prompt.
exit /b 1
