/*
 * QEMU VGT support
 *
 * Copyright (c) Intel
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/i386/pc.h"

DeviceState *vgt_vga_init(PCIBus *pci_bus)
{
    return NULL;
}

void vgt_bridge_pci_conf_init(PCIDevice *pdev)
{
}

void vgt_kvm_set_opregion_addr(uint32_t addr)
{
}

void vgt_bridge_pci_write(PCIDevice *dev,
                          uint32_t address, uint32_t val, int len)
{
}


