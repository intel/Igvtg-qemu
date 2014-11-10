/*
 * QEMU VGT/KVMGT Legacy VGA support
 *
 * Copyright (c) 2014 Intel
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
#include "config.h"

#ifdef CONFIG_KVM
#include "hw/i386/kvm/kvmgt.h"
#include "sysemu/arch_init.h"
#include "sysemu/sysemu.h"
#endif

#include "vga-vgt.h"
#include "qemu/log.h"

typedef struct VGTVGAState {
    PCIDevice dev;
    struct VGACommonState state;
    int num_displays;
    bool instance_created;
} VGTVGAState;

/* These are the default values */
int vgt_low_gm_sz = 64; /* in MB */
int vgt_high_gm_sz = 448; /* in MB */
int vgt_fence_sz = 4;
int vgt_primary = 1; /* -1 means "not specified */

int kvm_domid = 1;
extern uint32_t xen_domid;

/*
 *  Inform vGT driver to create a vGT instance
 */
static void create_vgt_instance(void)
{
    /* FIXME: this should be substituded as a environment variable */
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;
    int domid;

    /* get a resonable domid under either xen or kvm */
    domid = kvm_available() ? kvm_domid : xen_domid;

    qemu_log("vGT: %s: domid=%d, low_gm_sz=%dMB, high_gm_sz=%dMB, "
        "fence_sz=%d, vgt_primary=%d\n", __func__, domid,
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
    if (!err && fprintf(vgt_file, "%d,%u,%u,%u,%d\n", domid,
        vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary) < 0)
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
    int domid = kvm_available() ? kvm_domid : xen_domid;

    qemu_log("vGT: %s: domid=%d\n", __func__, domid);

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
    if (!err && fprintf(vgt_file, "%d\n", -domid) < 0)
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

static void vgt_pci_conf_init_from_host(PCIDevice *dev,
        uint32_t addr, int len)
{
    if(len > 4){
        JERROR("WARNIGN: length %x too large for config addr %x, ignore init\n",
                len, addr);
        return;
    }

    HostDevice host_dev = {
        .s = 0,
        .b = 0,
        .d = 2,
        .f = 0,
    };

    host_dev_get(&host_dev);
    pread(host_dev.config_fd, dev->config + addr, len, addr);
    host_dev_put(&host_dev);
}

static bool post_finished = false;
static void vgt_pci_conf_init(PCIDevice *pci_dev)
{
    printf("vendor id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x00));
    vgt_pci_conf_init_from_host(pci_dev, 0x00, 2); /* vendor id */
    printf("vendor id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x00));
    printf("device id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x02));
    vgt_pci_conf_init_from_host(pci_dev, 0x02, 2); /* device id */
    printf("device id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x02));
    vgt_pci_conf_init_from_host(pci_dev, 0x06, 2); /* status */
    vgt_pci_conf_init_from_host(pci_dev, 0x08, 2); /* revision id */
    vgt_pci_conf_init_from_host(pci_dev, 0x34, 1); /* capability */
    vgt_pci_conf_init_from_host(pci_dev, 0x50, 2); /* SNB: processor graphics control register */
    vgt_pci_conf_init_from_host(pci_dev, 0x52, 2); /* processor graphics control register */
}

static void finish_post(PCIDevice *pci_dev)
{
	if (post_finished)
		return;
	JDPRINT("post_finished: false -> true!\n");

	post_finished = true;
	if (vgt_enabled) {
		vgt_pci_conf_init(pci_dev);
	}
}

uint32_t vgt_pci_read(PCIDevice *pci_dev, uint32_t config_addr, int len)
{
	JDPRINT("addr=%x len=%x\n", config_addr, len);

	return pci_default_read_config(pci_dev, config_addr, len);
}

void vgt_pci_write(PCIDevice *pci_dev, uint32_t config_addr, uint32_t val, int len)
{
	/* Qemu needs to know where the access is from: virtual BIOS or guest OS.
	 *
	 * If the access is from SeaBIOS, we act like a traditional i440fx;
	 * Otherwise we act like the physical host bridge.
	 *
	 * This is ugly but currently necessary.
	 */
	if (config_addr == PCI_VENDOR_ID && val == 0xB105DEAD) {
		finish_post(pci_dev);
		return;
	}

	i440fx_write_config(pci_dev, config_addr, val, len);
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

    JDPRINT("vgt_initfn\n");
    d->instance_created = TRUE;

    create_vgt_instance();
    return 0;
}

DeviceState *vgt_vga_init(PCIBus *pci_bus)
{
    PCIDevice *dev;
    PCIBridge *br;
    HostDevice host_dev = {
        .s = 0,
        .b = 0,
        .d = 0x1f,
        .f = 0,
    };
    host_dev_get(&host_dev);

    if (host_dev.vendor_id != PCI_VENDOR_ID_INTEL) {
        host_dev_put(&host_dev);
        fprintf(stderr, " Error, vga-vgt is only supported on Intel GPUs\n");
        return NULL;
    }

    host_dev_put(&host_dev);

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x1f, 0), true,
                                   "vgt-isa");
    if (!dev) {
        fprintf(stderr, "Warning: vga-vgt not available\n");
        return NULL;
    }

    qdev_init_nofail(&dev->qdev);

    pci_config_set_vendor_id(dev->config, host_dev.vendor_id);
    pci_config_set_device_id(dev->config, host_dev.device_id);
    pci_config_set_revision(dev->config, host_dev.revision_id);
    pci_config_set_class(dev->config, PCI_CLASS_BRIDGE_ISA);


    br = PCI_BRIDGE(dev);
    pci_bridge_map_irq(br, "IGD Bridge", pch_map_irq);

    JDPRINT("Create vgt ISA bridge successfully\n");

    //Now, IGD's turn
    host_dev.d = 0x2;
    host_dev_get(&host_dev);
    if (host_dev.vendor_id != PCI_VENDOR_ID_INTEL) {
	    host_dev_put(&host_dev);
	    fprintf(stderr, " Error, vga-vgt is only supported on Intel GPUs\n");
	    return NULL;
    }
    host_dev_put(&host_dev);

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x2, 0), true, "vgt-vga");
    if (!dev) {
        JERROR("Warning: vga-vgt not available\n");
        return NULL;
    }
    JDPRINT("hi\n");
    qdev_init_nofail(&dev->qdev);
    JDPRINT("hi\n");

#if 1 //debug only
    pci_config_set_vendor_id(dev->config, 0xdead);
    pci_config_set_device_id(dev->config, 0xbeaf);
#endif

    JDPRINT("Create vgt VGA successfully\n");
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

#ifdef CONFIG_KVM
    vgt_opregion_init();
#endif
}

static TypeInfo vgt_info = {
    .name          = "vgt-vga",
    .parent        = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(VGTVGAState),
    .class_init    = vgt_class_initfn,
};

static TypeInfo isa_info = {
    .name          = "vgt-isa",
    .parent        = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(VGTVGAState),
};

static void vgt_register_types(void)
{
    type_register_static(&vgt_info);
    type_register_static(&isa_info);
}

type_init(vgt_register_types)
