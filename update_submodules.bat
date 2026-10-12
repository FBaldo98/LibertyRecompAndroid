@echo off
setlocal
rem The py launcher is absent from uv, scoop and Store installs. Running each
rem candidate also skips the Store "python.exe" alias that only prints a hint.
set "liberty_python="
for %%P in ("py -3" python3 python) do (
    if not defined liberty_python (
        %%~P -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1 && set "liberty_python=%%~P"
    )
)
if not defined liberty_python (
    echo LibertyRecomp setup: Python 3.10 or newer is required. Tried: py -3, python3, python. 1>&2
    exit /b 1
)
%liberty_python% "%~dp0tools\setup_repo.py" %*
exit /b %errorlevel%
