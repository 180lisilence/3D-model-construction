@echo off
REM ===========================================================================
REM  start_overlay.cmd  -  run the desktop overlay
REM
REM  double-click : transparent overlay on top of the desktop
REM                 middle-drag  = orbit camera
REM                 F8           = quit
REM ===========================================================================
setlocal
cd /d "%~dp0"

start "" "PhysxPUBGMock.exe" %1 %2 %3 %4 %5 %6 %7 %8 %9

endlocal
