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

typedef struct vgt_vga_state {
    PCIDevice dev;
    struct VGACommonState state;
    int num_displays;
    XenHostPCIDevice host_dev;
    bool instance_created;
} vgt_vga_state_t;

#define EDID_SIZE 128
#define MAX_INPUT_NUM 3
#define MAX_FILE_NAME_LENGTH 128

/* port definition must align with gvt-g driver */
enum vgt_port {
    PORT_A = 0,
    PORT_B,
    PORT_C,
    PORT_D,
    PORT_E,
    MAX_PORTS
};

typedef struct vgt_monitor_info {
    unsigned char port_type:4;
    unsigned char port_is_dp:4;  /* 0 = HDMI PORT, 1 = DP port, only valid for PORT_B/C/D */
    unsigned char port_override;
    unsigned char edid[EDID_SIZE];
}vgt_monitor_info_t;

/* These are the default values */
int vgt_low_gm_sz = 64; /* in MB */
int vgt_high_gm_sz = 448; /* in MB */
int vgt_fence_sz = 4;
int vgt_primary = 1; /* -1 means "not specified */
const char *vgt_monitor_config_file = NULL;


static inline unsigned int port_info_to_type(unsigned char port_is_dp, int port)
{
    /* port type definition must align with gvt-g driver */
    enum vgt_port_type {
        VGT_CRT = 0,
        VGT_DP_A,
        VGT_DP_B,
        VGT_DP_C,
        VGT_DP_D,
        VGT_HDMI_B,
        VGT_HDMI_C,
        VGT_HDMI_D,
        VGT_PORT_TYPE_MAX
    } ret;

    switch (port) {
        case PORT_A:
            ret = VGT_DP_A;
            break;
        case PORT_B:
            ret = (port_is_dp) ? VGT_DP_B : VGT_HDMI_B;
            break;
        case PORT_C:
            ret = (port_is_dp) ? VGT_DP_C : VGT_HDMI_C;
            break;
        case PORT_D:
            ret = (port_is_dp) ? VGT_DP_D : VGT_HDMI_D;
            break;
	case PORT_E:
            ret = VGT_CRT;
            break;
        default:
            ret = VGT_PORT_TYPE_MAX;
            break;
    }

    return ret;
}

static bool validate_monitor_configs(vgt_monitor_info_t *config)
{
    if (config->port_type >= MAX_PORTS) {
        qemu_log("vGT: %s failed because the invalid port_type input: %d!\n",
            __func__, config->port_type);
        return false;
    }
    if (config->port_override >= MAX_PORTS) {
        qemu_log("vGT: %s failed due to the invalid port_override input: %d!\n",
            __func__, config->port_override);
        return false;
    }
    if (config->edid[126] != 0) {
        qemu_log("vGT: %s failed because there is extended block in EDID! "
            "(EDID[126] is not zero)\n", __func__);
        return false;
    }

    return true;
}

static void config_hvm_monitors(vgt_monitor_info_t *config)
{
    const char *path_prefix = "/sys/kernel/vgt/vm";
    FILE *fp;
    char file_name[MAX_FILE_NAME_LENGTH];
    int ret;

    // override
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/port_override",
        path_prefix, xen_domid, 'A' + config->port_type);
    if ((fp = fopen(file_name, "w")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, file_name, errno);
        return;
    }
    fprintf(fp, "PORT_%c", 'A' + config->port_override);
    if (fclose(fp) != 0) {
        qemu_log("vGT: %s failed to close file: errno = %d\n", __func__, errno);
    }

    // type
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/type",
        path_prefix, xen_domid, 'A' + config->port_type);
    if ((fp = fopen(file_name, "w")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, file_name, errno);
        return;
    }
    fprintf(fp, "%d", port_info_to_type(config->port_is_dp, config->port_type));
    if (fclose(fp) != 0) {
        qemu_log("vGT: %s failed to close file: errno = %d\n", __func__, errno);
    }

    // edid
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/edid",
        path_prefix, xen_domid, 'A' + config->port_type);
    if ((fp = fopen(file_name, "w")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, file_name, errno);
        return;
    }
    ret = fwrite(config->edid, 1, EDID_SIZE, fp);
    if (ret != EDID_SIZE) {
        qemu_log("vGT: %s failed to write EDID with returned size %d: "
            "errno = %d\n", __func__, ret, errno);
    }
    if (fclose(fp) != 0) {
        qemu_log("vGT: %s failed to close file: errno = %d\n", __func__, errno);
    }

    // flush result to port structure
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/connection",
        path_prefix, xen_domid, 'A' + config->port_type);
    if ((fp = fopen(file_name, "w")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, file_name, errno);
        return;
    }
    fprintf(fp, "flush");
    if (fclose(fp) != 0) {
        qemu_log("vGT: %s failed to close file: errno = %d\n", __func__, errno);
    }
}

#define CTOI(chr) \
    (chr >= '0' && chr <= '9' ? chr - '0' : \
    (chr >= 'a' && chr <= 'f' ? chr - 'a' + 10 :\
    (chr >= 'A' && chr <= 'F' ? chr - 'A' + 10 : -1)))

static int get_byte_from_txt_file(FILE *file, const char *file_name)
{
    int i;
    int val[2];

    for (i = 0; i < 2; ++ i) {
        do {
            unsigned char buf;
            if (fread(&buf, 1, 1, file) != 1) {
                qemu_log("vGT: %s failed to get byte from text file %s with errno: %d!\n",
                    __func__, file_name, errno);
                return -1;
            }

            if (buf == '#') {
                // ignore comments
                int ret;
                while (((ret = fread(&buf, 1, 1, file)) == 1) && (buf != '\n')) ;
                if (ret != 1) {
                    qemu_log("vGT: %s failed to proceed after comment string "
                            "from text file %s with errno: %d!\n",
                            __func__, file_name, errno);
                    return -1;
                }
            }

            val[i] = CTOI(buf);
        } while (val[i] == -1);
    }

    return ((val[0] << 4) | val[1]);
}

static int get_config_header(unsigned char *buf, FILE *file, const char *file_name)
{
    int ret;
    unsigned char chr;

    if (fread(&chr, 1, 1, file) != 1) {
        qemu_log("vGT: %s failed to get byte from text file %s with errno: %d!\n",
            __func__, file_name, errno);
        return -1;
    }

    if (chr == '#') {
        // it is text format input.
        while (((ret = fread(&chr, 1, 1, file)) == 1) && (chr != '\n')) ;
        if (ret != 1) {
            qemu_log("vGT: %s failed to proceed after comment string "
                "from file %s with errno: %d!\n",
                __func__, file_name, errno);
            return -1;
        }
        ret = get_byte_from_txt_file(file, file_name);
        buf[0] = 1;
        buf[1] = (ret & 0xf);
    } else {
        if ((ret = fread(&buf[0], 1, 2, file)) != 2) {
            qemu_log("vGT: %s failed to read file %s! "
                "Expect to read %d bytes but only got %d bytes! errno: %d\n",
                __func__, file_name, 2, ret, errno);
            return -1;
        }

        if (buf[0] != 0) {
            // it is text format input.
            buf[1] -= '0';
        }
    }

    return 0;
}

static void config_vgt_guest_monitors(void)
{
    FILE *monitor_config_f;
    unsigned char buf[4];
    vgt_monitor_info_t monitor_configs[MAX_INPUT_NUM];
    bool text_mode;
    int input_items;
    int ret, i;

    if (!vgt_monitor_config_file) {
        return;
    }

    if ((monitor_config_f = fopen(vgt_monitor_config_file, "r")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, vgt_monitor_config_file, errno);
        return;
    }

    if (get_config_header(buf, monitor_config_f, vgt_monitor_config_file) != 0) {
        goto finish_config;
    }

    text_mode = !!buf[0];
    input_items = buf[1];

    if (input_items <= 0 || input_items > MAX_INPUT_NUM) {
        qemu_log("vGT: %s, Out of range input of the number of items! "
            "Should be [1 - 3] but input is %d\n", __func__, input_items);
        goto finish_config;
    }

    if (text_mode) {
        unsigned int total = sizeof(vgt_monitor_info_t) * input_items;
        unsigned char *p = (unsigned char *)monitor_configs;
        for (i = 0; i < total; ++i, ++p) {
            unsigned int val = get_byte_from_txt_file(monitor_config_f,
                vgt_monitor_config_file);
            if (val == -1) {
                break;
            } else {
                *p = val;
            }
        }
        if (i < total) {
            goto finish_config;
        }
    } else {
        unsigned int total = sizeof(vgt_monitor_info_t) * input_items;
        ret = fread(monitor_configs, sizeof(vgt_monitor_info_t), input_items,
                    monitor_config_f);
        if (ret != total) {
            qemu_log("vGT: %s failed to read file %s! "
                "Expect to read %d bytes but only got %d bytes! errno: %d\n",
                 __func__, vgt_monitor_config_file, total, ret, errno);
            goto finish_config;
        }
    }

    for (i = 0; i < input_items; ++ i) {
        if (validate_monitor_configs(&monitor_configs[i]) == false) {
            qemu_log("vGT: %s the monitor config[%d] input from %s is not valid!\n",
                __func__, i, vgt_monitor_config_file);
            goto finish_config;
        }
    }
    for (i = 0; i < input_items; ++ i) {
        config_hvm_monitors(&monitor_configs[i]);
    }

finish_config:
    if (fclose(monitor_config_f) != 0) {
        qemu_log("vGT: %s failed to close file %s: errno = %d\n", __func__,
            vgt_monitor_config_file, errno);
    }
    return;
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
    if (!err && fprintf(vgt_file, "%d,%u,%u,%u,%d\n", xen_domid,
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

    config_vgt_guest_monitors();
}

/*
 *  Inform vGT driver to close a vGT instance
 */
static void destroy_vgt_instance(void)
{
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;

    qemu_log("vGT: %s: domid=%d\n", __func__, xen_domid);

    if ((vgt_file = fopen(path, "w")) == NULL) {
        fprintf(stdout, "vGT: open %s failed\n", path);
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

void vgt_bridge_pci_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len)
{
#if 0
    vgt_vga_state_t *o = DO_UPCAST(vgt_vga_state_t, dev, dev);
#endif
    assert(dev->devfn == 0x00);

    XEN_PT_LOG(dev, "vGT Config Write: addr=%x len=%x val=%x\n", addr, len, val);

    switch (addr) {
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
    XenHostPCIDevice host_dev;

    if (len > 4) {
        XEN_PT_LOG(dev, "WARNIGN: length %x too large for config addr %x, ignore init\n",
                len, addr);
        return;
    }

    /* FIXME: need a better scheme to grab the root complex. This
     * only for a single VM scenario.
    */
    if ( xen_host_pci_device_get(&host_dev, 0, 0, 0, 0) < 0) {
        fprintf(stderr, " Error, failed to get host PCI device\n");
    }

    xen_host_pci_get_block(&host_dev, addr, dev->config + addr, len);
    xen_host_pci_device_put(&host_dev);
}

static void vgt_host_bridge_cap_init(PCIDevice *dev)
{
    assert(dev->devfn == 0x00);
    uint8_t cap_ptr = 0;
    XenHostPCIDevice host_dev;

    xen_host_pci_device_get(&host_dev, 0, 0, 0, 0);
    xen_host_pci_get_byte(&host_dev, 0x34, &cap_ptr);

    while (cap_ptr !=0) {
        vgt_bridge_pci_conf_init_from_host(dev, cap_ptr, 4); /* capability */
        vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 4, 4); /* capability */
        vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 8, 4); /* capability */
        vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 12, 4); /* capability */
        //XEN_PT_LOG(pci_dev, "Add vgt host bridge capability: offset=0x%x, cap=0x%x\n", cap_ptr,
        //    pt_pci_host_read(0, PCI_SLOT(pci_dev->devfn), 0, cap_ptr, 1) & 0xFF );
        xen_host_pci_get_byte(&host_dev, cap_ptr + 1, &cap_ptr);
    }

    xen_host_pci_device_put(&host_dev);
}


void vgt_bridge_pci_conf_init(PCIDevice *pci_dev)
{
    printf("vgt_bridge_pci_conf_init\n");
    printf("vendor id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x00));
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x00, 2); /* vendor id */
    printf("vendor id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x00));
    printf("device id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x02));
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x02, 2); /* device id */
    printf("device id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x02));
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
    vgt_vga_state_t *d = DO_UPCAST(vgt_vga_state_t, dev, dev);

    if (d->instance_created) {
        destroy_vgt_instance();
    }
}

static int vgt_initfn(PCIDevice *dev)
{
    vgt_vga_state_t *d = DO_UPCAST(vgt_vga_state_t, dev, dev);

    printf("vgt_initfn\n");
    d->instance_created = FALSE;

    create_vgt_instance();
    return 0;
}

DeviceState *xengt_vga_init(PCIBus *pci_bus)
{
    PCIDevice *dev;
    XenHostPCIDevice host_dev;
    PCIBridge *br;

    if (xen_host_pci_device_get(&host_dev, 0, 0, 0x1f, 0) < 0) {
        fprintf(stderr, " Error, failed to get host PCI device\n");
        return NULL;
    }

    if (host_dev.vendor_id != 0x8086) {
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
    pci_config_set_class(dev->config, host_dev.class_code);
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
    .instance_size = sizeof(vgt_vga_state_t),
    .class_init    = vgt_class_initfn,
};

static TypeInfo isa_info = {
    .name          = "xengt-isa",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(vgt_vga_state_t),
};

static void vgt_register_types(void)
{
    type_register_static(&vgt_info);
    type_register_static(&isa_info);
}

type_init(vgt_register_types)
