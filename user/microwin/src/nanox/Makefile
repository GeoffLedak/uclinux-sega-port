##############################################################################
# Microwindows template Makefile
# Copyright (c) 2000 Martin Jolicoeur, Greg Haerr
##############################################################################

ifndef TOP
TOP = ..
CONFIG = $(TOP)/config
endif

include $(CONFIG)

######################## Additional Flags section ############################

# Directories list for header files
INCLUDEDIRS +=

# Defines for preprocessor
ifeq ($(LINK_APP_INTO_SERVER), Y)
DEFINES += -DNONETWORK=1
endif

# Compilation flags for C files OTHER than include directories
CFLAGS +=
# Preprocessor flags OTHER than defines
CPPFLAGS +=
# Linking flags
LDFLAGS +=

############################# targets section ################################

ifeq ($(NANOX), Y)

# If you want to create a library with the objects files, define the name here
LIBNAME = libnano-X.a
LIBNAMESO = libnano-X.so

# Nano-X server files
NANOXFILES = srvmain.o srvfunc.o srvutil.o srvevent.o srvclip.o
NETFILES = srvnet.o
UTILFILES = nxdraw.o nxutil.o

ifeq ($(LINK_APP_INTO_SERVER), Y)
NANOXFILES += $(UTILFILES)
OBJS = $(NANOXFILES)
else
NANOXFILES += $(NETFILES)
OBJS = client.o nxproto.o error.o $(UTILFILES)
all: default $(TOP)/bin/nano-X
endif

endif

######################### Makefile.rules section #############################

include $(TOP)/Makefile.rules

######################## Tools targets section ###############################

# Server binary ...
ifeq ($(SHAREDLIBS), Y)
$(TOP)/bin/nano-X: $(NANOXFILES) $(NANOXSERVERLIBS) $(TOP)/config
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(NANOXFILES) $(CCNANOXSERVERLIBS) $(LDLIBS)
else
$(TOP)/bin/nano-X: $(NANOXFILES) $(NANOXSERVERLIBS) $(TOP)/config
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(NANOXFILES) $(NANOXSERVERLIBS) $(LDLIBS)
endif
