Total War: ROME 2 - CPU Performance Fix (dinput8.dll)
=====================================================

A drop-in DLL that makes Rome 2's main thread do less work per frame, and since
version 1.1 also makes loading faster. It changes no game files and no saves, so
you can remove it at any time.

Created with the help of AI.

Source code: https://github.com/JSa25/Rome2CPUPerfFix

Game version: the current Steam build of Rome 2 (Emperor Edition, empire.retail.dll
of September 2026). Every patch first checks the exact game bytes it replaces. If
they don't match (a different game version or another engine mod), that patch
turns itself off and the game runs unmodified.

NOTE: this mod does not show up in the game launcher's mod list. It is a DLL in
the game folder, not a .pack mod, so the launcher doesn't know about it. It is
active whenever dinput8.dll is in the game folder. To check, look at rome2fix.log
in the game folder after starting the game.


What it fixes
-------------
Frame rate:
- Tile streaming scan (campaign map and battles): every frame the main thread
  reads back a screen-sized image from the graphics card that says which
  texture tiles each pixel needs, and counted it pixel by pixel. Neighbouring
  pixels almost always ask for the same tile, so runs of identical pixels are
  now counted in one step. Same result, about 12-17 times less work.
- x87 rounding: about 1,200 old-style float-to-integer conversions across the
  engine switched the CPU's rounding mode every time, which is slow on modern
  CPUs. They are replaced with modern instructions that give identical results
  (the same fix as in the Attila CPU Performance Fix). On the campaign map these
  took about 10% of the main thread.
- Render-list sorts: two sorts of the draw lists are about 11 times faster,
  with the same order. This is about 2% of the main thread in big battles.

Loading (new in 1.1):
- Loose-file checks: before using a file from the .pack files, the game checks
  every mod folder for a loose copy that would override it. With a big mod list
  that is millions of checks per load, and almost all of them ask about folders
  that don't exist (a DEI save load made 3.4 million of them, 11.6 seconds in
  total). The mod now remembers which folders are missing and answers those
  checks itself. It watches the folders, so files or folders you add while the
  game runs are still found.
- Save files: the game unpacks a save into a temp file and reads it back a few
  bytes at a time (hundreds of thousands of reads). Those small reads now come
  from a buffer.
- .pack files: the game opened its .pack files in a way that skips the Windows
  file cache, so every game start read them from disk again. They now go through
  the cache, so the second start in a session loads from memory.
- String comparisons used while loading game data are about 5 times faster.

Every code patch produces the same results as the original code. They were
tested against the game's own code (or against Windows itself, for the loading
fixes) on millions of inputs.

Rome 2 does NOT have Attila's big multithreading problem (the shared heap lock).
That was checked: its threads never wait on it. A faster heap was also tested
for loading in 1.1 and gave no measurable gain, so it is not included.


Measured results (Ryzen 9 9950X3D, RTX 5090, 4K, V-Sync off)
-------------------------------------------------------------
Frame rate, vanilla vs. with the mod (version 1.0 fixes), recorded with
CapFrameX. Same save and camera view on the campaign map, same big battle.

                          Vanilla     With mod    Change
  Big battle (70-73 s)
    Average FPS            118.4       140.4       +19%
    1% low FPS              71.8        92.5       +29%
    0.1% low FPS            57.9        77.0       +33%
  Campaign map (21 s)
    Average FPS            100.5       120.5       +20%
    1% low FPS              63.5        63.8       same

Loading a Divide et Impera save (13 Workshop mods), stopwatch, same save:
    version 1.0 (no loading fixes):   38 s
    version 1.1:                      20 s   (-47%)
  Loading with a normal mod list, first load after starting the game: 18 -> 11 s.

The slowest frames in battles improve even more than the average, so big
battles feel smoother, not just faster. The campaign run was short, so its 0.1%
lows (2-3 frames) are left out. The loading gain depends on how many mods you
use: the more mod folders, the more checks the game makes.

Single fixes, switched on and off in the same scene (same PC, 4K):
  Tile scan fix:  campaign 100.5 -> 117.5 fps (+17%), battle 141.9 -> 156.8 fps (+10.5%)
  x87 fix:        campaign 117.4 -> 132.7 fps (+13%)
  Sort fix:       no measurable change at 4K on this PC (the graphics card was the
                  limit there); it removes about 2% of the main thread's work in
                  big battles.
  Loose-file check cache: DEI save load 28 -> 20 s

Your numbers will differ. The frame rate gains come from CPU time saved per
frame, so they are largest where the CPU limits your frame rate: most CPUs at
1080p or 1440p, or slower CPUs at any resolution. Please share your numbers.


Install
-------
1. Copy dinput8.dll into the game folder, next to Rome2.exe, for example:
   ...\steamapps\common\Total War Rome II\
2. Start the game. A rome2fix.log file appears in the same folder and lists
   which fixes are active, plus your game build, system and settings.

Uninstall: delete dinput8.dll (and rome2fix.log).

Steam Workshop: after subscribing, run install.bat in
...\steamapps\workshop\content\214950\3826423359\ once; later updates install
themselves on the next game start. Run uninstall.bat before unsubscribing.

Remember: the mod won't appear in the launcher's mod list. That's normal.


Turning single fixes off
------------------------
Create an empty file with one of these names in the game folder to disable that
fix (useful if you suspect one of them), then restart the game:

  tilescan_off.txt     tile streaming scan
  x87fix_off.txt       x87 rounding
  sortfix_off.txt      render-list sorts (both)
  attrcache_off.txt    loose-file check cache (loading)
  fileio_off.txt       save and .pack file reading (loading)
  strfix_off.txt       string comparisons (loading)

rome2fix.log shows what was installed or skipped.


Reporting a problem
-------------------
If the mod doesn't seem to work, or the game crashes, please send rome2fix.log
from the game folder (it's a small text file). It shows:
- whether the game loaded the mod at all, and from which folder
- your game build, Windows version, CPU, RAM and your graphics/thread settings
- other DLLs in the game folder that hook the game (ReShade, DXVK, other mods)
- for a crash: where it happened (EXCEPTION lines), so it can be traced
The log only records; it never changes how the game handles a crash. It shows
your Windows user name in folder paths, so blank that out before posting it
publicly if you like.


Compatibility and notes
-----------------------
- Needs a CPU with SSE4.1 (any CPU from roughly 2008 onward). Without it, the
  x87 fix only replaces the conversions that don't need it.
- NOT compatible with other dinput8.dll mods. Only one dinput8.dll can be loaded.
- Works with pack mods (tested with Divide et Impera and a large mod list) and
  UI mods.
- Does not show up in the launcher's mod list (see the note at the top).
- The x87 fix watches the game's floating-point mode at runtime. If the game
  ever switches it, the affected patches restore the original code
  automatically.
- DXVK was tested and made Rome 2 slower (-2% campaign, -12% battle) with
  wrong colours, so it is not recommended together with this mod.
- Not fixed: end-turn AI time (it runs on one thread and has no single big
  cost), the graphics driver's own share of the main thread, and the time the
  driver takes to create textures while loading.


Changelog
---------
Version 1.1: faster loading: loose-file check cache, buffered save reading,
           .pack files through the Windows file cache, faster string
           comparisons. DEI save load 38 -> 20 s on the test PC.
           SHA-256 of dinput8.dll:
           d3d9a3bd1216798088e2f89b0b44d8381483a6eaeed262c482f2911159a517eb
Version 1.0: first release: tile streaming scan, x87 rounding, render-list sorts.
           SHA-256 of dinput8.dll:
           166d36eb43de94675a869c6b43996b00c7d5fd8143edc9b5289e749729ad7152
