# Installing the C64 door on the BBS

## What the door folder holds

| File | What | Where it comes from |
| --- | --- | --- |
| `c64door` | the door | `make` here, on the BBS box |
| `vice_x64sc_libretro.so` | the C64 (VICE x64sc, GPL-2), with no ROMs inside | `sh build-core.sh` here (clones libretro/vice-libretro at a pinned commit and applies `vice-noroms.patch`) |
| `system/vice/C64/`, `system/vice/DRIVES/` | the Commodore ROMs | **the sysop's own** (below); never in the repo |
| `library/library.txt` + disk images | the library | the sysop: one line a title, `title \| description \| disk1.d64,disk2.d64` |
| `logs/` | one log per player, rewritten each session | made by the door |
| `players/<handle>-<number>/` | each caller's kept disks (their uploads, saves and all; up to 20) | made by the door |
| `test_door.py` | pretends to be SyncTERM | here |

## The ROMs

The C64's ROMs are still under copyright, so they aren't in this repo and the core is built without its usual
built-in copies. Put your own in the door folder under these names (the ones VICE uses; a VICE install's `C64` and
`DRIVES` data folders have them):

```
system/vice/C64/basic-901226-01.bin         8 KB  BASIC V2
system/vice/C64/kernal-901227-03.bin        8 KB  KERNAL, revision 3
system/vice/C64/chargen-901225-01.bin       4 KB  character set
system/vice/DRIVES/dos1541ii-251968-03.bin 16 KB  1541-II drive
system/vice/DRIVES/dos1581-318045-02.bin   32 KB  1581 drive (for .D81 disks)
```

Without them the door tells callers it isn't installed properly and names the first file missing (the core itself
would crash). JiffyDOS isn't used: some games' own fast
loaders hang with its drive ROM (The Last Ninja), and the door speeds loading up itself: while the drive is busy and
the C64 is silent it runs the C64 about 5 times faster (`C64DOOR_NO_WARP=1` turns that off). A .D81 gets a 1581 in
drive 8 (the core switches by itself). A .CRT (up to 2 MB, EasyFlash included) goes in the cartridge port and starts at
once. While the drive is busy a small 1541 light shows in the picture's corner.

The box needs **libjxl 0.11 or newer** (Ubuntu 25.10 has it; 0.7 makes lossless pictures about 3 times bigger),
**lrzsz** (`rz`/`sz`, for uploads and downloads) and, for the sandbox, **bubblewrap** (`bwrap`). Without a working
bwrap the C64 still runs, only not boxed in.

## Steps

1. Copy the door folder to the box and build there: `make clean && make`, then `sh build-core.sh` for the C64.
   Put your ROMs in `system/vice/` (above).
2. Test it in a **staging copy**, never in the live folder (the test writes `logs/`). The tests boot the first two
   titles in the sample `library/library.txt`, freeware the repo doesn't carry: get `hessian.d64` from Covert Bitops
   and the three Edge of Disgrace disks from CSDb, and put them in `library/`.
   `python3 test_door.py` (Hessian, 20 s, opened through Ctrl-Q and the library) and
   `python3 test_door.py --title 2 --seconds 60 --swap-at 30` (the demo, side 1a in through F12 at 30 s).
3. Add a door entry to your BBS that writes a **door32.sys** drop file and runs `c64door` with the node's drop file
   folder (or the door32.sys file itself) as its one argument. The door talks to the caller on stdin/stdout, so the
   BBS must hand it the connection that way (Mystic does on Linux).
   On Mystic that is a menu command **D3** with the data `<door folder>/c64door <mystic>/temp%3`.

## What callers see

The disclaimer, a short how-to, then the C64 at the BASIC prompt. Everything else is in the **F12** menu (**Ctrl-Q**
opens it too, for keyboards whose F12 needs Fn): load a library title (the C64 restarts with it and starts it),
upload their own disks by ZMODEM (kept for their next visit), boot or delete a kept disk (**M**), next disk, joystick port, arrows as joystick or cursor keys, sound, reset (to the BASIC prompt, the
disk left in the drive), and
**Q** back to the BBS. On the way out, any disk a game wrote to is offered as a ZMODEM download.

## Per-player link test

`logs/<player>.linktest` holding `kbps=300 ping=80` makes that player's sessions go through a modelled link of that
speed and round trip. Delete the file to go back.

## Debugging

- `logs/<player>.log`: what the terminal reported, then every 5 s the frames run and sent, KB/s, the link, sound
  pieces.
- `C64DOOR_DUMP=<dir>`: a snapshot of the C64's screen every 5 s, as PPM.
- `C64DOOR_CORE_LOG=<file>`: the core's own warnings.
