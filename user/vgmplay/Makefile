
EXEC = vgmplay
OBJS = vgmplay.o

CFLAGS += -I. -I$(ROOTDIR)/lib/zlib

all: $(EXEC)

$(EXEC): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lz $(LDLIBS)

# z80prog.h is the Z80 half, made from player.z80 by hand (the build
# has no python3):  python3 mkz80.py player.z80 z80prog.h
vgmplay.o: z80prog.h

romfs:
	$(ROMFSINST) /bin/$(EXEC)
	$(ROMFSINST) music /usr/music

clean:
	-rm -f $(EXEC) *.elf *.gdb *.o
