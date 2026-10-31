# Rome 2 CPU Performance Fix

Source code for the Total War: ROME 2 CPU performance mod: a `dinput8.dll` that the game loads at
startup and that patches the engine in memory. No game files are changed.

Download the ready-made DLL:
- Steam Workshop: https://steamcommunity.com/sharedfiles/filedetails/?id=3826423359 (after subscribing, run `install.bat` in the mod's folder)
- or build it yourself (below), or take it from the latest run under **Actions**

Created with the help of AI (Claude by Anthropic). Tested in game by JGdaCaesar.

The mod does not appear in the game launcher's mod list: it is a DLL in the game folder, not a `.pack` mod. It is
active whenever `dinput8.dll` is in the game folder; the STATUS lines in `rome2fix.log` show which fixes are on.

Version 1.1 adds faster loading (a Divide et Impera save: 38 s with 1.0, 20 s with 1.1 on the test PC; see the
player README for all measurements). Version 1.2 also supports the "pantheon" beta and makes `rome2fix.log`
easier to read. Version 1.3 keeps `rome2fix.log` small, answers more missing-file checks while loading, and carries
the game side of the separate DLSS add-on "Upscaling and Framegen for Rome II" (NVIDIA DLSS Super Resolution and
Frame Generation): it only starts when the add-on's `FrameBridge` folder is in the game folder, otherwise none of it
runs. The add-on's 64-bit helper is its own repository: https://github.com/JSa25/TotalWarUpscalingFramegen
Version 1.4 fixes naval battles with the add-on below native resolution: the ships' selection markers and wake foam,
and the water's reflections. Version 1.5 brings the add-on's DLSS closer to a native integration (the pixel shaders'
camera follows the sub-pixel jitter, NVIDIA's mip bias below native size, AMD FidelityFX RCAS sharpening of the DLSS
output, the DLSS preset choice), fixes the tone mapper's exposure, the land water's reflections, the terrain's
virtual-texture page requests and shadow maps below native size, supports the GOG version, and adds `rome2fix.ini`
for switching parts off. Version 1.6 links the add-on into ReShade (`rslink.cpp`): ReShade wraps the game's swapchain,
so the bridge now hands frames over from under ReShade's wrapper, runs ReShade's effects at its HUD-less point (after
DLSS, before the UI) and gives ReShade the scene depth with the camera's direction and near / far distances.

## What each file does

| File | Fix |
|---|---|
| `proxy.c` | Entry point (`DllMain`, or a DLL load notification when something loaded `dinput8.dll` before the game DLL), dinput8 forwarding, `rome2fix.log`, off switches |
| `tilescan.c` | The campaign/battle tile-feedback scan: every frame the main thread counts a screen-sized GPU readback of texture-tile requests pixel by pixel. Runs of identical entries are now counted in one step, with the same counters and the same request list in the same order. Its entry hook is also the per-frame callback for the x87 safety check |
| `x87fix.c` | About 1,200 x87 float-to-integer conversions that switch the FPU rounding mode (`fstcw`/`fldcw`/`fistp`) replaced by `fisttp` / SSE4.1 `roundss` (from the Attila mod, unchanged: it finds the blocks by pattern) |
| `sortfix.c` | Two render-list sorts (by +0x6C/+0x68 and +0x44/+0x40): key extraction plus radix sort, same order as the game's sorts (from the Attila mod, with Rome 2's addresses and keys) |
| `attrcache.c` | Loading: a cache of missing folders behind the game's `GetFileAttributesW` import. Before using a packed file the game checks every search root (Data, each Workshop mod, language folders) for a loose copy, millions of failed checks per load with big mod lists. A failed check records the first missing name below the deepest existing folder; later checks under it get Windows' own answer (`ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND`) without asking. That folder and every folder above it are watched for created/renamed/deleted names and polled on every call; any change clears the cache. Unusual path forms (`/`, `.`, `..`, device names, ...) go straight to Windows |
| `fileio.c` | Loading: `.pack` files opened without `FILE_FLAG_NO_BUFFERING` (so Windows caches them), and small reads of read-only files in the game's user folder (the unpacked save) served from a 64 KB buffer with the exact file position kept; writes to the same file drop the buffer |
| `strfix.c` | Loading: the game's string equality, compare and assign helpers and a pair-table search, rewritten with 16-byte compares and `memcpy` (same results, byte-for-byte checked sites) |
| `diag.c` | Diagnostics in `rome2fix.log`: mod version, game build, system, settings, other hook DLLs, and crash records (a vectored exception handler that only logs and never handles the exception) |
| `update.c` | Self-update from the Steam Workshop copy (item 3826423359): a newer version there replaces the DLL in the game folder for the next start; never downgrades, no network. Also updates the DLSS add-on: from the one Workshop item its `install.bat` recorded (`FrameBridge\source.txt`), only when that item's `FrameBridge\version.txt` is higher; `settings.txt` and logs are kept |
| `logtrim.c`, `logtrim.h` | Past 1 MB, `rome2fix.log` drops its oldest whole game sessions (about 256 KB stay), so players can still post it |
| `bridgeprobe.c` | DLSS add-on, game side (only with `<game>\FrameBridge\helper64.exe`): finds the frame without the HUD, the camera and the scene depth in the game's Direct3D 11 calls, moves the game's swapchain to a hidden window, jitters the viewport for DLSS, reads `FrameBridge\settings.txt` |
| `fgbridge.c` | Hands each frame (picture, depth, camera motion) to the 64-bit helper through shared textures and fences, and paces the game to it; stops the helper and falls back to the game's own frames if it stops answering |
| `fgb_proto.h` | The shared-memory layout and fences between `fgbridge.c` and the helper (the same file in the helper's repository) |
| `fgpanel.c` | The F11 settings panel drawn over the frame |
| `r2motion.c`, `r2motionvs.h` | Per-object motion vectors for moving soldiers and horses: the animated models drawn again on the GPU with the game's own skinning, matched to the previous frame by position |
| `rslink.cpp`, `reshade/` | The bridge's ReShade link (C++, no C++ runtime linked): the mod registers itself as a ReShade add-on and runs ReShade's effects at the HUD-less point with the scene depth; `reshade/` holds ReShade's add-on API headers (v6.8.0, unchanged) |
| `builds.h` | The game builds the fixes know (normal Steam version 2026-09-17, "pantheon" beta 2026-10-01); each fix keeps its addresses and byte hashes per build |
| `version.h` | Mod version |
| `dinput8.def` | The exports a `dinput8.dll` must have |

Every patch first compares the game's bytes at its address with what it expects (with relocated
addresses taken into account), and skips itself (and says so in `rome2fix.log`) if they differ.
So on a game version it doesn't know it does nothing. Each fix has a table of {address, hash} per known
build (`builds.h`) and uses the entry whose bytes match.

The developer build adds measuring tools (timing probes, A/B switches); they are not part of this
source or of the released DLL.

## Building

You need [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the release used: 20261006, UCRT,
x86_64 host), with its `bin` folder on `PATH`. Then run:

```
build.bat
```

The DLL lands in `build\dinput8.dll`. The build is reproducible: with the same llvm-mingw version
you should get exactly the same file as the released one. Compare the SHA-256
(`certutil -hashfile build\dinput8.dll SHA256`) with the one given for each release. Always build it
under the name `dinput8.dll`: the name is stored inside the file.

### Automated builds

Every commit is also built on GitHub's own machines by
[.github/workflows/build.yml](.github/workflows/build.yml), with the compiler downloaded from its
official release (checksum pinned). The run then compares the SHA-256 of that DLL with
[dinput8.dll.sha256](dinput8.dll.sha256), the hash of the DLL released on the Steam Workshop, and
fails if they differ. Open the latest run under **Actions**: its summary shows both hashes, and the
DLL itself can be downloaded there. Builds also carry a signed build attestation
(`gh attestation verify dinput8.dll -R JSa25/Rome2CPUPerfFix`).

The Nexus Mods file is uploaded by [.github/workflows/nexus.yml](.github/workflows/nexus.yml) (started by hand):
it builds the DLL the same way, stops unless the hash matches the released one, zips it with
`Rome2CPUPerfFix_README.txt` and adds it to the Nexus file as a new version.

## Tests

The tests run the game's own code next to the replacements and check the results are identical:
the tile scan on 3,000 random buffers (11.6 million entries), every x87 block on 2,740 edge-case and
random inputs (3.3 million runs), both sorts on 110 lists of up to 30,000 items, the string helpers on 400,000
random cases. `fileio_test` compares 300,000 random reads and seeks with plain Windows calls, and `attrcache_test`
compares 450,000 random file checks with plain Windows (attributes and error codes) while files and folders are
created, renamed and deleted between checks; with the folder watches switched off the same test finds thousands
of differences, so it would catch a stale cache. `diag_test` checks
the crash records on deliberate faults, `selfupd_test` the update rules (the DLL and the DLSS add-on) on throwaway
files, `bridgelive_test` the DLSS add-on's game side against a stand-in game on your graphics card (`addon`: its
settings), `r2motion_test` the per-object motion vectors against values worked out on the CPU, and
`lateload_test` loads the built DLL before the game DLL (as an overlay can) and checks every fix
still installs from Windows' load notification. They read your copy of the game's
`empire.retail.dll` (it is not included here):

```
build.bat test "C:\...\steamapps\common\Total War Rome II\empire.retail.dll"
```

## License

See [LICENSE](LICENSE).

## Third-party notices

The DLSS sharpening pass in `fgbridge.c` follows AMD FidelityFX Robust Contrast Adaptive Sharpening (RCAS), used under
the MIT licence:

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

The headers in `reshade/` are ReShade's add-on API (https://github.com/crosire/reshade, v6.8.0), copyright (C) 2021
Patrick Mours, used unchanged under their licence (SPDX-License-Identifier: BSD-3-Clause OR MIT; the notice is in
each file).
