@echo off
rem ===========================================================================
rem  RUN-CONTEXT-AB.cmd
rem
rem  Runs all four PROCESSCONTEXT-AB arms, EACH IN A NEW PROCESS, then prints the
rem  fixed result table and the fixed interpretation.
rem
rem      RUN-CONTEXT-AB.cmd [path\to\nvngx_dlssnr.dll]
rem
rem  The NR DLL defaults to nvngx_dlssnr.dll beside this script. It is gated on
rem  its SHA256 before it is loaded, so the wrong build cannot be measured by
rem  accident.
rem
rem  THE DATA PATH IS DERIVED, NOT SUPPLIED. The diagnostic takes the directory
rem  that holds --nr-dll - which is the directory NeuralScreen hands NGX - so
rem  this script never passes --data-path and must not need to. Pass the full
rem  path to the NeuralScreen runtime and the reference data path follows:
rem
rem      RUN-CONTEXT-AB.cmd "C:\...\neuralscreen-v1.15.0-full\native\nvngx_dlssnr.dll"
rem
rem  giving the data path "C:\...\neuralscreen-v1.15.0-full\native".
rem
rem  WHY THE EXECUTABLE IS COPIED BEFORE EACH RUN
rem      The DLSSNR snippet inspects its CALLER's module file name and refuses a
rem      caller whose name does not contain "nvngx.dll" with 0xBAD00002
rem      FAIL_PlatformError. The MGPU add-on answers this by deploying under a
rem      file name carrying that prefix; this script answers it the same way, by
rem      running the exe under such a name. Without it the SINGLE reference arm
rem      fails for a reason that has NOTHING to do with the hypothesis under
rem      test, which would make every run INVALID under rule E.
rem
rem  Nothing is deployed and nothing is modified: the copy is made here, used,
rem  and removed. Cyberpunk is never launched.
rem ===========================================================================

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "EXE=processcontext_ab.exe"
set "RUNNAME=nvngx.dll_processcontext_ab.exe"
set "NRDLL=%~1"
if "%NRDLL%"=="" set "NRDLL=nvngx_dlssnr.dll"

echo ================================================================================
echo PROCESSCONTEXT-AB  -  four arms, one process each
echo ================================================================================
echo   working directory : %CD%
echo   exe               : %EXE%
echo   run-as name       : %RUNNAME%   (carries the nvngx.dll caller-gate prefix)
echo   NR DLL            : %NRDLL%
echo ================================================================================
echo.

if not exist "%EXE%" (
  echo ERROR: "%EXE%" is not beside this script.
  echo        This script runs the built diagnostic; it does not build it.
  exit /b 1
)
if not exist "%NRDLL%" (
  echo ERROR: the NR DLL "%NRDLL%" was not found.
  echo        Pass its full path as the first argument:
  echo            RUN-CONTEXT-AB.cmd "C:\path\to\nvngx_dlssnr.dll"
  exit /b 1
)

copy /y "%EXE%" "%RUNNAME%" >nul
if errorlevel 1 (
  echo ERROR: could not create "%RUNNAME%".
  exit /b 1
)

set "FAILED=0"

for %%M in (single dual-held dual-active dual-released) do (
  echo.
  echo --------------------------------------------------------------------------------
  echo  ARM %%M   (new process)
  echo --------------------------------------------------------------------------------
  "%RUNNAME%" --mode %%M --nr-dll "%NRDLL%"
  set "RC=!ERRORLEVEL!"
  echo   [arm %%M exited with !RC!]
  if not "!RC!"=="0" set "FAILED=1"
)

echo.
echo --------------------------------------------------------------------------------
echo  TABLE   (reads context-single.log .. context-dual-released.log)
echo --------------------------------------------------------------------------------
"%EXE%" --mode table
set "TRC=!ERRORLEVEL!"

del /q "%RUNNAME%" >nul 2>&1

echo.
echo ================================================================================
echo PROCESSCONTEXT-AB  -  finished
echo ================================================================================
echo   per-arm logs : context-single.log  context-dual-held.log
echo                  context-dual-active.log  context-dual-released.log
if not "%FAILED%"=="0" echo   NOTE: at least one arm exited non-zero - read that arm's log.
echo   table exit   : %TRC%   (0 = a verdict was reached, 2 = INVALID under rule E,
echo                          4 = incomplete, no verdict possible)
echo.
echo   Cyberpunk was NOT launched and nothing was deployed.
echo.

endlocal & exit /b %TRC%
