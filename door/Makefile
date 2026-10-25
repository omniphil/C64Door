# The C64 door. Built on the BBS box: `make`. It needs only gcc; libjxl (0.11 or newer, for its lossless sizes) is
# loaded at run time, and its headers are in jxl_include/. The C64 itself is vice_x64sc_libretro.so beside the door,
# built from libretro/vice-libretro (`make EMUTYPE=x64sc`, see INSTALL.md).
CC = gcc
CFLAGS = -Wall -Wextra -Wno-format-truncation -std=c11 -O2 -Ijxl_include
LDFLAGS = -lm -ldl

OBJ = main.o door.o apc.o jxl_enc.o core.o play.o session.o

c64door: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

main.o: main.c door.h apc.h jxl_enc.h play.h session.h
core.o: core.c core.h libretro.h
play.o: play.c play.h core.h apc.h jxl_enc.h libretro.h session.h
session.o: session.c session.h apc.h

clean:
	rm -f c64door $(OBJ)

.PHONY: clean
