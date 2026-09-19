@echo off
rem ===========================================================================
rem  RUN-AUTOLAB.cmd
rem
rem  The only command the user runs.
rem
rem      RUN-AUTOLAB.cmd
rem
rem  It executes the complete predefined experiment tree until one of four
rem  things happens - a decisive causal result, an invalid run, the tree
rem  exhausted, or an unexpected/mixed state - then restores the original add-on
rem  and writes results\<timestamp>\.
rem
rem  Arguments are passed straight through to autolab.py, so the same entry
rem  point also serves the maintenance modes:
rem
rem      RUN-AUTOLAB.cmd --list            show the graph and its rules
rem      RUN-AUTOLAB.cmd --parser-tests    fixtures only, no hardware
rem      RUN-AUTOLAB.cmd --dry-run         synthetic logs, no build/deploy/launch
rem      RUN-AUTOLAB.cmd --preflight-only  checks only, nothing launched
rem
rem  Cyberpunk is NOT launched unless safety.allow_game_launch is true in
rem  config.yaml, or --allow-game-launch is passed explicitly.
rem ===========================================================================

setlocal EnableExtensions
cd /d "%~dp0"

set "PY="
where py >nul 2>&1 && set "PY=py -3"
if not defined PY (
  where python >nul 2>&1 && set "PY=python"
)
if not defined PY (
  echo.
  echo ERROR: Python 3 was not found on PATH.
  echo        AutoLab needs Python 3.8 or newer, and uses only the standard
  echo        library. Install it, or run autolab.py with the interpreter you
  echo        already have.
  echo.
  exit /b 2
)

%PY% autolab.py %*
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
  echo [AUTOLAB] finished with exit code 0.
) else (
  echo [AUTOLAB] finished with exit code %RC%.
  echo [AUTOLAB] read the summary above and the results directory it named.
)
echo [AUTOLAB] if a run was performed, the original add-on was restored and its
echo [AUTOLAB] hash verified; autolab.py says so explicitly in its own output.

endlocal & exit /b %RC%
