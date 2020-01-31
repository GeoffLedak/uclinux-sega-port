cp linux-2.0.x/drivers/block/blkmem.c.normal linux-2.0.x/drivers/block/blkmem.c
cp linux-2.0.x/arch/m68knommu/platform/68000/68Katy/crt0_rom.S.rom linux-2.0.x/arch/m68knommu/platform/68000/68Katy/crt0_rom.S
cp linux-2.0.x/arch/m68knommu/platform/68000/68Katy/rom.ld.rom linux-2.0.x/arch/m68knommu/platform/68000/68Katy/rom.ld
rm linux-2.0.x/drivers/block/blkmem.o
make 

