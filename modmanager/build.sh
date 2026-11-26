#!/bin/sh
# Total War Mod Manager for Rome 2: a 32-bit exe (it uses the game's own steam_api.dll), built with llvm-mingw.
#   ./build.sh  ->  build/TotalWarModManager.exe
# It includes the engine fix's ../update.c (Workshop updates), which reads ../version.h.
# VERSION_DIR=<folder with a version.h> builds against another version.h (see README.md).
set -e
cd "$(dirname "$0")"
CC=${CC:-i686-w64-mingw32-clang}
WINDRES=${WINDRES:-i686-w64-mingw32-windres}
VERSION_DIR=${VERSION_DIR:-..}
mkdir -p build
$WINDRES launcher.rc -O coff -o build/launcher_res.o
# a fixed link timestamp keeps the build reproducible
$CC -O2 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers \
    -I. -I"$VERSION_DIR" -I.. launcher.c build/launcher_res.o -o build/TotalWarModManager.exe \
    -mwindows -luser32 -lgdi32 -lcomctl32 -Wl,-Xlink=-timestamp:1795672727
ls -la build/TotalWarModManager.exe
