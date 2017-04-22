/*
 * QEMU VGA Emulator.
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QEMU_HW_DISPLAY_VGA_H
#define QEMU_HW_DISPLAY_VGA_H

#include "exec/memory.h"

enum vga_retrace_method {
    VGA_RETRACE_DUMB,
    VGA_RETRACE_PRECISE
};

extern enum vga_retrace_method vga_retrace_method;

int isa_vga_mm_init(hwaddr vram_base,
                    hwaddr ctrl_base, int it_shift,
                    MemoryRegion *address_space);
/* vgt_vga.c */
extern int vgt_low_gm_sz;
extern int vgt_high_gm_sz;
extern int vgt_fence_sz;

DeviceState *vgt_vga_init(PCIBus *pci_bus);
void vgt_bridge_pci_conf_init(PCIDevice *dev);
void vgt_bridge_pci_write(PCIDevice *dev,
                          uint32_t address, uint32_t val, int len);
#endif
