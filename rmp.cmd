@echo off
rem rmp: build, run, test and ship this project. `rmp help` lists the commands.
rem
rem cmd.exe's launcher: it only finds a Python 3.11+ and hands every argument
rem to tools\rmp.py, the whole implementation.
setlocal
set "RMP_PROBE=import sys; sys.exit(sys.version_info < (3, 11))"
for %%P in ("py -3" "python3" "python") do (
    %%~P -c "%RMP_PROBE%" >nul 2>&1
    if not errorlevel 1 (
        %%~P "%~dp0tools\rmp.py" %*
        goto done
    )
)
>&2 echo rmp: needs Python 3.11 or newer on PATH (py, python3 or python).
>&2 echo      https://www.python.org/downloads/
endlocal & exit /b 127
:done
endlocal & exit /b %ERRORLEVEL%
