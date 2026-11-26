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

Battles (new in 2.2, from the Attila Engine Performance Fix):
- Unit cards: the battle's unit card panel is no longer built and drawn every
  frame. It is drawn into a picture a few times a second and whenever you press
  a key or click, and that picture is shown in between, so the cards look the
  same while the main thread does less.
- Soldier draw lists: the soldiers' per-pass draw lists are sorted on four
  threads instead of one, with the same order.
- Culling: small parts of far-away soldiers (belts, quivers, gauntlets,
  bridles) are not drawn once they would be only a few pixels tall. Helmets,
  heads and anything with real bulk are always kept. It is on Light by default;
  F8 > Culling picks Off, Light, Normal or Strong (see "Turning single fixes
  off").
- Sound: in very big battles the game's sound memory can run full; a sound that
  then gets no buffer is skipped for a moment instead of crashing the game.

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
then restart the game; 1 switches it back on. Since 2.3 a newer version adds
its new lines to the file (your own values stay as they are); otherwise the
mod only changes the culling= line (when you pick it in F8). Delete the file
to get the defaults back.

  [fixes]
  tile_feedback_scan   tile streaming scan
  x87_rounding         x87 rounding
  render_list_sorts    render-list sorts (both)
  file_reading         save and .pack file reading (loading)
  string_functions     string comparisons (loading)
  missing_file_cache   loose-file check cache (loading)
  sound_crash_guard    sound crash guard (2.2)
  battle_ui_cache      unit cards shown from a cache (2.2)
  parallel_sort        soldier draw lists sorted on four threads (2.2)
  [mod]
  self_update          copy a newer version from the Steam Workshop at startup
  crash_logging        record crashes in rome2fix.log (and a crash_*.dmp file)
  plugins              load other mods' DLLs from the plugins folder (2.1)
  [tuning]
  culling              off, light (the default), normal or strong (2.2)

The older way still works too: an empty tilescan_off.txt, x87fix_off.txt,
sortfix_off.txt, attrcache_off.txt, fileio_off.txt, strfix_off.txt,
soundguard_off.txt, uicache_off.txt, parsort_off.txt or partcull_off.txt in the
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
Since 2.1 the log also has your graphics card, driver, display, free memory and
disk space and the mods you have on, a "FREEZE?" line when the game stops
drawing frames for 20 s, and memory warnings when the 32-bit game runs low.
The first crash of a session also writes a crash_<date>_<time>.dmp file into
the game folder (the newest 3 are kept): please attach it to a crash report.
The log only records; it never changes how the game handles a crash. It shows
your Windows user name in folder paths, so blank that out before posting it
publicly if you like.


For modders (new in 2.1)
------------------------
- Plugins: any DLL in a plugins folder in the game folder is loaded at game
  start, in name order (for example plugins\smoke.dll), so a DLL mod doesn't
  need its own dinput8.dll / xinput slot. rome2fix.log lists each one.
  plugins=0 in rome2fix.ini switches them off.
- Lua scripts: .lua files in a lua_ext folder in the game folder run in every
  script environment the game opens (front end, campaign), and Ctrl+` opens a
  Lua console. ext_print and errors go to lua_ext\lua_ext.log. Nothing runs
  without the lua_ext folder. Steam version of the game only.
- Frame times: create an empty fps_on.txt in the game folder before starting
  the game: fps_log.csv gets one line per second (fps, 1% low, worst frame)
  and a summary when you delete fps_on.txt.


The F8 panel (new in 2.2)
-------------------------
F8 opens a small panel in game, also without the DLSS add-on: Culling, and
What's new (everything in this version). The first start of a new version
shows a short What's new card at the top of the screen, and a card says when
Steam has a newer version of the mod (restart the game to use it). With the
add-on, F8 is its settings panel, with Culling and What's new buttons at the
top. The main menu shows this mod's version in the top right.


Compatibility and notes
-----------------------
- Needs a CPU with SSE4.1 (any CPU from roughly 2008 onward). Without it, the
  x87 fix only replaces the conversions that don't need it.
- NOT compatible with other dinput8.dll mods. Only one dinput8.dll can be loaded.
  Since 2.2 a second copy of this mod in the same game (for example one under
  another name in the plugins folder) stays out and says so in rome2fix.log.
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
Rome II": NVIDIA DLSS and (since 2.0) AMD FSR upscaling and frame generation,
on NVIDIA, AMD and Intel graphics cards. It does nothing unless that add-on's FrameBridge folder is in
the game folder (its install.bat puts it there). The add-on's own README has
the details; the engine fix keeps it up to date from the add-on's Workshop item.
1.5 brings a sharper, more native-looking DLSS picture and the add-on's new
F11 settings (DLSS preset, sharpness) together with add-on version 2.
1.6 makes ReShade work with the add-on: ReShade's effects go on the DLSS
picture before the game's UI is drawn (banners and menus stay clean), and
ReShade gets the game's real depth, so depth effects (DOF, AO, fog) line up
without setting anything. Nothing changes for ReShade without the add-on.
1.9 adds a V-Sync On / Off setting (F11, with add-on version 5) and fixes thin
white lines along the right edge with DLAA while the camera moves.
2.0 brings AMD FSR, motion data for swaying trees and plants, and the F11 FPS
counter and recommended setup, together with add-on version 6.
2.1 moves the add-on's settings panel to F8 (panel_key= in its settings.txt
picks another key) and shows it, with a note, when the add-on could not start.


Third-party notices
-------------------
The ReShade link (in dinput8.dll) is built with ReShade's add-on API headers,
Copyright (C) 2021 Patrick Mours, used under the MIT licence (the same text as
below, with his name in place of AMD's).

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
Version 2.3:
           - the unit cards show again in battles. 2.2's card cache put
           its picture where the card panel's layout position said, but
           the game draws its UI scaled again (1.69 times that position on
           the test PC, UI scale 1.7 at 4K), so the picture held nothing
           and the cards were not shown. The cache now takes the cards'
           place on screen from what the game actually draws. It caches
           the cards panel itself (cards_panel), as the Attila fix does.
           - rome2fix.ini: a newer version adds its new lines to the file,
           your own values kept (a file from before 2.2 had no
           battle_ui_cache line to switch the cache off with).
           SHA-256 of dinput8.dll:
           7344f8c73b8f9dbaf767f49002c07f3fc9d884c02432c0e17c1e2f426d795d54
Version 2.2:
           - battles: the unit card panel is drawn from a cache, and the
           soldiers' draw lists are sorted on four threads (both ported
           from the Attila Engine Performance Fix).
           - Culling, on Light by default: small parts of far-away
           soldiers are not drawn (F8 > Culling, or culling= in
           rome2fix.ini).
           - huge battles: a sound the full sound memory gives no buffer
           is skipped instead of crashing the game.
           - F8 panel without the DLSS add-on too (Culling, What's new),
           What's new and "newer version" cards, the mod's version on the
           main menu.
           - addon=off in FrameBridge\settings.txt really turns the DLSS
           add-on off (the line sat past the part of the file that was
           read, so the add-on started anyway).
           - with the add-on: the F8 panel stays under the mouse after a
           resolution change, its cards close by themselves after 20 s,
           and with NVIDIA DLSS the add-on skips work only AMD FSR uses.
           - only one copy of the mod runs in a game; the crash logger no
           longer loses a record when the crashed thread's stack is
           broken; lua_ext.log keeps earlier lines.
           SHA-256 of dinput8.dll:
           b26454e599a4862cf8b4f666c205338a1970253b26340512b6b06ffeac2b0366
Version 2.1:
           - with the DLSS add-on (version 7): the settings panel opens
           with F8 (was F11, which other mods use); panel_key= in
           FrameBridge\settings.txt picks another key. When the add-on
           cannot start, F8 still opens the panel with a note, a small
           card at the top of the screen says what broke, and a graphics
           card without DirectX 12 feature level 12_0 is named.
           - with the add-on: the helper may take up to 30 s to start
           (slow laptops got no DLSS); switching DLSS / FSR in the panel
           is quicker and safer; the add-on's video memory (about 400 MB
           at 4K) is freed when it turns off.
           - plugins folder: other mods' DLLs load from it (see "For
           modders").
           - Lua scripts and a Lua console (lua_ext folder).
           - crash reports: a crash dump file, a freeze line, your PC's
           specs and mod list, and memory warnings in rome2fix.log; the
           crash logger itself can no longer crash on a damaged module.
           - fps_on.txt: a frame-time log (fps_log.csv).
           - self-update no longer takes a Workshop file Steam hasn't
           finished downloading.
           SHA-256 of dinput8.dll:
           654ea8f1ff1838eaa1116fb361255f2b7e933dac405ca21629ed9919a9411a0b
Version 2.0:
           - with the DLSS add-on (version 6): AMD FSR upscaling and frame
           generation for AMD, Intel and older NVIDIA cards, and F11's
           Upscaler / Frame Gen Tech choice (switches at once).
           - with the add-on: motion data for swaying trees and plants;
           Motion Vectors Full is the default for everyone, and what
           barely moves in a frame is skipped, so it costs less.
           - with the add-on: F11 FPS counter, recommended setup with a
           Defaults button, and the V-Sync row shows a V-Sync forced by
           the graphics driver.
           - with the add-on: sharper motion data far away (no smearing
           on sky and distant land), textures stay sharp with upscaling in
           more places, and a helper that stops answering no longer holds
           the game up.
           - a rare freeze at game start is fixed: the x87 fix could pause
           a game thread while it held Windows' DLL loader lock.
           - rome2fix.ini: "yes" (and other words) no longer switch a fix
           off; only 0, off, false and no do.
           - from a code review: a rare failed load or crash when one file
           closed while another opened; rome2fix.log now stays short
           after a very long session too; the crash that closes the game
           is always recorded in it; small fixes in the add-on's F11
           panel, ReShade link and motion data.
           SHA-256 of dinput8.dll:
           2d9e3b0c1ad0a5d74998d10ad77e8fa266a1f756c14a1e1581ad274ebe7b4554
Version 1.9: with the DLSS add-on:
           -fix1: thin white lines along the right edge of the screen with
           DLAA while the camera moved (and the bottom edge the same way).
           Half the frames left the last column / row of the picture
           empty and DLSS smeared it.
           -fix2: V-Sync On / Off in the F11 panel (vsync= in
           FrameBridge\settings.txt). On stays the default (no tearing);
           Off takes the cap at the screen's refresh rate away, but the
           picture may tear. Needs add-on version 5.
           SHA-256 of dinput8.dll:
           9d913468ff15b268d6aa859bd1e62f378cfe40ef0ac6d2a0a378ca36b1f12e05
Version 1.8: with the DLSS add-on and DLSS upscaling or DLAA: a moment where
           the graphics card is busy (for example while ReShade loads its
           effects) no longer switches DLSS off for the rest of the session.
           DLSS now only turns off when its helper has really stopped
           (closed, failed, or no answer for 3 seconds). Slow frames are
           noted in rome2fix.log.
           SHA-256 of dinput8.dll:
           ab55488009c52f903074d15827b5ab3fdf680fc7d7dc49c9439ec026c2b5fb0c
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
