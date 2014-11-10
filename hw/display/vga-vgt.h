#ifndef __VGT_H__
#define __VGT_H__

DeviceState *vgt_vga_init(PCIBus *pci_bus);
void vgt_pci_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len);
uint32_t vgt_pci_read(PCIDevice *pci_dev, uint32_t config_addr, int len);

#endif /* __VGT_H__ */
