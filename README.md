# C64 door

A Commodore 64 as a BBS door: VICE's x64sc (as a libretro core) runs on the BBS box and callers play it in SyncTERM,
the screen sent as JPEG XL pictures. Callers load titles from the sysop's library or upload their own disks by ZMODEM.

**No Commodore ROMs are included.** The C64's BASIC, KERNAL and character ROMs and the drive ROMs are still under
copyright: the core is built without its usual built-in copies, and each sysop supplies their own files.
`door/INSTALL.md` has the build steps and the list of ROM files and where they go.

Everything is in `door/`: the door itself (C, `make`), `build-core.sh` and `vice-noroms.patch` for the ROM-less core,
and `test_door.py`.

The door is free software under the GNU General Public License, version 2 or (at your option) any later version:
see `LICENSE`. VICE is GPL-2 too (https://vice-emu.sourceforge.io). The libjxl headers in `door/jxl_include/` carry
their own BSD licence.
