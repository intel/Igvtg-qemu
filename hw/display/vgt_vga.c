/*
 * QEMU KVMGT VGA support
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
#include "exec/ram_addr.h"

#define DEBUG_VGT

#ifdef DEBUG_VGT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "vgt: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define FUNC_ENTER DPRINTF("[%s] Enter\n", __FUNCTION__)
#define FUNC_EXIT DPRINTF("[%s] Exit\n", __FUNCTION__)

#define BITS_TO_BYTES(bits) (((bits) + BITS_PER_BYTE - 1)/BITS_PER_BYTE)

typedef struct VGTHostDevice {
    PCIHostDeviceAddress addr;
    int config_fd;
} VGTHostDevice;

struct VGTVGAState;

typedef struct VGTVMState {
    struct VGACommonState vga;
    struct VGTVGAState* parent;
} VGTVMState;

typedef struct VGTVGAState {
    PCIDevice dev;
    VGTVMState state;
    int num_displays;
    VGTHostDevice host_dev;
    bool instance_created;
    int domid;
    /* for KVMGT migration support*/
    bool vgt_paused;
    MemoryListener vgt_memory_listener;
} VGTVGAState;

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

typedef struct VGTMonitorInfo {
    unsigned char port_type:4;
    unsigned char port_is_dp:4;  /* 0 = HDMI PORT, 1 = DP port, only valid for PORT_B/C/D */
    unsigned char port_override;
    unsigned char edid[EDID_SIZE];
}VGTMonitorInfo;

/* These are the default values */
int vgt_low_gm_sz = 64; /* in MB */
int vgt_high_gm_sz = 448; /* in MB */
int vgt_fence_sz = 4;
int vgt_primary = 1; /* -1 means "not specified */
int vgt_cap = 0;
const char *vgt_monitor_config_file = NULL;

int guest_domid = 0;
int get_guest_domid(void)
{
    return guest_domid;
}

static void cpu_update_state(void *opaque, int running, RunState state);

static void vgt_log_start(MemoryListener *listener,
                          MemoryRegionSection *section);
static void vgt_log_stop(MemoryListener *listener,
                          MemoryRegionSection *section);
static void vgt_log_sync(MemoryListener *listener,
                         MemoryRegionSection *section);
static void vgt_log_global_start(struct MemoryListener *listener);
static void vgt_log_global_stop(struct MemoryListener *listener);

int dirty_bitmap_read(uint8_t* bitmap, unsigned long off, unsigned long count);
static int vgt_host_pci_cfg_get(VGTHostDevice *host_dev,
                                void *data, int len, uint32_t addr);

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

static bool validate_monitor_configs(VGTMonitorInfo *config)
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

static void config_hvm_monitors(VGTVGAState *vdev, VGTMonitorInfo *config)
{
    const char *path_prefix = "/sys/kernel/vgt/vm";
    FILE *fp;
    char file_name[MAX_FILE_NAME_LENGTH];
    int ret;
    int domid = vdev->domid;

    // override
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/port_override",
        path_prefix, domid, 'A' + config->port_type);
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
        path_prefix, domid, 'A' + config->port_type);
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
        path_prefix, domid, 'A' + config->port_type);
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
        path_prefix, domid, 'A' + config->port_type);
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

static void config_vgt_guest_monitors(VGTVGAState *vdev)
{
    FILE *monitor_config_f;
    unsigned char buf[4];
    VGTMonitorInfo monitor_configs[MAX_INPUT_NUM];
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
        unsigned int total = sizeof(VGTMonitorInfo) * input_items;
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
        unsigned int total = sizeof(VGTMonitorInfo) * input_items;
        ret = fread(monitor_configs, sizeof(VGTMonitorInfo), input_items,
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
        config_hvm_monitors(vdev, &monitor_configs[i]);
    }

finish_config:
    if (fclose(monitor_config_f) != 0) {
        qemu_log("vGT: %s failed to close file %s: errno = %d\n", __func__,
            vgt_monitor_config_file, errno);
    }
    return;
}

#ifdef CONFIG_KVM
MemoryRegion opregion_mr;
uint32_t vgt_kvm_opregion_addr;
static bool post_finished = false;

static void finish_post(PCIDevice *pci_dev)
{
    if (post_finished) {
        return;
    }
    fprintf(stderr, "post_finished: false -> true!\n");

    post_finished = true;
    if (vgt_vga_enabled) {
        vgt_bridge_pci_conf_init(pci_dev);
    }
}

static void vgt_opregion_prepare_mem(void)
{
    memory_region_init_ram(&opregion_mr, NULL, "opregion.ram",
                           VGT_OPREGION_SIZE, NULL);
    vmstate_register_ram_global(&opregion_mr);
    memory_region_add_subregion(get_system_memory(),
                                vgt_kvm_opregion_addr, &opregion_mr);
}

static void vgt_opregion_init(void)
{
    KVMState *s = kvm_state;
    int ret;

    vgt_opregion_prepare_mem();
    ret = kvm_vm_ioctl(s, KVM_VGT_SET_OPREGION, &vgt_kvm_opregion_addr);
    if (ret < 0) {
        DPRINTF("kvm_vm_ioctl KVM_VGT_SET_OPREGION failed: ret = %d\n", ret);
        exit(1);
    }
}

void vgt_kvm_set_opregion_addr(uint32_t addr)
{
    DPRINTF("opregion:%x\n", addr);
    vgt_kvm_opregion_addr = addr;
}
#endif

void vgt_bridge_pci_write(PCIDevice *dev,
                          uint32_t address, uint32_t val, int len)
{
    assert(dev->devfn == 0x00);

#ifdef CONFIG_KVM
    /* QEMU needs to know where the access is from: virtual BIOS or guest OS.
     *
     * If the access is from SeaBIOS, we act like a traditional i440fx;
     * Otherwise we act like the physical host bridge.
     *
     * This is ugly but currently necessary.
     */
    if (kvm_enabled() && address == PCI_VENDOR_ID && val == 0xB105DEAD) {
        finish_post(dev);
        return;
    }
#endif

    i440fx_write_config(dev, address, val, len);
}

/*
 *  Inform vGT driver to create a vGT instance
 */
static void create_vgt_instance(VGTVGAState *vdev)
{
    /* FIXME: this should be substituded as a environment variable */
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;
    int domid = vdev->domid;

    qemu_log("vGT: %s: domid=%d, low_gm_sz=%dMB, high_gm_sz=%dMB, "
        "fence_sz=%d, vgt_primary=%d, vgt_cap=%d\n", __func__, domid,
        vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary,
		vgt_cap);
    if (vgt_low_gm_sz <= 0 || vgt_high_gm_sz <=0 ||
               vgt_cap < 0 || vgt_cap > 100 ||
		vgt_primary < -1 || vgt_primary > 1 ||
        vgt_fence_sz <=0) {
        qemu_log("vGT: %s failed: invalid parameters!\n", __func__);
        abort();
    }
#ifdef CONFIG_KVM
    if (kvm_enabled()) {
        vgt_opregion_init();
    }
#endif

    if ((vgt_file = fopen(path, "w")) == NULL) {
        err = errno;
        qemu_log("vGT: open %s failed\n", path);
    }
    /* The format of the string is:
     * domid,aperture_size,gm_size,fence_size. This means we want the vgt
     * driver to create a vgt instanc for Domain domid with the required
     * parameters. NOTE: aperture_size and gm_size are in MB.
     */
    if (!err && fprintf(vgt_file, "%d,%u,%u,%u,%d,%u\n", domid,
        vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary,
		vgt_cap) < 0) {
        err = errno;
    }

    if (!err && fclose(vgt_file) != 0) {
        err = errno;
    }

    if (err) {
        qemu_log("vGT: %s failed: errno=%d\n", __func__, err);
        exit(-1);
    }

    config_vgt_guest_monitors(vdev);
    vdev->instance_created = true;
    vdev->vgt_paused = false;
}

/*
 *  Inform vGT driver to close a vGT instance
 */
static void destroy_vgt_instance(int domid)
{
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;

#ifdef CONFIG_KVM
    if (kvm_enabled()) {
        memory_region_del_subregion(get_system_memory(), &opregion_mr);
        object_unref(OBJECT(&opregion_mr));
    }
#endif

    if ((vgt_file = fopen(path, "w")) == NULL) {
        error_report("vgt: error: open %s failed", path);
        err = errno;
    }

    /* -domid means we want the vgt driver to free the vgt instance
     * of Domain domid.
     * */
    if (!err && fprintf(vgt_file, "%d\n", -domid) < 0) {
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

static int vgt_host_device_get(VGTHostDevice *dev)
{
    char name[PATH_MAX];

    snprintf(name, sizeof(name), "/sys/bus/pci/devices/%04x:%02x:%02x.%x/config",
             dev->addr.domain, dev->addr.bus, dev->addr.slot, dev->addr.function);
    dev->config_fd = open(name, O_RDONLY);
    if (dev->config_fd == -1) {
        error_report("vgt:open failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void vgt_host_device_put(VGTHostDevice *dev)
{
    if (dev->config_fd >= 0) {
        close(dev->config_fd);
        dev->config_fd = -1;
    }
}

static int vgt_host_pci_cfg_get(VGTHostDevice *host_dev,
                                void *data, int len, uint32_t addr)
{
    int ret;

    vgt_host_device_get(host_dev);
    ret = pread(host_dev->config_fd, data, len, addr);
    if (ret < len) {
        ret = ret < 0 ? -errno : -EFAULT;
        error_report("failed to read device config space: %m");
        goto out;
    }

out:
    vgt_host_device_put(host_dev);
    return ret;
}

static void vgt_host_bridge_cap_init(PCIDevice *dev, VGTHostDevice *host_dev)
{
    assert(dev->devfn == 0x00);
    uint8_t cap_ptr = 0;

    vgt_host_pci_cfg_get(host_dev, &cap_ptr, 1, PCI_CAPABILITY_LIST);
    while (cap_ptr !=0) {
        vgt_host_pci_cfg_get(host_dev, dev->config + cap_ptr, 4, cap_ptr);
        vgt_host_pci_cfg_get(host_dev, dev->config + cap_ptr + 4, 4,
                             cap_ptr + 4);
        vgt_host_pci_cfg_get(host_dev, dev->config + cap_ptr + 8, 4,
                             cap_ptr + 8);
        vgt_host_pci_cfg_get(host_dev, dev->config + cap_ptr + 12, 4,
                             cap_ptr + 12);
        vgt_host_pci_cfg_get(host_dev, &cap_ptr, 1, cap_ptr + 1);
    }
}

static void vgt_host_dev_init(PCIDevice *pdev, VGTHostDevice *host_dev)
{
    assert(pdev != NULL && host_dev != NULL);

    host_dev->addr.domain = 0;
    host_dev->addr.bus = pci_bus_num(pdev->bus);
    host_dev->addr.slot = PCI_SLOT(pdev->devfn);
    host_dev->addr.function = PCI_FUNC(pdev->devfn);
}

void vgt_bridge_pci_conf_init(PCIDevice *pdev)
{
    printf("vgt_bridge_pci_conf_init\n");
    VGTHostDevice host_dev;

    vgt_host_dev_init(pdev, &host_dev);

    vgt_host_pci_cfg_get(&host_dev, pdev->config, 2, 0x00);
    printf("vendor id: %x\n", *(uint16_t *)((char *)pdev->config + 0x00));

    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x02, 2, 0x02);
    printf("device id: %x\n", *(uint16_t *)((char *)pdev->config + 0x02));
    /* status */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x06, 2, 0x06);
    /* revision id */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x08, 2, 0x08);
    /* capability */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x34, 1, 0x34);
    vgt_host_bridge_cap_init(pdev, &host_dev);

    /* SNB: processor graphics control register */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x50, 2, 0x50);
    /* processor graphics control register */
    vgt_host_pci_cfg_get(&host_dev, pdev->config + 0x52, 2, 0x52);
}

static void vgt_reset(DeviceState *dev)
{
    PCIDevice *pdev = DO_UPCAST(PCIDevice, qdev, dev);
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, pdev);

    if (d->instance_created) {
        destroy_vgt_instance(d->domid);
    }

    create_vgt_instance(d);
}

static void vgt_cleanupfn(PCIDevice *dev)
{
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, dev);

    if (d->instance_created) {
        destroy_vgt_instance(d->domid);
    }
}

static int vgt_get_domid(void)
{
    int domid = 0;

#ifdef CONFIG_KVM
    if (kvm_enabled()) {
        KVMState *s = kvm_state;

        domid = kvm_vm_ioctl(s, KVM_GET_DOMID, 0);
        if (domid <= 0) {
            error_report("vgt: get KVM_GET_DOMID failed: %d", domid);
            exit(-1);
        }
        DPRINTF("kvm_domid is %d\n", domid);
    }
#endif

    if (xen_enabled()) {
        domid = xen_domid;
    }
    assert(domid > 0);
    guest_domid = domid;

    return domid;
}

static int vgt_initfn(PCIDevice *dev)
{
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, dev);

    DPRINTF("vgt_initfn\n");
    vgt_host_dev_init(dev, &d->host_dev);
    d->domid = vgt_get_domid();
    d->state.parent = d;
    d->vgt_paused = false;

    d->vgt_memory_listener = (MemoryListener) {
        .log_start = vgt_log_start,
        .log_stop = vgt_log_stop,
        .log_sync = vgt_log_sync,
        .log_global_start = vgt_log_global_start,
        .log_global_stop = vgt_log_global_stop,
    };

    memory_listener_register(&d->vgt_memory_listener, &address_space_memory);

    qemu_add_vm_change_state_handler(cpu_update_state, d);

    return 0;
}

DeviceState *vgt_vga_init(PCIBus *pci_bus)
{
    PCIDevice *dev;
    PCIBridge *br;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision_id;
    uint16_t class_dev;
    VGTHostDevice host_dev = {
        .addr.domain = 0,
        .addr.bus = 0,
        .addr.slot = 0x1f,
        .addr.function = 0,
    };

    vgt_host_pci_cfg_get(&host_dev, &vendor_id, sizeof(vendor_id),
                         PCI_VENDOR_ID);
    vgt_host_pci_cfg_get(&host_dev, &device_id, sizeof(device_id),
                         PCI_DEVICE_ID);
    vgt_host_pci_cfg_get(&host_dev, &revision_id, sizeof(revision_id),
                         PCI_REVISION_ID);
    vgt_host_pci_cfg_get(&host_dev, &class_dev, sizeof(class_dev),
                         PCI_CLASS_DEVICE);
    DPRINTF("vendor: 0x%hx, device: 0x%hx, revision: 0x%hhx\n",
            vendor_id, device_id, revision_id);

    if (vendor_id != PCI_VENDOR_ID_INTEL) {
        error_report("vgt: error: vgt-vga is only supported on Intel GPUs");
        return NULL;
    }

    dev = pci_create_multifunction(pci_bus, PCI_DEVFN(0x1f, 0), true,
                                   "vgt-isa");
    if (!dev) {
        error_report("vgt: error: vgt-isa not available");
        return NULL;
    }

    qdev_init_nofail(&dev->qdev);

    pci_config_set_vendor_id(dev->config, vendor_id);
    pci_config_set_device_id(dev->config, device_id);
    pci_config_set_revision(dev->config, revision_id);
    pci_config_set_class(dev->config, class_dev);
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

/*
 * Keep this function to profile logdirty APIs
 */
static inline unsigned long rdtscll(void)
{
    unsigned long val;
    asm volatile("rdtsc" : "=A" (val));
    return val;
}

/*
 * is_read = ture:  Read vGPU state and provide to QEMU file
 * is_read = false: Get QEMU file and write migrating source data to vGPU state
 */
static void read_write_snapshot(QEMUFile* f, VGTVGAState* d, bool is_read)
{
    char file_name[PATH_MAX] = {0};
    FILE* fp = NULL;
    struct stat st;
    int sz;
    uint8_t* buf = NULL;
    int count = 0;

    FUNC_ENTER;

    snprintf(file_name, PATH_MAX, "/sys/kernel/vgt/vm%d/state", d->domid);

    if ((fp=fopen(file_name, "r+")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
                __func__, file_name, errno);
        goto EXIT;
    }

    fstat(fileno(fp), &st);
    sz = st.st_size;

    if (sz <= 0) {
        qemu_log("vGT: failed to achieve file size. file name=%s \n",
               file_name);
        goto EXIT;
    }


    if ((buf=g_malloc(sz))==NULL) {
        qemu_log("vGT: %s failed to allocate memory size %d! errno = %d\n",
                __func__, sz, errno);
        goto EXIT;
    }

    DPRINTF("Allocate %d size of buffer for snapshot\n", sz);

    if (is_read) {
        count = fread(buf, 1, sz, fp);
        qemu_put_buffer(f, buf, sz);
    }
    else {
        qemu_get_buffer(f, buf, sz);
        count = fwrite(buf, 1, sz, fp);
    }

    if (count != sz) {
        qemu_log("vGT: read/write snapshot file size is differ %d:%d \n",
                count, sz);
    }

    DPRINTF("[%s] %d size of buffer for snapshot\n", is_read? "READ":"WRITE",
            count);

EXIT:
    if (buf)
        g_free(buf);
    if (fp)
        fclose(fp);
    return;
}

/*
 * Pause vGPU scheduling from GVT-g scheduling
 */
static void vgt_pause(int domid)
{
    int ret;
    /* pause vGPU scheduling */
    char cmd[PATH_MAX] = {0};
    /* switch foreground to Dom0 first */
    snprintf(cmd, PATH_MAX, "echo 0 > /sys/kernel/vgt/control/foreground_vm");
    ret = system(cmd);
    if (ret < 0) {
        DPRINTF("Execute commands failed. \n");
    }

    memset(cmd, 0, PATH_MAX);
    /* remove DomID from vGPU scheduling */
    snprintf(cmd, PATH_MAX, "echo 0 > /sys/kernel/vgt/vm%d/start", domid);
    ret = system(cmd);
    if (ret < 0) {
        DPRINTF("Execute commands failed. \n");
    }
}

/*
 * Add back vGPU to GVT-g scheduling
 */
static void vgt_resume(int domid)
{
    int ret;
    /* resume vGPU scheduling */
    char cmd[PATH_MAX] = {0};
    snprintf(cmd, PATH_MAX, "echo 1 > /sys/kernel/vgt/vm%d/start", domid);
    ret = system(cmd);
    if (ret < 0) {
        DPRINTF("Execute commands failed. \n");
    }
}

/*
 * QEMU callback function whenever CPU state changed to:
 * pause/Migrate/resume/running
 */
static void cpu_update_state(void *opaque, int running, RunState state)
{
    VGTVGAState *d = (VGTVGAState*) opaque;

    FUNC_ENTER;
    if (state == RUN_STATE_FINISH_MIGRATE) {
        /* pause vGPU scheduling */
        vgt_pause(d->domid);
        d->vgt_paused = true;
    }
    FUNC_EXIT;
}

/*
 * bitmap:  output of dirty_bitmap status
 * off:  read /sys/kernel/vgt/vm#/dirty_bitmap from offset in bytes
 * count: read bytes
 */
int dirty_bitmap_read(uint8_t* bitmap, unsigned long off, unsigned long count)
{
    char file_name[PATH_MAX] = {0};
    int domid = vgt_get_domid();
    int fd = -1;
    int sz = 1<<TARGET_PAGE_BITS; /* one page*/
    char* buf = NULL;
    unsigned long total=0;

    snprintf(file_name, PATH_MAX, "/sys/kernel/vgt/vm%d/dirty_bitmap", domid);

    /* must use low level open() instead of fopen() */
    if ((fd=open(file_name, O_RDWR)) == -1) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
                __func__, file_name, errno);
        goto EXIT;
    }

    if (count < sz)
        sz = count;

    buf = g_malloc(sz);

    /*STEP1: Set to all dirty before achieve real GPU bitmap */
    memset(buf, 0xFF, sz);

    if (lseek(fd, off, SEEK_SET) == -1) {
        DPRINTF("Seek to 0x%lx failed. \n", off);
        goto EXIT;
    }

    total = 0;
    while(1){
        int remains = (count - total) > sz ? sz: count -total;
        int n_written = write(fd, buf, remains);
        if (n_written < 0) {
            DPRINTF("Write dirty_bitmap failed.\n");
            total = 0;
            goto EXIT;
        }

        total += n_written;
        if (n_written < remains || total >= count )
            break;
        
    }

    DPRINTF("WRITE 0x%lx size of dirty_bitmap from offset=0x%lx."
           " Actual write 0x%lx \n", count, off, total);
    if (total <= 0) 
        goto EXIT;

    /*STEP2: Read back all dirty status*/
    total = 0;
    if (lseek(fd, off, SEEK_SET) == -1) {
        DPRINTF("Seek to 0x%lx failed. \n", off);
        goto EXIT;
    }

    while(1){
        int remains = (count - total) > sz ? sz: count -total;
        int n_read = read(fd, buf, remains);
        if (n_read < 0) {
            DPRINTF("Read dirty_bitmap failed. \n");
            total = 0;
            goto EXIT;
        }

        memcpy(bitmap + total, buf, n_read);
        total += n_read;
        if (n_read < remains || total >= count )
            break;
    }

    DPRINTF("READ 0x%lx size of dirty_bitmap from offset=0x%lx."
            " Actual get 0x%lx \n", count, off, total);

EXIT:
    if (buf)
        g_free(buf);
    if (fd != -1)
        close(fd);
    return total;
}

static void vgt_sync_dirty_bitmap(VGTVGAState *d, uint8_t *ram_bitmap, 
        unsigned long ram_bitmap_size, /* bitmap size in bytes */
        unsigned long start_addr, 
        unsigned long nr_pages)
{
    unsigned long bit_start = start_addr >> TARGET_PAGE_BITS;
    int bit_offset = bit_start % BITS_PER_BYTE;
    int n = 0;

    FUNC_ENTER;

    n = dirty_bitmap_read(ram_bitmap, bit_start / BITS_PER_BYTE, 
            BITS_TO_BYTES(nr_pages + (bit_start % BITS_PER_BYTE)));
    memset(ram_bitmap + n, 0, ram_bitmap_size - n);

    if (n > 0 && bit_offset) {
        /* bit_start is not BTYES aligned.*/
        char* dst = (char*) ram_bitmap;
        char* src = dst;
        int i;

        DPRINTF("Hit non-bytes aligned bit operation. Shift bit: %d \n",
                bit_offset);
        for(i=0; i< nr_pages / BITS_PER_BYTE; i++) {
            char b;
            b = src[i];
            b >>= bit_offset;
            b &= (src[i + 1] << (BITS_PER_BYTE - bit_offset));
            dst[i] = b;
        }
        /* clear last bytes */
        dst[i+1] = 0;
    }

    return;
}

/*
 * Qemu callback function whenever log dirty required
 */
static void vgt_log_sync(MemoryListener *listener,
        MemoryRegionSection *section)
{
    VGTVGAState *d = container_of(listener, 
            struct VGTVGAState, vgt_memory_listener);

    if (d->vgt_paused) {
        hwaddr start_addr = section->offset_within_address_space;
        ram_addr_t size = int128_get64(section->size);
        unsigned long nr_pages = size >> TARGET_PAGE_BITS;	
        /* allocate additional 8 bytes in case (start_addr>>12)
         * is not bytes aligned.*/
        unsigned long bitmap_size =
            (BITS_TO_LONGS(nr_pages) + 1)*sizeof(unsigned long);
        unsigned long *bitmap = g_malloc(bitmap_size);

        DPRINTF("[%s] MemSection HWADDR 0x%lx size 0x%lx bitmap_size=0x%lx \n",
               __FUNCTION__, start_addr, size, bitmap_size);

        vgt_sync_dirty_bitmap(d,
                (uint8_t*)bitmap,
                bitmap_size,
                start_addr,
                nr_pages);

        cpu_physical_memory_set_dirty_lebitmap(bitmap, start_addr, nr_pages);

        g_free(bitmap);
    }
}

static void vgt_log_start(MemoryListener *listener,
        MemoryRegionSection *section)
{
}

static void vgt_log_stop(MemoryListener *listener,
        MemoryRegionSection *section)
{
}

static void vgt_log_global_start(struct MemoryListener *listener)
{
    /* currently not used. keep for furture extension */
}

static void vgt_log_global_stop(struct MemoryListener *listener)
{
    /* currently not used. keep for furture extension */
}

static void put_snapshot(QEMUFile *f, void *pv, size_t size)
{
    VGTVGAState *d = ((VGTVMState*) pv)->parent;

    FUNC_ENTER;

    /* pause vGPU scheduling if not */
    if (!d->vgt_paused) {
        vgt_pause(d->domid);
        d->vgt_paused = true;
    }

    /* vgt device log dirty pages in xen */
    if (xen_enabled()) {
        ram_addr_t start_pfn, end_pfn;
        ram_addr_t nb_pages = last_ram_offset() >> TARGET_PAGE_BITS;
        unsigned long bitmap_size =
            BITS_TO_LONGS(nb_pages) * sizeof(unsigned long);
        unsigned long *bitmap = g_malloc(bitmap_size);
        start_pfn = 0;

        vgt_sync_dirty_bitmap(d,
            (uint8_t*)bitmap,
            bitmap_size,
            start_pfn,
            nb_pages);

        for (start_pfn = find_first_bit(bitmap, nb_pages);
            start_pfn < nb_pages;
            start_pfn = find_next_bit(bitmap, nb_pages, end_pfn + 1)) {
            end_pfn = find_next_zero_bit(bitmap, nb_pages, start_pfn);
            xen_modified_memory(start_pfn << TARGET_PAGE_BITS,
                (end_pfn - start_pfn) * TARGET_PAGE_SIZE);
        }

        g_free(bitmap);
    }

    /* Sending VM: Read snapshot and write to qemu file*/
    read_write_snapshot(f, d, 1);

    d->vgt_paused = false;
}

static int get_snapshot(QEMUFile *f, void *pv, size_t size)
{
    VGTVGAState *d = ((VGTVMState*) pv)->parent;

    FUNC_ENTER;
    /* Receiving VM: Read from qemu file and write to snapshot */
    read_write_snapshot(f, d, 0);

    /* resume vGPU scheduling */
    vgt_resume(d->domid);
    d->vgt_paused = false;
    return 0;
}

/*
 * VMState structure to read/write vGPU state from GVT-g
 */
static VMStateInfo vmstate_info_snapshot = {
    .name = "snapshot state",
    .get  = get_snapshot,
    .put  = put_snapshot,
};

const VMStateDescription vmstate_vgt_internal_common = {
    .name = "vgt-internal",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        {
            .name         = "snapshot",
            .version_id   = 0,
            .field_exists = NULL,
            .size         = 0,   /* ouch */
            .info         = &vmstate_info_snapshot,
            .flags        = VMS_SINGLE,
            .offset       = 0,
        },

        VMSTATE_END_OF_LIST()
    },
};

/*
 * Cleanup to ignore Qemu VGA save/restore here, since we do not use that.
 * Add GVT-g VMState to save/restore GVT-g vGPU state during migration
 */
static const VMStateDescription vmstate_vga_vgt = {
    .name = "vga-vgt",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, VGTVGAState),
        VMSTATE_STRUCT(state, VGTVGAState, 0, vmstate_vgt_internal_common, VGTVMState),
        VMSTATE_END_OF_LIST()
    }
};

static void vgt_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *ic = PCI_DEVICE_CLASS(klass);
    ic->init = vgt_initfn;
    dc->reset = vgt_reset;
    ic->exit = vgt_cleanupfn;
    dc->vmsd = &vmstate_vga_vgt;
}


static TypeInfo igd_info = {
    .name          = "vgt-vga",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VGTVGAState),
    .class_init    = vgt_class_initfn,
};

static TypeInfo pch_info = {
    .name          = "vgt-isa",
    .parent        = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(PCIBridge),
};

static void vgt_register_types(void)
{
    type_register_static(&igd_info);
    type_register_static(&pch_info);
}

type_init(vgt_register_types)
