#
# Makefile for the Linux NET/ROM layer.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definition is now in the main makefile...

O_TARGET := netrom.o
O_OBJS	 := af_netrom.o sysctl_net_netrom.o nr_dev.o nr_in.o nr_out.o nr_route.o nr_subr.o nr_timer.o nr_loopback.o
M_OBJS   := $(O_TARGET)

include $(TOPDIR)/Rules.make

tar:
		tar -cvf /dev/f1 .
