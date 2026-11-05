
EXEC = vgmplay
OBJS = vgmplay.o

CFLAGS += -I.

all: $(EXEC)

$(EXEC): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

romfs:
	$(ROMFSINST) /bin/$(EXEC)
	$(ROMFSINST) music /usr/music

clean:
	-rm -f $(EXEC) *.elf *.gdb *.o
