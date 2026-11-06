EXEC = win
OBJS = main.o gfx.o gfx68k.o wm.o ctl.o hw.o art.o progman.o bench.o notepad.o calc.o clock.o puzzle.o \
	paint.o filemgr.o control.o

CFLAGS += -I.
FLTFLAGS += -s 16384

all: $(EXEC)

$(EXEC): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): win.h art.h

# the drawing and window code runs all the time: optimise it for speed
# (everything else is built for size, -Os)
gfx.o wm.o ctl.o notepad.o: CFLAGS += -O2

# the assembly is written without % before register names
gfx68k.o: gfx68k.S
	$(CC) -m68000 -Wa,--register-prefix-optional -c -o $@ $<

romfs:
	$(ROMFSINST) /bin/$(EXEC)

clean:
	-rm -f $(EXEC) *.elf *.gdb *.o win-host
