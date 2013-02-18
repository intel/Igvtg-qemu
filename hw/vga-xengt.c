/*
 * QEMU vGT/XenGT Legacy VGA support
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) Citrix Systems, Inc
 * Copyright (c) Intel
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
#include "hw.h"
#include "console.h"
#include "pc.h"
#include "pci.h"
#include "pci_host.h"
#include "pci_bridge.h"
#include "pci_internals.h"
#include "vga_int.h"
#include "pixel_ops.h"
#include "qemu-timer.h"
#include "loader.h"
#include "xen_pt.h"
#include "vga-xengt.h"
#include "qemu-log.h"

typedef struct VGTVGAState {
    PCIDevice dev;
    struct VGACommonState state;
    int num_displays;
    XenHostPCIDevice host_dev;
    bool instance_created;
} VGTVGAState;

/* These are the default values */
int vgt_aperture_sz = 128; /* in MB */
int vgt_gm_sz = 512; /* in MB */
int vgt_fence_sz = 4;
int vgt_primary = 1; /* -1 means "not specified */

/*
 *  Inform vGT driver to create a vGT instance
 */
static void create_vgt_instance(void)
{
    /* FIXME: this should be substituded as a environment variable */
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;

    qemu_log("vGT: %s: domid=%d, aperture_sz=%dMB, gm_sz=%dMB, "
        "fence_sz=%d, vgt_primary=%d\n", __func__, xen_domid,
        vgt_aperture_sz, vgt_gm_sz, vgt_fence_sz, vgt_primary);
    if (vgt_aperture_sz <= 0 ||  vgt_aperture_sz > vgt_gm_sz ||
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
    if (!err && fprintf(vgt_file, "%d,%u,%u,%u,%d\n", xen_domid,
        vgt_aperture_sz, vgt_gm_sz, vgt_fence_sz, vgt_primary) < 0)
        err = errno;

    if (!err && fclose(vgt_file) != 0)
        err = errno;

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
    FILE *vgt_file, *shell_output;
    int err = 0;
    int tmp, fast_switch = 0;

    qemu_log("vGT: %s: domid=%d\n", __func__, xen_domid);

    if ((vgt_file = fopen(path, "w")) == NULL) {
        fprintf(stdout, "vGT: open %s failed\n", path);
        err = errno;
    }

    shell_output = popen("(cat /sys/kernel/vgt/control/display_switch_method "
        "2>/dev/null | grep -q 'using the fast-path method') "
        "&& echo 0xdeadbeaf", "r");
    if (shell_output != NULL && fscanf(shell_output, "%x", &tmp) == 1 &&
            tmp == 0xdeadbeaf)
        fast_switch = 1;
    fprintf(stderr, "vGT: the vgt driver is using %s display switch\n",
        fast_switch ? "fast" : "slow");
    if (shell_output != NULL)
        pclose(shell_output);

    //use the slow method temperarily to workaround the issue "win7 shutdown
    //makes the SNB laptop's LVDS screen always black.
    if (fast_switch)
        system("echo 0 > /sys/kernel/vgt/control/display_switch_method");

    /* -domid means we want the vgt driver to free the vgt instance
     * of Domain domid.
     * */
    if (!err && fprintf(vgt_file, "%d\n", -xen_domid) < 0)
        err = errno;

    if (!err && fclose(vgt_file) != 0)
        err = errno;

    //restore to the fast method
    if (fast_switch)
        system("echo 1 > /sys/kernel/vgt/control/display_switch_method");

    if (err) {
        qemu_log("vGT: %s: failed: errno=%d\n", __func__, err);
        exit(-1);
    }
}

static int pch_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return irq_num;
}

void vgt_bridge_pci_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len)
{
#if 0
    VGTVGAState *o = DO_UPCAST(VGTVGAState, dev, dev);
#endif
    assert(dev->devfn == 0x00);

    XEN_PT_LOG(dev, "vGT Config Write: addr=%x len=%x val=%x\n", addr, len, val);

    switch (addr)
    {
#if 0
        case 0x58:        // PAVPC Offset
            xen_host_pci_set_block(o->host_dev, addr, val, len);
            break;
#endif
        default:
            pci_default_write_config(dev, addr, val, len);
    }
}

static void vgt_bridge_pci_conf_init_from_host(PCIDevice *dev,
        uint32_t addr, int len)
{
    VGTVGAState *o = DO_UPCAST(VGTVGAState, dev, dev);

    if(len > 4){
        XEN_PT_LOG(dev, "WARNIGN: length %x too large for config addr %x, ignore init\n",
                len, addr);
        return;
    }

    xen_host_pci_get_block(&o->host_dev, addr, dev->config + addr, len);
}

static void vgt_host_bridge_cap_init(PCIDevice *dev)
{
    VGTVGAState *o = DO_UPCAST(VGTVGAState, dev, dev);
    assert(dev->devfn == 0x00);
	uint8_t cap_ptr;

	xen_host_pci_get_byte(&o->host_dev, 0x34, &cap_ptr);

	while(cap_ptr !=0 ){
		vgt_bridge_pci_conf_init_from_host(dev, cap_ptr, 4); /* capability */
		vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 4, 4); /* capability */
		vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 8, 4); /* capability */
		vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 12, 4); /* capability */
//		XEN_PT_LOG(pci_dev, "Add vgt host bridge capability: offset=0x%x, cap=0x%x\n", cap_ptr,
//				pt_pci_host_read(0, PCI_SLOT(pci_dev->devfn), 0, cap_ptr, 1) & 0xFF );
		xen_host_pci_get_byte(&o->host_dev, cap_ptr + 1, &cap_ptr);
	}
}


void vgt_bridge_pci_conf_init(PCIDevice *pci_dev)
{
	vgt_bridge_pci_conf_init_from_host(pci_dev, 0x00, 2); /* vendor id */
	vgt_bridge_pci_conf_init_from_host(pci_dev, 0x02, 2); /* device id */
	vgt_bridge_pci_conf_init_from_host(pci_dev, 0x06, 2); /* status */
	vgt_bridge_pci_conf_init_from_host(pci_dev, 0x08, 2); /* revision id */
	vgt_bridge_pci_conf_init_from_host(pci_dev, 0x34, 1); /* capability */
	vgt_host_bridge_cap_init(pci_dev);
	vgt_bridge_pci_conf_init_from_host(pci_dev, 0x50, 2); /* SNB: processor graphics control register */
	vgt_bridge_pci_conf_init_from_host(pci_dev, 0x52, 2); /* processor graphics control register */
}

uint32_t vgt_bridge_pci_read(PCIDevice *pci_dev, uint32_t config_addr, int len)
{
	uint32_t val;

	val = pci_default_read_config(pci_dev, config_addr, len);
	XEN_PT_LOG(pci_dev, "addr=%x len=%x val=%x\n", config_addr, len, val);

	return val;
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

static int vgt_initfn(PCIDevice *dev)
{
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, dev);

    printf("vgt_initfn\n");
    d->instance_created = FALSE;

    xen_host_pci_device_get(&d->host_dev, xen_domid, 0, 0x1f, 0);

    create_vgt_instance();
    return 0;
}

DeviceState *xengt_vga_init(PCIBus *pci_bus)
{
    PCIDevice *dev;
    XenHostPCIDevice host_dev;
    PCIBridge *br;

    if( xen_host_pci_device_get(&host_dev, 0, 0, 0x1f, 0) < 0)
    {
        fprintf(stderr, " Error, failed to get host PCI device\n");
        return NULL;
    }

    if ( host_dev.vendor_id != 0x8086 ) {

        xen_host_pci_device_put(&host_dev);
        fprintf(stderr, " Error, vga-xengt is only supported on Intel GPUs\n");
	return NULL;
    }

    xen_host_pci_device_put(&host_dev);

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x1f, 0), true,
                                   "xengt-isa");
    if (!dev) {
        fprintf(stderr, "Warning: vga-xengt not available\n");
        return NULL;
    }

    qdev_init_nofail(&dev->qdev);

    pci_config_set_vendor_id(dev->config, host_dev.vendor_id);
    pci_config_set_device_id(dev->config, host_dev.device_id);
    pci_config_set_revision(dev->config, host_dev.revision_id);
    br = DO_UPCAST(PCIBridge, dev, dev);
    pci_bridge_map_irq(br, "IGD Bridge",
                       pch_map_irq);

    printf("Create xengt ISA bridge successfully\n");

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x2, 0), true,
                                   "xengt-vga");
    if (!dev) {
        fprintf(stderr, "Warning: vga-xengt not available\n");
        return NULL;
    }
    qdev_init_nofail(&dev->qdev);
    printf("Create xengt VGA successfully\n");
    return &dev->qdev;
}

static void vgt_class_initfn(ObjectClass *klass, void *data)
{
    printf("vgt_class_initfn\n");
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *ic = PCI_DEVICE_CLASS(klass);
    ic->init = vgt_initfn;
    dc->reset = vgt_reset;
    ic->exit = vgt_cleanupfn;
    dc->vmsd = &vmstate_vga_common;
}

static TypeInfo vgt_info = {
    .name          = "xengt-vga",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VGTVGAState),
    .class_init    = vgt_class_initfn,
};

static TypeInfo isa_info = {
    .name          = "xengt-isa",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VGTVGAState),
};

static void vgt_register_types(void)
{
    type_register_static(&vgt_info);
    type_register_static(&isa_info);
}

type_init(vgt_register_types)
