@echo off
rem Builds dinput8.dll (and with "build.bat test <path to empire.retail.dll>" also runs the tests).
rem Needs llvm-mingw (its bin folder on PATH). See README.md.
setlocal
cd /d "%~dp0"
if "%CC%"=="" set "CC=i686-w64-mingw32-clang"
if "%CXX%"=="" set "CXX=i686-w64-mingw32-clang++"
if "%WINDRES%"=="" set "WINDRES=i686-w64-mingw32-windres"
if not exist build mkdir build

rem --no-insert-timestamp keeps the build reproducible: the same compiler gives the same file
rem bridgeprobe.c fgbridge.c fgpanel.c r2motion.c: the DLSS add-on's frame bridge, started only by <game>\FrameBridge
rem rslink.cpp: the bridge's ReShade link (C++, ReShade's add-on API headers in reshade\; no C++ runtime linked)
"%CXX%" -std=c++17 -O2 -Wall -Wextra -isystem reshade -fno-exceptions -fno-rtti -fno-threadsafe-statics -c -o build\rslink.o rslink.cpp || exit /b 1
rem dinput8.rc: the version resource (Properties > Details)
"%WINDRES%" -O coff -o build\dinput8_res.o dinput8.rc || exit /b 1
rem not stripped (-s): the stripped 2.0 DLL was flagged by Microsoft's cloud ML (Wacatac), unstripped passes (VirusTotal 2026-11-02)
"%CC%" -O2 -Wall -Wextra -shared -static -Wl,--no-insert-timestamp -o build\dinput8.dll proxy.c tilescan.c x87fix.c sortfix.c diag.c update.c fileio.c strfix.c attrcache.c logtrim.c memwatch.c scriptext.c plugins.c fpslog.c soundguard.c overlay.c menuver.c launcher_hook.c partcull.c parsort.c uicache.c bridgeprobe.c fgbridge.c fgpanel.c r2motion.c build\rslink.o build\dinput8_res.o dinput8.def -lgdi32 -lpsapi -ld3d11 || exit /b 1
echo built build\dinput8.dll

if /i not "%~1"=="test" exit /b 0
if "%~2"=="" (
    echo usage: build.bat test "path\to\empire.retail.dll"
    exit /b 1
)
"%CC%" -O2 -Wall -static -o build\tilescan_test.exe tests\tilescan_test.c tilescan.c fpslog.c || exit /b 1
"%CC%" -O2 -Wall -static -o build\logtrim_test.exe tests\logtrim_test.c logtrim.c || exit /b 1
for %%T in (x87fix sort diag selfupd lateload fileio strfix attrcache memwatch fpslog soundguard dupload menuver partcull parsort) do (
    "%CC%" -O2 -Wall -static -o build\%%T_test.exe tests\%%T_test.c || exit /b 1
)
for %%T in (tilescan x87fix sort strfix soundguard menuver partcull parsort) do (
    echo == %%T
    build\%%T_test.exe "%~2" || (echo %%T test FAILED & exit /b 1)
)
for %%T in (diag selfupd fileio attrcache logtrim memwatch fpslog) do (
    echo == %%T
    build\%%T_test.exe || (echo %%T test FAILED & exit /b 1)
)
echo == diag wildesp
build\diag_test.exe wildesp || (echo diag wildesp test FAILED & exit /b 1)
echo == dupload
build\dupload_test.exe "%~dp0build\dinput8.dll" || (echo dupload test FAILED & exit /b 1)
rem the frame bridge (needs a D3D11 GPU): the glue with fgbridge.c stubbed, the add-on's settings, the motion pass
"%CC%" -O2 -Wall -static -o build\bridgelive_test.exe tests\bridgelive_test.c r2motion.c build\rslink.o || exit /b 1
"%CC%" -O2 -Wall -static -o build\r2motion_test.exe tests\r2motion_test.c || exit /b 1
"%CC%" -O2 -Wall -static -o build\jitedge_test.exe tests\jitedge_test.c -ld3d11 -lgdi32 || exit /b 1
"%CC%" -O2 -Wall -static -o build\fgpanel_test.exe tests\fgpanel_test.c -ld3d11 -lgdi32 || exit /b 1
"%CC%" -O2 -Wall -static -o build\fgfill_test.exe tests\fgfill_test.c -ld3d11 -lgdi32 || exit /b 1
"%CC%" -O2 -Wall -static -o build\overlay_test.exe tests\overlay_test.c -ld3d11 -lgdi32 || exit /b 1
"%CC%" -O2 -Wall -static -o build\overlay_nest_test.exe tests\overlay_nest_test.c || exit /b 1
"%CC%" -O2 -Wall -static -municode -o build\launcher_hook_test.exe tests\launcher_hook_test.c || exit /b 1
"%CC%" -O2 -Wall -static -o build\uicache_test.exe tests\uicache_test.c -ld3d11 -lgdi32 || exit /b 1
echo == bridgelive
build\bridgelive_test.exe || (echo bridgelive test FAILED & exit /b 1)
build\bridgelive_test.exe addon || (echo bridgelive add-on test FAILED & exit /b 1)
build\bridgelive_test.exe startfail || (echo bridgelive start-failed test FAILED & exit /b 1)
build\bridgelive_test.exe startfail nod3d12 || (echo bridgelive no-12_0 test FAILED & exit /b 1)
echo == r2motion
build\r2motion_test.exe || (echo r2motion test FAILED & exit /b 1)
echo == jitedge
build\jitedge_test.exe || (echo jitedge test FAILED & exit /b 1)
echo == fgpanel
build\fgpanel_test.exe || (echo fgpanel test FAILED & exit /b 1)
echo == fgfill
build\fgfill_test.exe || (echo fgfill test FAILED & exit /b 1)
echo == overlay
build\overlay_test.exe || (echo overlay test FAILED & exit /b 1)
echo == overlay_nest
build\overlay_nest_test.exe || (echo overlay_nest test FAILED & exit /b 1)
echo == launcher_hook
build\launcher_hook_test.exe || (echo launcher_hook test FAILED & exit /b 1)
echo == uicache
build\uicache_test.exe || (echo uicache test FAILED & exit /b 1)
build\uicache_test.exe plain || (echo uicache no-add-on test FAILED & exit /b 1)
build\uicache_test.exe "%~2" || (echo uicache game check FAILED & exit /b 1)
echo == lateload
build\lateload_test.exe "%~dp0build\dinput8.dll" "%~2" || (echo lateload test FAILED & exit /b 1)
echo == plugins
"%CC%" -O2 -Wall -shared -static -o build\plugin_dummy.dll tests\plugin_dummy.c || exit /b 1
"%CC%" -O2 -Wall -static -o build\plugins_test.exe tests\plugins_test.c plugins.c || exit /b 1
pushd build
.\plugins_test.exe plugin_dummy.dll || (popd & echo plugins test FAILED & exit /b 1)
popd
echo all tests passed
