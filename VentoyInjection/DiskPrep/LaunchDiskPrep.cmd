@echo off
setlocal

set "ARCH=%PROCESSOR_ARCHITECTURE%"

if defined PROCESSOR_ARCHITEW6432 (
    set "ARCH=%PROCESSOR_ARCHITEW6432%"
)

set "DISKPREP="

if /I "%ARCH%"=="AMD64" set "DISKPREP=%~dp0DiskPrep_x64.exe"
if /I "%ARCH%"=="x86"   set "DISKPREP=%~dp0DiskPrep_x86.exe"

if not defined DISKPREP (
    echo [SKIP] Unsupported architecture: %ARCH%
    goto END
)

if not exist "%DISKPREP%" (
    echo [SKIP] DiskPrep executable not found: %DISKPREP%
    goto END
)

echo [RUN] %DISKPREP% /winsetup
"%DISKPREP%" /winsetup

set "RC=%ERRORLEVEL%"
echo [DONE] DiskPrep exit code: %RC%

:END
exit /b 0