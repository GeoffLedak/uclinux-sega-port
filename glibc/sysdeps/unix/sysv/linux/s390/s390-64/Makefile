ifeq ($(subdir),misc)
sysdep_routines += setfsgid setfsuid setresgid setresuid
sysdep_headers += sys/elf.h
endif

ifeq ($(subdir),elf)
ifeq (yes,$(build-shared))
# This is needed to support g++ v2 and v3.
sysdep_routines += framestate
shared-only-routines += framestate
endif
endif
