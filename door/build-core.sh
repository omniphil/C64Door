#!/bin/sh
# Builds the door's C64, vice_x64sc_libretro.so, from libretro/vice-libretro with no Commodore ROM inside it.
#
# Stock vice-libretro compiles the C64's BASIC, KERNAL and character ROMs and the drive ROMs into the core.
# vice-noroms.patch makes the core skip those built-in copies, so it reads every ROM from the door's system/vice/
# folder instead, where the sysop puts their own (INSTALL.md lists the files). The built core holds no ROM bytes.
#
#   sh build-core.sh [build dir]        (default: ./vice-libretro-build; needs git, gcc, g++, make)
#
# The commit is pinned: the patch and the door's core options (core.c) were tested against it.
set -e

COMMIT=9d7983826ea792f6cce7fdfe6c09488129c6f886
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HERE/vice-libretro-build}

if [ ! -d "$SRC/.git" ]; then
    git clone https://github.com/libretro/vice-libretro.git "$SRC"
fi
cd "$SRC"
git fetch -q origin "$COMMIT" 2>/dev/null || true
git checkout -q -f "$COMMIT"
git apply "$HERE/vice-noroms.patch"
make EMUTYPE=x64sc -j"$(nproc)"
cp vice_x64sc_libretro.so "$HERE/"
git checkout -q -- .                        # leave the clone clean, so a second run applies the patch again
echo "Built $HERE/vice_x64sc_libretro.so. Now put your ROMs in $HERE/system/vice/ (see INSTALL.md)."
