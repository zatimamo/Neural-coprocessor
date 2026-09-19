@echo off
rem ===========================================================================
rem  RUN-NR-WORKER.cmd
rem
rem  Runs everything about the out-of-process NR worker that needs the REAL
rem  machine: the worker's Reserved18 proof and the two-GPU transport benchmark.
rem  Neither can run in CI, because GitHub's runners have no NVIDIA adapter.
rem
rem      RUN-NR-WORKER.cmd [path\to\nvngx_dlssnr.dll]
rem
rem  The NR DLL defaults to nvngx_dlssnr.dll beside this script, exactly as the
rem  PROCESSCONTEXT launcher does. Pass the NeuralScreen runtime and the data
rem  path follows from its directory:
rem
rem      RUN-NR-WORKER.cmd "C:\...\neuralscreen-v1.15.0-full\native\nvngx_dlssnr.dll"
rem
rem  WHAT RUNS, IN THIS ORDER
rem      1. nr_selftest.exe          GPU-free: protocol, real pipe handshake,
rem                                  frame-slot pipeline, JSON. Says so itself.
rem      2. mgpu_nr_worker.exe --prove
rem                                  the lane, in a prefixed copy, in its own
rem                                  process. This is the Reserved18 proof.
rem      3. mgpu_nr_worker.exe --serve --protocol-only
rem         nr_ipc_probe.exe         the two REAL binaries doing a real
rem                                  cross-process handshake.
rem      4. nr_transport_bench.exe   the four transport legs, four resolutions,
rem                                  transport-benchmark.json.
rem
rem  WHY THE WORKER IS COPIED FIRST
rem      The DLSSNR snippet inspects its CALLER's module name and refuses a
rem      caller that does not contain "nvngx.dll" with 0xBAD00002 FAIL_PlatformError
rem      - a failure that has nothing to do with the neural work. Production
rem      deploys the worker under such a name; this script makes the same copy the
rem      PROCESSCONTEXT launcher makes, for the same reason.
rem
rem  Nothing is deployed. Cyberpunk is never launched. The transport benchmark
rem  loads neither NGX nor NVAPI, on purpose: if it fails, the transport is
rem  broken and the neural worker is not implicated.
rem
rem  NOTE ON STYLE. goto labels rather than parenthesised if-blocks, and no echo
rem  text containing a parenthesis inside a block: cmd.exe closes a block at the
rem  first unescaped ")", including one inside an echo.
rem ===========================================================================

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "WORKER=mgpu_nr_worker.exe"
set "RUNNAME=nvngx.dll_mgpu_nr_worker.exe"
set "NRDLL=%~1"
if "%NRDLL%"=="" set "NRDLL=nvngx_dlssnr.dll"

echo ================================================================================
echo NR-WORKER  -  the out-of-process neural lane, on the real machine
echo ================================================================================
echo   working directory : %CD%
echo   NR DLL            : %NRDLL%
echo   worker            : %WORKER%  run as %RUNNAME%
echo ================================================================================
echo.

if not exist "%WORKER%" goto :nr_noexe
if not exist "nr_selftest.exe" goto :nr_notools
if not exist "nr_transport_bench.exe" goto :nr_notools
if not exist "nr_ipc_probe.exe" goto :nr_notools
if not exist "%NRDLL%" goto :nr_nodll

set "FAILED=0"
set "TRC=0"

echo --------------------------------------------------------------------------------
echo  1. GPU-free self-test
echo --------------------------------------------------------------------------------
nr_selftest.exe --json nr-selftest.json
if not "!ERRORLEVEL!"=="0" set "FAILED=1"
echo   [nr_selftest exited with !ERRORLEVEL!]
echo.

echo --------------------------------------------------------------------------------
echo  2. the worker proof - the lane, in its own process  -  Reserved18
echo --------------------------------------------------------------------------------
copy /y "%WORKER%" "%RUNNAME%" >nul
if errorlevel 1 goto :nr_nocopy
"%RUNNAME%" --prove --nr-dll "%NRDLL%" --log-name nr-worker
set "TRC=!ERRORLEVEL!"
echo   [worker --prove exited with !TRC!]
if not "!TRC!"=="0" set "FAILED=1"
echo.

echo --------------------------------------------------------------------------------
echo  3. the control handshake between the two REAL binaries
echo      the worker serves with --protocol-only, so no neural session is involved
echo      the pipe name is fixed here, so nothing has to discover a pid
echo --------------------------------------------------------------------------------
start "" /b "%RUNNAME%" --serve --protocol-only --accept-seconds 30 --pipe \\.\pipe\MGPU_NR_HANDSHAKE --log-name nr-worker-ipc
timeout /t 2 /nobreak >nul
nr_ipc_probe.exe --pipe \\.\pipe\MGPU_NR_HANDSHAKE --frames 4 --expect-protocol-only --json nr-ipc-probe.json
if not "!ERRORLEVEL!"=="0" set "FAILED=1"
echo   [nr_ipc_probe exited with !ERRORLEVEL!]
timeout /t 2 /nobreak >nul
echo.

echo --------------------------------------------------------------------------------
echo  4. the two-GPU transport benchmark - NO NR, NO NGX, NO NVAPI
echo --------------------------------------------------------------------------------
nr_transport_bench.exe --frames 30 --slots 3 --json transport-benchmark.json
if not "!ERRORLEVEL!"=="0" set "FAILED=1"
echo   [nr_transport_bench exited with !ERRORLEVEL!]
echo.

del /q "%RUNNAME%" >nul 2>&1

echo ================================================================================
echo NR-WORKER  -  finished
echo ================================================================================
echo   logs       : context-nr-worker.log  context-nr-worker-ipc.log
echo   reports    : nr-selftest.json  nr-ipc-probe.json  transport-benchmark.json
if not "%FAILED%"=="0" echo   NOTE: at least one stage failed - read the log it names.
echo.
echo   Cyberpunk was NOT launched and nothing was deployed.
echo.
endlocal & exit /b %FAILED%

:nr_noexe
echo ERROR: "%WORKER%" is not beside this script.
echo        Build it first: see README.txt for the two cmake commands.
endlocal & exit /b 1

:nr_notools
echo ERROR: nr_selftest.exe, nr_ipc_probe.exe and nr_transport_bench.exe must all be
echo        beside this script. They come from the same build.
endlocal & exit /b 1

:nr_nodll
echo ERROR: the NR DLL "%NRDLL%" was not found.
echo        Pass the NeuralScreen runtime:
echo            RUN-NR-WORKER.cmd "C:\...\neuralscreen-v1.15.0-full\native\nvngx_dlssnr.dll"
endlocal & exit /b 1

:nr_nocopy
echo ERROR: could not create "%RUNNAME%".
endlocal & exit /b 1
