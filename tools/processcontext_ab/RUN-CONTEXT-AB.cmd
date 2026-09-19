@echo off
rem ===========================================================================
rem  RUN-CONTEXT-AB.cmd
rem
rem  Runs the PROCESSCONTEXT-AB arms, EACH IN A NEW PROCESS.
rem
rem      RUN-CONTEXT-AB.cmd [path\to\nvngx_dlssnr.dll] [arm arm ...]
rem
rem  NO ARM NAMES  - the documented invocation, and the only one a human needs:
rem                  the four arms in the fixed order, then the fixed result
rem                  table. This behaviour is unchanged.
rem
rem      RUN-CONTEXT-AB.cmd
rem      RUN-CONTEXT-AB.cmd "C:\...\nvngx_dlssnr.dll"
rem
rem  ARM NAMES GIVEN - only those arms run, and the fixed table is NOT printed
rem                  because there is nothing to compare yet. This exists so an
rem                  orchestrator can enforce SINGLE as a reference GATE: run
rem                  single, read its result, and only then decide whether the
rem                  other three are worth a process each. It does not change
rem                  what any arm does.
rem
rem      RUN-CONTEXT-AB.cmd "" single
rem      RUN-CONTEXT-AB.cmd "C:\...\nvngx_dlssnr.dll" single
rem      RUN-CONTEXT-AB.cmd "C:\...\nvngx_dlssnr.dll" dual-held dual-active
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
rem  NOTE ON STYLE. This script uses goto labels rather than parenthesised
rem  if-blocks for the two run paths, and no echo text inside a block contains a
rem  parenthesis. cmd.exe closes a parenthesised block at the first unescaped
rem  ")", including one inside an echo - which silently truncates the block and
rem  lets the other branch run. That is not a cosmetic concern: it printed the
rem  table AND the explicit-arm-list notice in the same run.
rem
rem  Nothing is deployed and nothing is modified: the copy is made here, used,
rem  and removed. Cyberpunk is never launched.
rem ===========================================================================

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "EXE=processcontext_ab.exe"
set "RUNNAME=nvngx.dll_processcontext_ab.exe"

rem  ---- arguments: %1 = the NR DLL, %2..%9 = an optional arm list ---------
set "NRDLL=%~1"
if "%NRDLL%"=="" set "NRDLL=nvngx_dlssnr.dll"
set "FIRSTARM=%~2"

set "ARMS="
shift
:pcab_collect
if "%~1"=="" goto :pcab_collected
set "ARMS=%ARMS% %~1"
shift
goto :pcab_collect
:pcab_collected

set "TITLE=four arms, one process each"
set "MODELINE=all four, fixed order, then the fixed table"
set "EXPLICIT="
if not "%FIRSTARM%"=="" set "TITLE=selected arms, one process each"
if not "%FIRSTARM%"=="" set "MODELINE=%ARMS%"
if not "%FIRSTARM%"=="" set "EXPLICIT=1"
if not "%FIRSTARM%"=="" set "MODELINE=explicit:%ARMS% - the fixed table is NOT printed"

echo ================================================================================
echo PROCESSCONTEXT-AB  -  %TITLE%
echo ================================================================================
echo   working directory : %CD%
echo   exe               : %EXE%
echo   run-as name       : %RUNNAME%   carries the nvngx.dll caller-gate prefix
echo   NR DLL            : %NRDLL%
echo   arms              : %MODELINE%
echo ================================================================================
echo.

if not exist "%EXE%" goto :pcab_noexe
if not exist "%NRDLL%" goto :pcab_nodll

copy /y "%EXE%" "%RUNNAME%" >nul
if errorlevel 1 goto :pcab_nocopy

set "FAILED=0"
set "TRC=0"

if not "%EXPLICIT%"=="" goto :pcab_explicit

rem  THE DOCUMENTED INVOCATION. Unchanged: the four arms in the fixed order,
rem  then the fixed table.
for %%M in (single dual-held dual-active dual-released) do call :pcab_arm %%M
echo.
echo --------------------------------------------------------------------------------
echo  TABLE   -  reads context-single.log .. context-dual-released.log
echo --------------------------------------------------------------------------------
"%EXE%" --mode table
set "TRC=!ERRORLEVEL!"
goto :pcab_finish

:pcab_explicit
rem  An explicit arm list. The table is not printed: it compares four arms and
rem  only some of them were asked for.
for %%M in (%ARMS%) do call :pcab_arm %%M
echo.
echo [PCAB] an explicit arm list was given, so the fixed table is not printed.
echo [PCAB] run this script with no arm names to get the table.

:pcab_finish
del /q "%RUNNAME%" >nul 2>&1

echo.
echo ================================================================================
echo PROCESSCONTEXT-AB  -  finished
echo ================================================================================
echo   per-arm logs : context-single.log  context-dual-held.log
echo                  context-dual-active.log  context-dual-released.log
if not "%FAILED%"=="0" echo   NOTE: at least one arm exited non-zero - read that arm's log.
echo   exit code    : %TRC%
echo                   table mode: 0 = a verdict was reached, 2 = INVALID under
echo                   rule E, 4 = incomplete, no verdict possible
echo.
echo   Cyberpunk was NOT launched and nothing was deployed.
echo.

endlocal & exit /b %TRC%

:pcab_noexe
echo ERROR: "%EXE%" is not beside this script.
echo        This script runs the built diagnostic; it does not build it.
endlocal & exit /b 1

:pcab_nodll
echo ERROR: the NR DLL "%NRDLL%" was not found.
echo        Pass its full path as the first argument:
echo            RUN-CONTEXT-AB.cmd "C:\path\to\nvngx_dlssnr.dll"
endlocal & exit /b 1

:pcab_nocopy
echo ERROR: could not create "%RUNNAME%".
endlocal & exit /b 1

rem ===========================================================================
rem  One arm, in its own process. %1 = the arm name.
rem ===========================================================================
:pcab_arm
echo.
echo --------------------------------------------------------------------------------
echo  ARM %1   -   new process
echo --------------------------------------------------------------------------------
"%RUNNAME%" --mode %1 --nr-dll "%NRDLL%"
set "RC=!ERRORLEVEL!"
echo   [arm %1 exited with !RC!]
if not "!RC!"=="0" set "FAILED=1"
exit /b 0
