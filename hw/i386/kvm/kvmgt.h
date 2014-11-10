#ifndef _KVM_GT_
#define _KVM_GT_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "hw/pci/pci_regs.h"

typedef struct HostDevice {
    uint16_t s;
    uint8_t b, d, f;
    uint16_t vendor_id, device_id;
    uint8_t revision_id;
    int config_fd;
} HostDevice;

static inline void host_dev_get(HostDevice *dev)
{
    char name[PATH_MAX];

    snprintf(name, sizeof(name), "/sys/bus/pci/devices/%04x:%02x:%02x.%x/config",
            dev->s, dev->b, dev->d, dev->f);
    dev->config_fd = open(name, O_RDONLY);
    if (dev->config_fd == -1) {
        JERROR("open failed: %s\n", strerror(errno));
    }

    pread(dev->config_fd, &dev->vendor_id, sizeof(dev->vendor_id), PCI_VENDOR_ID);
    pread(dev->config_fd, &dev->device_id, sizeof(dev->device_id), PCI_DEVICE_ID);
    pread(dev->config_fd, &dev->revision_id, sizeof(dev->revision_id), PCI_REVISION_ID);

    JDPRINT("vendor: 0x%hx, device: 0x%hx, revision: 0x%hhx\n", dev->vendor_id, dev->device_id, dev->revision_id);
}

static inline void host_dev_put(HostDevice *dev)
{
    if (fcntl(dev->config_fd, F_GETFD) != -1)
        close(dev->config_fd);
}

static inline uint32_t host_dev_pci_read(uint16_t s, uint8_t b, uint8_t d,
        uint8_t f, uint32_t addr, int len)
{
    uint32_t val = 0;

    HostDevice host_dev = {
        .s = s,
        .b = b,
        .d = d,
        .f = f,
    };

    host_dev_get(&host_dev);
    pread(host_dev.config_fd, &val, len, addr);
    host_dev_put(&host_dev);

    return le32_to_cpu(val);
}

#define IGD_OPREGION    0xfc

extern int kvm_domid;
extern int vgt_low_gm_sz;
extern int vgt_high_gm_sz;
extern int vgt_fence_sz;

void vgt_opregion_init(void);

#endif /* _KVM_GT_ */
