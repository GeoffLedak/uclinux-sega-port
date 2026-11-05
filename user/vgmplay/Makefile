
EXEC = vgmplay
OBJS = vgmplay.o

CFLAGS += -I. -I$(ROOTDIR)/lib/zlib

all: $(EXEC)

$(EXEC): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lz $(LDLIBS)

romfs:
	$(ROMFSINST) /bin/$(EXEC)
	$(ROMFSINST) music /usr/music

clean:
	-rm -f $(EXEC) *.elf *.gdb *.o
