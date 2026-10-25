@echo off
rem Builds dinput8.dll (and with "build.bat test <path to empire.retail.dll>" also runs the tests).
rem Needs llvm-mingw (its bin folder on PATH). See README.md.
setlocal
cd /d "%~dp0"
if "%CC%"=="" set "CC=i686-w64-mingw32-clang"
if not exist build mkdir build

rem --no-insert-timestamp keeps the build reproducible: the same compiler gives the same file
"%CC%" -O2 -Wall -Wextra -shared -s -static -Wl,--no-insert-timestamp -o build\dinput8.dll proxy.c tilescan.c x87fix.c sortfix.c diag.c update.c fileio.c strfix.c attrcache.c dinput8.def || exit /b 1
echo built build\dinput8.dll

if /i not "%~1"=="test" exit /b 0
if "%~2"=="" (
    echo usage: build.bat test "path\to\empire.retail.dll"
    exit /b 1
)
"%CC%" -O2 -Wall -static -o build\tilescan_test.exe tests\tilescan_test.c tilescan.c || exit /b 1
for %%T in (x87fix sort diag selfupd lateload fileio strfix attrcache) do (
    "%CC%" -O2 -Wall -static -o build\%%T_test.exe tests\%%T_test.c || exit /b 1
)
for %%T in (tilescan x87fix sort strfix) do (
    echo == %%T
    build\%%T_test.exe "%~2" || (echo %%T test FAILED & exit /b 1)
)
for %%T in (diag selfupd fileio attrcache) do (
    echo == %%T
    build\%%T_test.exe || (echo %%T test FAILED & exit /b 1)
)
echo == lateload
build\lateload_test.exe "%~dp0build\dinput8.dll" "%~2" || (echo lateload test FAILED & exit /b 1)
echo all tests passed
