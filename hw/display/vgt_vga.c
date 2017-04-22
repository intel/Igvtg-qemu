/*
 * QEMU vGT/XenGT Legacy VGA support
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) Citrix Systems, Inc
 * Copyright (c) Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_bus.h"
#include "vga_int.h"
#include "ui/pixel_ops.h"
#include "qemu/timer.h"
#include "hw/loader.h"
#include "qemu/log.h"
#include "sysemu/arch_init.h"
#include "hw/xen/xen.h"
#include "hw/display/vga.h"

//#define DEBUG_VGT

#ifdef DEBUG_VGT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "vgt: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

typedef struct VGTHostDevice {
    PCIHostDeviceAddress addr;
    int config_fd;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision_id;
    uint16_t class_dev;
} VGTHostDevice;

typedef struct VGTVGAState {
    PCIDevice dev;
    struct VGACommonState state;
    int num_displays;
    VGTHostDevice host_dev;
    bool instance_created;
} VGTVGAState;

/* These are the default values */
int vgt_low_gm_sz = 64; /* in MB */
int vgt_high_gm_sz = 448; /* in MB */
int vgt_fence_sz = 4;
int vgt_primary = 1; /* -1 means "not specified */

static int vgt_host_device_get(VGTHostDevice *dev);
static void vgt_host_device_put(VGTHostDevice *dev);

void vgt_bridge_pci_write(PCIDevice *dev,
                          uint32_t address, uint32_t val, int len)
{
#if 0
    VGTVGAState *o = DO_UPCAST(VGTVGAState, dev, dev);
#endif

    assert(dev->devfn == 0x00);

//  fprintf("vGT Config Write: addr=%x len=%x val=%x\n", addr, len, val);

    switch (address) {
#if 0
        case 0x58:        // PAVPC Offset
            xen_host_pci_set_block(o->host_dev, addr, val, len);
            break;
#endif
    }

    i440fx_write_config(dev, address, val, len);
}

/*
 *  Inform vGT driver to create a vGT instance
 */
static void create_vgt_instance(void)
{
    /* FIXME: this should be substituded as a environment variable */
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;

    qemu_log("vGT: %s: domid=%d, low_gm_sz=%dMB, high_gm_sz=%dMB, "
        "fence_sz=%d, vgt_primary=%d\n", __func__, xen_domid,
        vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary);
    if (vgt_low_gm_sz <= 0 || vgt_high_gm_sz <=0 ||
		vgt_primary < -1 || vgt_primary > 1 ||
        vgt_fence_sz <=0) {
        qemu_log("vGT: %s failed: invalid parameters!\n", __func__);
        abort();
    }

    if ((vgt_file = fopen(path, "w")) == NULL) {
        err = errno;
        qemu_log("vGT: open %s failed\n", path);
    }
    /* The format of the string is:
     * domid,aperture_size,gm_size,fence_size. This means we want the vgt
     * driver to create a vgt instanc for Domain domid with the required
     * parameters. NOTE: aperture_size and gm_size are in MB.
     */
    if (!err && fprintf(vgt_file, "%d,%u,%u,%u,%d,0\n", xen_domid,
        vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary) < 0) {
        err = errno;
    }

    if (!err && fclose(vgt_file) != 0) {
        err = errno;
    }

    if (err) {
        qemu_log("vGT: %s failed: errno=%d\n", __func__, err);
        exit(-1);
    }
}

/*
 *  Inform vGT driver to close a vGT instance
 */
static void destroy_vgt_instance(void)
{
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;

    if ((vgt_file = fopen(path, "w")) == NULL) {
        error_report("vgt: error: open %s failed", path);
        err = errno;
    }

    /* -domid means we want the vgt driver to free the vgt instance
     * of Domain domid.
     * */
    if (!err && fprintf(vgt_file, "%d\n", -xen_domid) < 0) {
        err = errno;
    }

    if (!err && fclose(vgt_file) != 0) {
        err = errno;
    }

    if (err) {
        qemu_log("vGT: %s: failed: errno=%d\n", __func__, err);
        exit(-1);
    }
}

static int pch_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return irq_num;
}

static void vgt_pci_conf_init_from_host(PCIDevice *dev,
        uint32_t addr, int len)
{
    int ret;

    if (len > 4) {
        error_report("WARNIGN: length %x too large for config addr %x, ignore init",
                len, addr);
        return;
    }

    VGTHostDevice host_dev = {
        .addr.domain = 0,
        .addr.bus = pci_dev_bus_num(dev),
        .addr.slot = PCI_SLOT(dev->devfn),
        .addr.function = PCI_FUNC(dev->devfn),
    };

    /* FIXME: need a better scheme to grab the root complex. This
     * only for a single VM scenario.
     */
    vgt_host_device_get(&host_dev);
    ret = pread(host_dev.config_fd, dev->config + addr, len, addr);
    if (ret < len) {
        error_report("%s, read config addr %x, len %d failed.", __func__, addr, len);
        return;
    }
    vgt_host_device_put(&host_dev);
}

static int vgt_host_pci_get_byte(VGTHostDevice *host_dev,
                                  uint32_t addr, uint8_t *p)
{
    int ret;
    uint8_t buf;


    vgt_host_device_get(host_dev);
    ret = pread(host_dev->config_fd, &buf, 1, addr);
    if (ret < 1) {
        error_report("%s, failed.", __func__);
        return ret;
    }
    vgt_host_device_put(host_dev);

    *p = buf;
    return ret;
}

static void vgt_host_bridge_cap_init(PCIDevice *dev)
{
    assert(dev->devfn == 0x00);
    uint8_t cap_ptr = 0;

    VGTHostDevice host_dev = {
        .addr.domain = 0,
        .addr.bus = 0,
        .addr.slot = 0,
        .addr.function = 0,
    };

    vgt_host_pci_get_byte(&host_dev, PCI_CAPABILITY_LIST, &cap_ptr);
    while (cap_ptr !=0) {
        vgt_pci_conf_init_from_host(dev, cap_ptr, 4); /* capability */
        vgt_pci_conf_init_from_host(dev, cap_ptr + 4, 4); /* capability */
        vgt_pci_conf_init_from_host(dev, cap_ptr + 8, 4); /* capability */
        vgt_pci_conf_init_from_host(dev, cap_ptr + 12, 4); /* capability */
        //XEN_PT_LOG(pci_dev, "Add vgt host bridge capability: offset=0x%x, cap=0x%x\n", cap_ptr,
        //    pt_pci_host_read(0, PCI_SLOT(pci_dev->devfn), 0, cap_ptr, 1) & 0xFF );
        vgt_host_pci_get_byte(&host_dev, cap_ptr + 1, &cap_ptr);
    }
}

void vgt_bridge_pci_conf_init(PCIDevice *pci_dev)
{
    printf("vgt_bridge_pci_conf_init\n");
    printf("vendor id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x00));
    vgt_pci_conf_init_from_host(pci_dev, 0x00, 2); /* vendor id */
    printf("vendor id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x00));
    printf("device id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x02));
    vgt_pci_conf_init_from_host(pci_dev, 0x02, 2); /* device id */
    printf("device id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x02));
    vgt_pci_conf_init_from_host(pci_dev, 0x06, 2); /* status */
    vgt_pci_conf_init_from_host(pci_dev, 0x08, 2); /* revision id */
    vgt_pci_conf_init_from_host(pci_dev, 0x34, 1); /* capability */
    vgt_host_bridge_cap_init(pci_dev);
    vgt_pci_conf_init_from_host(pci_dev, 0x50, 2); /* SNB: processor graphics control register */
    vgt_pci_conf_init_from_host(pci_dev, 0x52, 2); /* processor graphics control register */
}

static void vgt_reset(DeviceState *dev)
{
}

static void vgt_cleanupfn(PCIDevice *dev)
{
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, dev);

    if (d->instance_created) {
        destroy_vgt_instance();
    }
}

static void vgt_initfn(PCIDevice *dev, Error **errp)
{
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, dev);

    DPRINTF("vgt_initfn\n");
    d->instance_created = TRUE;

    create_vgt_instance();
}

static int vgt_host_device_get(VGTHostDevice *dev)
{
    char name[PATH_MAX];
    int ret;

    snprintf(name, sizeof(name), "/sys/bus/pci/devices/%04x:%02x:%02x.%x/config",
             dev->addr.domain, dev->addr.bus, dev->addr.slot, dev->addr.function);
    dev->config_fd = open(name, O_RDONLY);
    if (dev->config_fd == -1) {
        error_report("vgt: open failed: %s\n", strerror(errno));
        return -1;
    }

    ret = pread(dev->config_fd, &dev->vendor_id, sizeof(dev->vendor_id), PCI_VENDOR_ID);
    if (ret < sizeof(dev->vendor_id)) {
        goto error;
    }
    ret = pread(dev->config_fd, &dev->device_id, sizeof(dev->device_id), PCI_DEVICE_ID);
    if (ret < sizeof(dev->device_id)) {
        goto error;
    }
    ret = pread(dev->config_fd, &dev->revision_id, sizeof(dev->revision_id), PCI_REVISION_ID);
    if (ret < sizeof(dev->revision_id)) {
        goto error;
    }
    ret = pread(dev->config_fd, &dev->class_dev, sizeof(dev->class_dev), PCI_CLASS_DEVICE);
    if (ret < sizeof(dev->class_dev)) {
        goto error;
    }
    DPRINTF("vendor: 0x%hx, device: 0x%hx, revision: 0x%hhx\n",
           dev->vendor_id, dev->device_id, dev->revision_id);

    return 0;

error:
    ret = ret < 0 ? -errno : -EFAULT;
    error_report("vgt: Failed to read device config space");
    return ret;
}

static void vgt_host_device_put(VGTHostDevice *dev)
{
    if (dev->config_fd >= 0) {
        close(dev->config_fd);
        dev->config_fd = -1;
    }
}

DeviceState *vgt_vga_init(PCIBus *pci_bus)
{
    PCIDevice *dev;
    PCIBridge *br;
    VGTHostDevice host_dev = {
        .addr.domain = 0,
        .addr.bus = 0,
        .addr.slot = 0x1f,
        .addr.function = 0,
    };

    if (vgt_host_device_get(&host_dev) < 0) {
        error_report("vgt: error: failed to get host PCI device");
        return NULL;
    }

    if (host_dev.vendor_id != PCI_VENDOR_ID_INTEL) {
        vgt_host_device_put(&host_dev);
        error_report("vgt: error: vgt-vga is only supported on Intel GPUs");
        return NULL;
    }

    vgt_host_device_put(&host_dev);

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x1f, 0), true,
                                   "vgt-isa");
    if (!dev) {
        error_report("vgt: error: vgt-isa not available");
        return NULL;
    }

    qdev_init_nofail(&dev->qdev);

    pci_config_set_vendor_id(dev->config, host_dev.vendor_id);
    pci_config_set_device_id(dev->config, host_dev.device_id);
    pci_config_set_revision(dev->config, host_dev.revision_id);
    pci_config_set_class(dev->config, host_dev.class_dev);
    br = PCI_BRIDGE(dev);
    pci_bridge_map_irq(br, "IGD Bridge",
                       pch_map_irq);

    printf("Create vgt ISA bridge successfully\n");

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x2, 0), true,
                                   "vgt-vga");
    if (!dev) {
        error_report("vgt: error: vgt-vga not available");
        return NULL;
    }

    qdev_init_nofail(&dev->qdev);
    printf("Create vgt VGA successfully\n");
    return DEVICE(dev);
}

static void vgt_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *ic = PCI_DEVICE_CLASS(klass);
    ic->realize = vgt_initfn;
    dc->reset = vgt_reset;
    ic->exit = vgt_cleanupfn;
    dc->vmsd = &vmstate_vga_common;
}

static TypeInfo vgt_info = {
    .name          = "vgt-vga",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VGTVGAState),
    .class_init    = vgt_class_initfn,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { }
    },
};

static TypeInfo isa_info = {
    .name          = "vgt-isa",
    .parent        = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(PCIBridge),
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { }
    },

};

static void vgt_register_types(void)
{
    type_register_static(&vgt_info);
    type_register_static(&isa_info);
}

type_init(vgt_register_types)
