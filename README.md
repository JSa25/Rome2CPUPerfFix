# Rome II CPU Performance Fix

Source code for the Total War: ROME II CPU performance mod: a `dinput8.dll` that the game loads at
startup and that patches the engine in memory. No game files are changed.

Download the ready-made DLL: (links added on release)

Created with the help of AI (Claude by Anthropic). Tested in game by JGdaCaesar.

## What each file does

| File | Fix |
|---|---|
| `proxy.c` | Entry point (`DllMain`, or a DLL load notification when something loaded `dinput8.dll` before the game DLL), dinput8 forwarding, `rome2fix.log`, off switches |
| `tilescan.c` | The campaign/battle tile-feedback scan: every frame the main thread counts a screen-sized GPU readback of texture-tile requests pixel by pixel. Runs of identical entries are now counted in one step, with the same counters and the same request list in the same order. Its entry hook is also the per-frame callback for the x87 safety check |
| `x87fix.c` | About 1,200 x87 float-to-integer conversions that switch the FPU rounding mode (`fstcw`/`fldcw`/`fistp`) replaced by `fisttp` / SSE4.1 `roundss` (from the Attila mod, unchanged: it finds the blocks by pattern) |
| `sortfix.c` | Two render-list sorts (by +0x6C/+0x68 and +0x44/+0x40): key extraction plus radix sort, same order as the game's sorts (from the Attila mod, with Rome II's addresses and keys) |
| `diag.c` | Diagnostics in `rome2fix.log`: mod version, game build, system, settings, other hook DLLs, and crash records (a vectored exception handler that only logs and never handles the exception) |
| `update.c` | Self-update from the Steam Workshop copy (off until the Workshop item exists) |
| `version.h` | Mod version |
| `dinput8.def` | The exports a `dinput8.dll` must have |

Every patch first compares the game's bytes at its address with what it expects (with relocated
addresses taken into account), and skips itself (and says so in `rome2fix.log`) if they differ.
So on another game version it does nothing.

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

## Tests

The tests run the game's own code next to the replacements and check the results are identical:
the tile scan on 3,000 random buffers (11.6 million entries), every x87 block on 2,740 edge-case and
random inputs (3.3 million runs), both sorts on 110 lists of up to 30,000 items. `diag_test` checks
the crash records on deliberate faults, `selfupd_test` the update rules on throwaway files, and
`lateload_test` loads the built DLL before the game DLL (as an overlay can) and checks every fix
still installs from Windows' load notification. They read your copy of the game's
`empire.retail.dll` (it is not included here):

```
build.bat test "C:\...\steamapps\common\Total War Rome II\empire.retail.dll"
```

## License

See [LICENSE](LICENSE).
