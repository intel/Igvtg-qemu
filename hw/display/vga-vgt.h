#ifndef __VGT_H__
#define __VGT_H__

#define OPREGION_SIZE 0x2000
DeviceState *vgt_vga_init(PCIBus *pci_bus);
void vgt_pci_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len);
uint32_t vgt_pci_read(PCIDevice *pci_dev, uint32_t config_addr, int len);
void vgt_opregion_reserve(MemoryRegion *mr, ram_addr_t addr);

#endif /* __VGT_H__ */
