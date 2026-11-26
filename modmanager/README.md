# Total War Mod Manager (Rome 2)

Ships with the Rome 2 Engine Performance Fix (Steam Workshop item 3826423359, next to `dinput8.dll`). It takes the place
of CA's launcher at `launcher\launcher.exe` (CA's is kept as `launcher_ca.exe`), so Steam's Play button opens it: the mod
list of Total War: ROME II with presets, search, drag to reorder, export and import with Workshop links, a list code for
multiplayer, and CA's launcher one click away. Ported from Attila's Total War Mod Manager (its 7.6 source), without
Attila's 64-bit parts.

## Files
- `launcher.c`: paths, the mod list, presets, Steam (through the game's own `steam_api.dll`), Workshop updates, Play.
- `host_ui.inc`: the questions, the add-on switches, Play. `launcher_swap.inc`: install and uninstall (`--install`,
  `--uninstall`, used by the engine fix's `install.bat` / `uninstall.bat` and its DLL).
- `picker_*.inc`: the window (drawing, lists, packs, input, background work, test hooks).
- `launcher.rc`, `launcher.manifest`, `launcher.ico` (a gold T, drawn for this program).
- The engine fix side is `../launcher_hook.c`: it puts the Mod Manager back after Steam's "Verify files" and reads
  `[mod] launcher=` in `rome2fix.ini` (1 = Total War Mod Manager, 2 = CA's launcher). It finds the Mod Manager by the
  marker `AttilaLauncher-version:` in `launcher\launcher.exe` (the same marker as Attila's, kept on purpose).

## What Play does
It saves the ticked mods to `used_mods.txt` in the game folder (`add_working_directory "<folder>";` and `mod "<pack>";`
lines, the format CA's launcher writes), takes newer engine fix and upscaling add-on files from the Workshop
(`update_off.txt` in the game folder stops that), and starts `Rome2.exe used_mods.txt;` in the game folder, as CA's
launcher does. Its own log and settings are in `<game>\TotalWarModManager\`.

## Building
With [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20261006 (UCRT, x86_64 host) on `PATH`, in this folder:

```
./build.sh
```

The exe lands in `build/TotalWarModManager.exe`. The build is reproducible. It includes the engine fix's `../update.c`,
which reads `../version.h`: its `MOD_VERSION_NUM` only sets a marker string inside the exe (the update check reads the
installed DLL's own version), but it changes the exe's bytes. To build against another `version.h`:

```
VERSION_DIR=/path/to/folder_with_version.h ./build.sh
```

## License
Covered by the engine fix's [LICENSE](../LICENSE) at the top of its source (in this repository:
`release/source/Rome2CPUPerfFix/LICENSE`), except the portions listed in
[SOL_INVICTUS_MIT.txt](SOL_INVICTUS_MIT.txt), which come from Sol Invictus's 64-bit Attila and keep its MIT license.

## 1.01 (engine fix 2.4.1)
A crash writes one `CRASH` line into `TotalWarModManager\launcher.log` in the game folder (`crashlog.inc`, the same
file as the Attila Mod Manager's): the exception, the module and offset, what each thread was doing, the Windows
version, and this exe's return addresses on the stack; `build/TotalWarModManager_syms.exe` (the same build with debug
info) turns the offsets into functions and lines (`llvm-addr2line -f -i -e build/TotalWarModManager_syms.exe
0x<0x400000 + offset>`). After two starts in a row that ended by themselves in their first 10 seconds, the next start
asks whether to go back to CA's launcher. CA's launcher started from here gets Steam's game number (SteamAppId).
