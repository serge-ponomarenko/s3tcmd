@echo off
rem Copies the built plugin to the development install location used by Total Commander.
rem Total Commander keeps the DLL loaded: close TC before running this script.
setlocal
set "DEST=%APPDATA%\GHISLER\plugins\wfx\s3tcmd"
if not exist "%~dp0build\s3tcmd.wfx64" goto no_build
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0build\s3tcmd.wfx64" "%DEST%\" >nul || goto copy_failed
copy /y "%~dp0build\s3tcmd-clip.exe" "%DEST%\" >nul || goto copy_failed
copy /y "%~dp0build\pluginst.inf" "%DEST%\" >nul
echo Installed to "%DEST%\s3tcmd.wfx64"
exit /b 0

:no_build
echo build\s3tcmd.wfx64 not found - run build.bat first
exit /b 1

:copy_failed
echo Copy failed - is Total Commander still running?
exit /b 1
