@echo off
setlocal
REM ===========================================================================
REM  build.cmd  -  standalone build script (no vcvars, no reg.exe)
REM
REM  usage:
REM      build.cmd            build both targets
REM      build.cmd mock       build desktop overlay  PhysxPUBGMock.exe
REM      build.cmd aurora     build launcher anim    AuroraLauncher.exe
REM
REM  layout:
REM      core\          source (mock + ue4math + util)
REM      build\         obj / pdb
REM      *.exe          output, sitting next to this script
REM ===========================================================================

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "SDKI=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
set "SDKL=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"

set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDKI%\ucrt;%SDKI%\um;%SDKI%\shared;%SDKI%\winrt;%SDKI%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDKL%\ucrt\x64;%SDKL%\um\x64"

if not exist "%ROOT%\build" mkdir "%ROOT%\build"

set CFLAGS=/nologo /std:c++20 /EHsc /O2 /utf-8 /MD /W3 /wd4996 /wd4828
set INCS=/I "%ROOT%\core" /I "%ROOT%\core\mock" /I "%ROOT%"
set MATHSRC="%ROOT%\core\ue4math\vector.cpp" "%ROOT%\core\ue4math\matrix.cpp" "%ROOT%\core\ue4math\quat.cpp" "%ROOT%\core\ue4math\rotator.cpp" "%ROOT%\core\ue4math\transform.cpp" "%ROOT%\core\util\VectorHelper.cpp"
set LIBS=/link user32.lib gdi32.lib dwmapi.lib winmm.lib

set TARGET=%~1
if "%TARGET%"=="" set TARGET=all

if /i "%TARGET%"=="aurora" goto build_aurora

:build_mock
echo [build] PhysxPUBGMock.exe ...
cl %CFLAGS% /FI"%ROOT%\core\mock\WinPre.h" %INCS% /Fo:"%ROOT%\build\\" /Fd:"%ROOT%\build\mock.pdb" /Fe:"%ROOT%\PhysxPUBGMock.exe" "%ROOT%\core\mock\main_mock.cpp" %MATHSRC% %LIBS%
if errorlevel 1 (
  echo [FAIL] PhysxPUBGMock
  exit /b 1
)
echo [ ok ] PhysxPUBGMock.exe
if /i "%TARGET%"=="mock" goto done

:build_aurora
if not exist "%ROOT%\core\mock\main_launcher.cpp" (
  echo [skip] core\mock\main_launcher.cpp not found
  goto done
)
echo [build] AuroraLauncher.exe ...
cl %CFLAGS% /FI"%ROOT%\core\mock\WinPre.h" %INCS% /Fo:"%ROOT%\build\\" /Fd:"%ROOT%\build\aurora.pdb" /Fe:"%ROOT%\AuroraLauncher.exe" "%ROOT%\core\mock\main_launcher.cpp" %MATHSRC% %LIBS%
if errorlevel 1 (
  echo [FAIL] AuroraLauncher
  exit /b 1
)
echo [ ok ] AuroraLauncher.exe

:done
echo.
echo BUILD_DONE
endlocal
