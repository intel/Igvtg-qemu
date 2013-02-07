#ifndef __XENGT_H__
#define __XENGT_H__

DeviceState *xengt_vga_init(PCIBus *pci_bus);
void vgt_bridge_pci_conf_init(PCIDevice *dev);
void vgt_bridge_pci_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len);
uint32_t vgt_bridge_pci_read(PCIDevice *pci_dev, uint32_t config_addr, int len);
#endif
