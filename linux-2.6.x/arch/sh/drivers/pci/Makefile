#
# Makefile for the PCI specific kernel interface routines under Linux.
#

obj-y					+= pci.o
obj-$(CONFIG_PCI_AUTO)			+= pci-auto.o
obj-$(CONFIG_PCI_DMA)			+= pci-dma.o

obj-$(CONFIG_CPU_SUBTYPE_ST40STB1)	+= pci-st40.o
obj-$(CONFIG_CPU_SUBTYPE_SH7751)	+= pci-sh7751.o 

obj-$(CONFIG_SH_DREAMCAST)		+= ops-dreamcast.o fixups-dreamcast.o \
					   dma-dreamcast.o

