@echo off
setlocal
rem Build the PGO recording APK and install it without launching, from cmd or
rem PowerShell. This is step 1 of the profile-guided optimization flow (see
rem os\android\scripts\pgo.sh and AGENT.md "Building"):
rem
rem   1. build_android_pgo_record.bat      (this script)
rem   2. start the game and play about five minutes through typical scenes;
rem      the recording build is slower, counters are written every 60 s
rem   3. build_android_pgo_optimized.bat   (pulls the counters, merges the
rem      profile into out\pgo\liberty.profdata and builds with it)
rem
rem The recording build lives in its own build folder
rem (out\build\android-arm64-pgogen) and never touches the normal or the
rem optimized build folders.
if not defined STRIP set "STRIP=1"

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

rem The recording build: profile counters are compiled in (PGO=generate), in
rem its own build folder. Forced even when the environment already defines
rem PGO or BUILD_DIR (build_android.bat auto-detects the finished profile).
set "liberty_root=%~dp0"
set "liberty_root=%liberty_root:\=/%"
set "PGO=generate"
set "BUILD_DIR=%liberty_root%out/build/android-arm64-pgogen"

echo == bash:   %liberty_bash%
echo == python: %liberty_python%
if defined JAVA_HOME echo == java:   %JAVA_HOME%
echo == pgo:    generate (recording build, %BUILD_DIR%)

rem The driver proxy only has to be built once.
if not exist "os\android\app\src\main\jniLibs\arm64-v8a\libvulkan.so" (
    "%liberty_bash%" os/android/scripts/build_driver_proxy.sh || exit /b 1
)
"%liberty_bash%" os/android/scripts/build_native.sh || exit /b 1
rem Gradle's incremental packaging leaves dead space in an existing APK.
if exist "os\android\app\build\outputs\apk\debug\app-debug.apk" del "os\android\app\build\outputs\apk\debug\app-debug.apk"
"%liberty_bash%" os/android/scripts/build_apk.sh || exit /b 1
rem Install without launching: the tester starts the game when ready.
"%liberty_bash%" os/android/scripts/install.sh debug --no-launch || exit /b 1
rem Drop stale counters from a previous recording before this one runs.
"%liberty_bash%" os/android/scripts/pgo.sh --clear || exit /b 1
echo.
echo == Recording build installed, not launched. Next:
echo ==   1. Start the game and play about five minutes through typical scenes.
echo ==      Counters are written to files/pgo every 60 seconds.
echo ==   2. Leave the game running and run build_android_pgo_optimized.bat.
