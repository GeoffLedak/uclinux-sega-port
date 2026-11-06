EXEC = win
OBJS = main.o gfx.o wm.o ctl.o hw.o art.o progman.o notepad.o calc.o clock.o puzzle.o \
	paint.o filemgr.o control.o

CFLAGS += -I.
FLTFLAGS += -s 16384

all: $(EXEC)

$(EXEC): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): win.h art.h

romfs:
	$(ROMFSINST) /bin/$(EXEC)

clean:
	-rm -f $(EXEC) *.elf *.gdb *.o win-host
