Total War: ROME 2 - CPU Performance Fix (dinput8.dll)
=====================================================

A drop-in DLL that makes Rome 2's main thread do less work per frame, and since
version 1.1 also makes loading faster. It changes no game files and no saves, so
you can remove it at any time.

Created with the help of AI.

Source code: https://github.com/JSa25/Rome2CPUPerfFix

Game versions: the normal Steam version of Rome 2 (Emperor Edition, September
2026), since 1.2 the "pantheon" beta (October 2026), and since 1.5 the GOG
version (copy dinput8.dll into the game folder by hand there; the automatic
updates come from the Steam Workshop, so GOG has none). Every patch first checks
the exact game bytes it replaces. If they don't match (another game version or
another engine mod), that patch turns itself off and the game runs unmodified.
The first lines of rome2fix.log say which game version you have and whether it is
supported.

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
  the cache, so the second start in a session loads from memory. (The
  pantheon beta no longer opens them that way, so there this part has nothing
  to do.)
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
  Loading without mods, first load after starting the game: 18 -> 9 s.

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
2. Start the game. A rome2fix.log file appears in the same folder. Each game
   start begins with a "====" line; a few lines below it, STATUS says which
   fixes are on (and why one is off, if any).

Uninstall: delete dinput8.dll (and rome2fix.log).

Steam Workshop: after subscribing, run install.bat in
...\steamapps\workshop\content\214950\3826423359\ once; later updates install
themselves on the next game start. Run uninstall.bat before unsubscribing.

Remember: the mod won't appear in the launcher's mod list. That's normal.


Turning single fixes off
------------------------
Since 1.5 the mod writes rome2fix.ini into the game folder at the first start.
Set a line to 0 to switch that part off (useful if you suspect one of them),
then restart the game; 1 switches it back on. The mod never changes the file
after writing it; delete it to get the defaults back.

  [fixes]
  tile_feedback_scan   tile streaming scan
  x87_rounding         x87 rounding
  render_list_sorts    render-list sorts (both)
  file_reading         save and .pack file reading (loading)
  string_functions     string comparisons (loading)
  missing_file_cache   loose-file check cache (loading)
  [mod]
  self_update          copy a newer version from the Steam Workshop at startup
  crash_logging        record crashes in rome2fix.log

The older way still works too: an empty tilescan_off.txt, x87fix_off.txt,
sortfix_off.txt, attrcache_off.txt, fileio_off.txt or strfix_off.txt in the
game folder switches that fix off. The STATUS lines in rome2fix.log show which
fixes are on.


Reporting a problem
-------------------
If the mod doesn't seem to work, or the game crashes, please send rome2fix.log
from the game folder (it's a small text file). Send the block from the last "====" line down. It shows:
- whether the game loaded the mod at all, and from which folder
- your game version and whether it is supported, and a STATUS summary of
  which fixes are on
- your game build, Windows version, CPU, RAM and your graphics/thread settings
- other DLLs in the game folder that hook the game (ReShade, DXVK, other mods)
- for a crash: where it happened (EXCEPTION lines, with a plain line saying
  whether it was in the game's own code or in this mod), so it can be traced
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


DLSS add-on (separate download)
-------------------------------
Since 1.3 this DLL can run the separate add-on "Upscaling and Framegen for
Rome II": NVIDIA DLSS Super Resolution and DLSS Frame Generation on NVIDIA RTX
graphics cards. It does nothing unless that add-on's FrameBridge folder is in
the game folder (its install.bat puts it there). The add-on's own README has
the details; the engine fix keeps it up to date from the add-on's Workshop item.
1.5 brings a sharper, more native-looking DLSS picture and the add-on's new
F11 settings (DLSS preset, sharpness) together with add-on version 2.
1.6 makes ReShade work with the add-on: ReShade's effects go on the DLSS
picture before the game's UI is drawn (banners and menus stay clean), and
ReShade gets the game's real depth, so depth effects (DOF, AO, fog) line up
without setting anything. Nothing changes for ReShade without the add-on.


Third-party notices
-------------------
The DLSS add-on's sharpening (in dinput8.dll) follows AMD FidelityFX Robust
Contrast Adaptive Sharpening (RCAS), used under the MIT licence:

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

The rest of this mod's code is by JGdaCaesar, all rights reserved; its source
is published for review at github.com/JSa25/Rome2CPUPerfFix.

Changelog
---------
Version 1.7: fixes 1.6's ReShade depth range getting stuck: ReShade saves the
           value the mod sets in your preset, and on the next start the mod
           took it for your own setting and stopped changing it (the campaign
           map kept the battle's range or the other way round). The mod now
           remembers its own value (FrameBridge\reshade_link.txt); a value you
           set yourself in ReShade is still left alone.
           SHA-256 of dinput8.dll:
           3b246bfcc7e383d07ff3cf6c0fe8e37f7d57bd18c9ff79a9cf06acaf5158e67a
Version 1.6: ReShade works with the DLSS add-on: its effects now go on after
           DLSS and before the game's UI, ReShade gets the game's real depth
           with the camera's near / far distances, and the ReShade menu shows
           and takes keys again (with the add-on it was drawn where nobody saw
           it). With frame generation and DLSS upscaling the effects are in
           the generated frames too (needs add-on version 4). No change
           without the add-on.
           SHA-256 of dinput8.dll:
           5a9f8de1ae0b6311574bcddb275d999da0687fe555a12b2f87b2385bf7d1de34
Version 1.5: with the DLSS add-on: sharper image (the game's lighting now
           follows DLSS's sub-pixel jitter, as games with DLSS built in do;
           new Sharpness setting, default Low), DLSS preset choice (J, K, L,
           M), finer texture detail below native resolution, and fixes for
           too bright a picture and lighter shadows below native resolution,
           dark lakes, coasts and rivers, and blurry terrain at the edges of
           the view. Supports the GOG version. rome2fix.ini to switch parts
           off. Fixes from a code review: a very long game folder path, safer
           undo of the x87 patches, and the game no longer freezes for 5 s
           when the DLSS helper stops responding. The log and the F11 panel
           now say when the DLSS add-on is too old for a setting.
           SHA-256 of dinput8.dll:
           bdeb9740fce552afd217b9b55e460fa2ea72b9d6c9b953620391200abb85fca3
Version 1.4: naval battles with the DLSS add-on below native resolution
           (Quality ... Ultra Performance): ship selection markers and wake
           foam sit on the ships again, and the water's reflections (sky,
           clouds, ships) are drawn in the right place. No change without the
           add-on.
           SHA-256 of dinput8.dll:
           15e87e437f5e091c87a808b26db85bfd2ac62545c50ff7d1c0872ecf3e97d5df
Version 1.3: runs the separate DLSS add-on "Upscaling and Framegen for Rome II"
           (does nothing without it) and copies the add-on's Workshop updates
           in at game start. rome2fix.log keeps only the newest ~256 KB once
           it passes 1 MB. The loose-file check cache also answers when the
           game opens a file that is known to be missing.
           SHA-256 of dinput8.dll:
           a009e01fecddf2b648f81dde0f7236c707231ae1d03c3595ba072a4f5ffec28b
Version 1.2: works on the "pantheon" beta too (all fixes). Easier to read
           rome2fix.log: a line per game start, the game version in plain
           words, a STATUS summary, plainer crash lines, less repetition.
           SHA-256 of dinput8.dll:
           0492d17646cff3428408a9920fe1425db10bc718dd842763234c7b23ec758626
Version 1.1: faster loading: loose-file check cache, buffered save reading,
           .pack files through the Windows file cache, faster string
           comparisons. DEI save load 38 -> 20 s on the test PC.
           SHA-256 of dinput8.dll:
           d3d9a3bd1216798088e2f89b0b44d8381483a6eaeed262c482f2911159a517eb
Version 1.0: first release: tile streaming scan, x87 rounding, render-list sorts.
           SHA-256 of dinput8.dll:
           166d36eb43de94675a869c6b43996b00c7d5fd8143edc9b5289e749729ad7152
