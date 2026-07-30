# Where's My Water? 2 — Nintendo Switch port (loader wrapper)
 
This is a native wrapper / loader that runs the original ARM64 build of
*Where's My Water? 2* on Switch homebrew. It contains no game code and no game
assets. It loads the game's own native libraries (`libwalaber.so`,
`libfmodex.so`).
 
## Install & run
 
You need files from `com.disney.wheresmywater2_goo`.
 
Copy the `.nro` to your SD card (e.g. `sdmc:/switch/wmw2_nx/wmw2_nx.nro`), then
place your game files next to the `.nro`, in the same folder:
 
```
sdmc:/switch/wmw2_nx
├── wmw2_nx.nro
├── libwalaber.so              <- from your APK: lib/arm64-v8a/
├── libfmodex.so               <- from your APK: lib/arm64-v8a/
└── assets/
    └── Water/                 <- from your APK: assets/Water/, keep the Water folder
        └── ... (Data, Textures, Levels, Movies, Audio, Sprites, ...)
```

`libwalaber.so` and `libfmodex.so` — the 64-bit libraries from `lib/arm64-v8a/`
in your APK. (32-bit `armeabi-v7a` will not work; this wrapper is arm64.)

Optionally, drop a `cursor.png` (≤64x64, transparency supported) to replace the
on-screen cursor with your own.

`config.txt` is generated on first run and controls screen rotation.

## Saves and add-on content
 
Your game save lives in `migs_profile.json`, next to the `.nro`. Level progress,
audio settings and owned add-ons are all in that one file — back it up, or
delete it to start over.
 
The in-app purchases are recorded there too:
 
```json
"IAPInfo__StarterBundle01": { "InternalID": "StarterBundle01", "BuyCount": "0" }
```
 
Set `BuyCount` to `"1"` and the game treats that item as owned. Nothing is
patched and no game code is modified — this is the same record the game writes
itself. Quit the game before editing, or your change will be overwritten.

## Controls
 
| Input | Action |
|---|---|
| Touchscreen | Direct, handheld only |
| Left stick | Move the cursor |
| A / ZR / ZL | Tap and hold — press, drag, release digs |
| + | Toggle the on-screen cursor |
| − | Toggle gyro pointing (tilt/turn the controller to aim) |
| L / R | Recentre the cursor |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |
| A / + during a cutscene | Skip it |
 
A USB mouse works in both handheld and docked.

## Building
 
Requires devkitPro with the `switch-dev` group plus these portlibs:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib
pacman -S switch-ffmpeg switch-pkg-config
```
## Legal
 
No affiliation with Disney or Creature Feep. *Where's My Water? 2* is © Disney.
This is an independent, non-commercial interoperability wrapper. No game program
code, level data, art or audio ships here or may be distributed with builds.
 
## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `util`, `error`,
`nx_pointer`, `opensles`) derives from the open-source Switch ports of Burger
Shop and Bloons TD 5 by Andy Nguyen, fgsfds and ChanseyIsTheBest, which in turn
build on TheOfficialFloW's Vita/Switch loader lineage — all MIT-licensed. The
portrait rotation approach follows the Papers Please port, and the newlib
file-table locking follows BTD5's analysis. The cutscene player follows the
ffmpeg approach used by the CloverPit and Chaos Rings 3 Switch ports, rewritten
for GLES 1.1. The WMW2-specific JNI, MIGS profile store, platform callbacks,
audio, imports, asset index and main loop in this project are new.
