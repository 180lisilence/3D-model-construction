@echo off
REM ===========================================================================
REM  start_launcher.cmd  -  run the boot animation
REM
REM  double-click           : fullscreen, 9 seconds, default aurora
REM  start_launcher.cmd -w  : windowed 1280x720 (handy for tuning)
REM
REM  Extra args are passed through to AuroraLauncher.exe, for example:
REM     start_launcher.cmd --seconds 6 --aurora 1.0 --aurora-speed 2
REM ===========================================================================
setlocal
cd /d "%~dp0"

if /i "%~1"=="-w" (
    shift
    "AuroraLauncher.exe" --windowed --size 1280x720 %1 %2 %3 %4 %5 %6 %7 %8 %9
) else (
    "AuroraLauncher.exe" %1 %2 %3 %4 %5 %6 %7 %8 %9
)

endlocal
