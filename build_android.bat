@echo off
setlocal
rem Build a stripped Android APK from cmd or PowerShell and install it on the
rem connected device. Finds Git Bash and Python, then runs the
rem os\android\scripts steps (see docs\android\BUILDING.md).
rem
rem usage: build_android.bat [--setup] [--no-install]
rem   --setup       fetch the pinned dependencies first (tools\setup_repo.py)
rem   --no-install  only build the APK
rem STRIP=0 keeps the symbols (ndk-stack, symbolize.sh, simpleperf).
rem BUILD_TYPE=Release in the environment reaches build_native.sh.
if not defined STRIP set "STRIP=1"
set "do_setup="
set "do_install=1"
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--setup" (
    set "do_setup=1"
) else if /i "%~1"=="--no-install" (
    set "do_install="
) else (
    echo LibertyRecomp Android build: unknown option %~1 1>&2
    exit /b 2
)
shift
goto parse
:parsed

rem "bash" on PATH is usually the WSL launcher, so take the one next to git.
set "liberty_bash="
for /f "delims=" %%G in ('git --exec-path 2^>nul') do (
    for %%B in ("%%G\..\..\..\bin\bash.exe") do if exist "%%~fB" set "liberty_bash=%%~fB"
)
if not defined liberty_bash if exist "%ProgramFiles%\Git\bin\bash.exe" set "liberty_bash=%ProgramFiles%\Git\bin\bash.exe"
if not defined liberty_bash (
    echo LibertyRecomp Android build: Git Bash was not found. Install Git for Windows. 1>&2
    exit /b 1
)

rem The scripts call "python". The py launcher is absent from uv, scoop and
rem Store installs, and uv puts no python on PATH at all.
set "liberty_python="
for %%P in ("py -3" python3 python) do (
    if not defined liberty_python (
        for /f "delims=" %%E in ('%%~P -c "import sys; sys.version_info >= (3, 10) and print(sys.executable)" 2^>nul') do set "liberty_python=%%E"
    )
)
if not defined liberty_python (
    for /f "delims=" %%E in ('uv python find 2^>nul') do set "liberty_python=%%E"
)
if not defined liberty_python (
    echo LibertyRecomp Android build: Python 3.10 or newer is required. Tried: py -3, python3, python, uv. 1>&2
    exit /b 1
)
for %%D in ("%liberty_python%") do set "PATH=%%~dpD;%PATH%"

cd /d "%~dp0"

rem Gradle 8.11 does not run on the JDK 25 that newer Android Studio versions
rem bundle. Unless that jbr is a 17 or 21, use a JDK 21 in out\host-tools.
set "liberty_jdk=%~dp0out\host-tools\jdk"
if not defined JAVA_HOME (
    findstr /b /r "JAVA_VERSION=.17\. JAVA_VERSION=.21\." "%ProgramFiles%\Android\Android Studio\jbr\release" >nul 2>&1 || set "JAVA_HOME=%liberty_jdk%"
)
if /i "%JAVA_HOME%"=="%liberty_jdk%" if not exist "%liberty_jdk%\bin\java.exe" (
    echo == downloading JDK 21 into %liberty_jdk%
    if not exist "%liberty_jdk%" mkdir "%liberty_jdk%"
    curl -fsSL -o "%liberty_jdk%.zip" "https://api.adoptium.net/v3/binary/latest/21/ga/windows/x64/jdk/hotspot/normal/eclipse" || exit /b 1
    tar -xf "%liberty_jdk%.zip" --strip-components=1 -C "%liberty_jdk%" || exit /b 1
    del "%liberty_jdk%.zip"
)

echo == bash:   %liberty_bash%
echo == python: %liberty_python%
if defined JAVA_HOME echo == java:   %JAVA_HOME%

if defined do_setup (
    python tools\setup_repo.py || exit /b 1
)
rem The driver proxy only has to be built once.
if not exist "os\android\app\src\main\jniLibs\arm64-v8a\libvulkan.so" (
    "%liberty_bash%" os/android/scripts/build_driver_proxy.sh || exit /b 1
)
"%liberty_bash%" os/android/scripts/build_native.sh || exit /b 1
rem Gradle's incremental packaging leaves dead space in an existing APK.
if exist "os\android\app\build\outputs\apk\debug\app-debug.apk" del "os\android\app\build\outputs\apk\debug\app-debug.apk"
"%liberty_bash%" os/android/scripts/build_apk.sh || exit /b 1
if defined do_install (
    "%liberty_bash%" os/android/scripts/install.sh || exit /b 1
)
echo == APK: %~dp0os\android\app\build\outputs\apk\debug\app-debug.apk
