/*
 * QEMU KVMGT VGA support
 *
 * Copyright (c) Intel
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */
#include <libudev.h>
#include <i915_drm.h>
#include <xf86drm.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

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

//#define DEBUG_VGT

#ifdef DEBUG_VGT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "vgt: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

struct drm_i915_gem_vgtbuffer {
        __u32 vmid;
        __u32 plane_id;
#define I915_VGT_PLANE_PRIMARY 1
#define I915_VGT_PLANE_SPRITE 2
#define I915_VGT_PLANE_CURSOR 3
        __u32 pipe_id;
        __u32 phys_pipe_id;
        __u8  enabled;
        __u8  tiled;
        __u32 bpp;
        __u32 hw_format;
        __u32 drm_format;
        __u32 start;
        __u32 x_pos;
        __u32 y_pos;
        __u32 x_offset;
        __u32 y_offset;
        __u32 size;
        __u32 width;
        __u32 height;
        __u32 stride;
        __u64 user_ptr;
        __u32 user_size;
        __u32 flags;
#define I915_VGTBUFFER_READ_ONLY (1<<0)
#define I915_VGTBUFFER_QUERY_ONLY (1<<1)
#define I915_VGTBUFFER_CHECK_CAPABILITY (1<<2)
#define I915_VGTBUFFER_UNSYNCHRONIZED 0x80000000
        /**
         * Returned handle for the object.
         *
         * Object handles are nonzero.
         */
        __u32 handle;
};

#define DRM_I915_GEM_VGTBUFFER          0x36
#define DRM_IOCTL_I915_GEM_VGTBUFFER    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_VGTBUFFER, struct drm_i915_gem_vgtbuffer)

typedef struct VGTHostDevice {
    PCIHostDeviceAddress addr;
    int config_fd;
} VGTHostDevice;

struct VGTCursor {
    uint8_t *fb_cursor_ptr;
    int fb_cursor_size;
    int width;
    int height;
    int stride;
    int bpp;
    int hot_x;
    int hot_y;
    int pos_x;
    int pos_y;
    int enabled;
    uint32_t handle;
    uint32_t old_handle;
};

struct VGTPrimaryPlane {
    int new_width;
    int new_height;
    int new_stride;
    int new_depth;
    int format;
    int tiled;
    uint8_t *fb_ptr;
    int fb_size;
    int x_pos;
    int y_pos;
    uint32_t handle;
    uint32_t old_handle;
};

typedef struct VGTVGAState {
    PCIDevice dev;
    struct VGACommonState vga;
    int num_displays;
    VGTHostDevice host_dev;
    bool instance_created;
    int domid;
    int invalidated;
    bool enabled;
    int fb_fd;
    struct VGTPrimaryPlane pp;
    struct VGTCursor cs;
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
const char *vgt_monitor_config_file = NULL;

int guest_domid = 0;
int get_guest_domid(void)
{
    return guest_domid;
}

static int vgt_host_pci_cfg_get(VGTHostDevice *host_dev,
                                void *data, int len, uint32_t addr);
static void gem_close(int fd, uint32_t handle);

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
        "fence_sz=%d, vgt_primary=%d\n", __func__, domid,
        vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary);
    if (vgt_low_gm_sz <= 0 || vgt_high_gm_sz <=0 ||
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
    if (!err && fprintf(vgt_file, "%d,%u,%u,%u,%d\n", domid,
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

    config_vgt_guest_monitors(vdev);
    vdev->instance_created = TRUE;
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
        return ret;
    }
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

static void vgtvga_invalidate_display(void *opaque)
{
    VGTVGAState *d = opaque;

    if (!d->enabled) {
        d->vga.hw_ops->invalidate(&d->vga);
        return;
    }

    d->invalidated = 1;
}

static void vgtvga_text_update(void *opaque, console_ch_t *chardata)
{
    VGTVGAState *d = opaque;

    if (d->vga.hw_ops->text_update) {
        d->vga.hw_ops->text_update(&d->vga, chardata);
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

/********** UDEV Part ************/
static struct udev * udev;
static struct udev_monitor * mon;

static int udev_init(void)
{
    int ret;

    udev = udev_new();
    if (!udev) {
        return -1;
    }

    mon = udev_monitor_new_from_netlink(udev, "kernel");
    if (!mon) {
        ret = -2;
        goto release_udev;
    }

    ret = udev_monitor_filter_add_match_subsystem_devtype(mon, "vgt", NULL);
    if (ret < 0) {
        ret = -3;
        goto release_mon;
    }

    ret = udev_monitor_enable_receiving(mon);
    if (ret < 0) {
        ret = -4;
        goto release_mon;
    }

    return 0;

release_mon:
    udev_monitor_unref(mon);
release_udev:
    udev_unref(udev);

    return ret;
}

static void udev_destroy(void)
{
    udev_monitor_unref(mon);
    udev_unref(udev);
}

static int check_vgt_uevent(void)
{
    int ret = 0;
    const char * value;
    struct udev_device * dev;

    dev = udev_monitor_receive_device(mon);
    if (!dev) {
        goto out;
    }

    value = udev_device_get_property_value(dev, "VGT_DISPLAY_READY");
    if (!value || strcmp(value, "1")) {
        goto out;
    }

    value = udev_device_get_property_value(dev, "VMID");
    if (!value || 1 != sscanf(value, "%d", &ret) || ret != get_guest_domid()) {
        goto out;
    }
    ret = 1;

out:
    udev_device_unref(dev);

    return ret;
}

static void gem_write(int fd, uint32_t handle, uint64_t offset,
                      const void *buf, uint64_t length)
{
    struct drm_i915_gem_pwrite gem_pwrite;

    memset(&gem_pwrite, 0, sizeof(gem_pwrite));
    gem_pwrite.handle = handle;
    gem_pwrite.offset = offset;
    gem_pwrite.size = length;
    gem_pwrite.data_ptr = (uint64_t)buf;
    drmIoctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &gem_pwrite);
}

static uint32_t gem_create(int fd, uint64_t size)
{
    struct drm_i915_gem_create create;

    memset(&create, 0, sizeof(create));
    create.handle = 0;
    create.size = size;
    drmIoctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create);
    assert(create.handle);

    return create.handle;
}

static void gem_close(int fd, uint32_t handle)
{
    struct drm_gem_close close_bo;

    assert(handle != 0);
    memset(&close_bo, 0, sizeof(close_bo));
    close_bo.handle = handle;
    drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
}

/* BLT COMMAND */
#define XY_SRC_COPY_BLT_CMD             ((2<<29)|(0x53<<22))
#define XY_SRC_COPY_BLT_WRITE_ALPHA     (1<<21)
#define XY_SRC_COPY_BLT_WRITE_RGB       (1<<20)
#define XY_SRC_COPY_BLT_SRC_TILED       (1<<15)

/* Batch */
#define MI_BATCH_BUFFER_END     (0xA << 23)

/* Noop */
#define MI_NOOP                         0x00

#define I915_TILING_Yf  3
#define I915_TILING_Ys  4

#define XY_FAST_COPY_BLT                                ((2<<29)|(0x42<<22)|0x8)
/* dword 0 */
#define   XY_FAST_COPY_SRC_TILING_LINEAR                (0 << 20)
#define   XY_FAST_COPY_SRC_TILING_X                     (1 << 20)
#define   XY_FAST_COPY_SRC_TILING_Yb_Yf                 (2 << 20)
#define   XY_FAST_COPY_SRC_TILING_Ys                    (3 << 20)
#define   XY_FAST_COPY_SRC_HORIZONTAL_ALIGNMENT(n)      (n << 17)
#define   XY_FAST_COPY_SRC_VERTICAL_ALIGNMENT(n)        (n << 15)
#define   XY_FAST_COPY_DST_TILING_X                     (1 << 13)
#define   XY_FAST_COPY_DST_TILING_Yb_Yf                 (2 << 13)
#define   XY_FAST_COPY_DST_TILING_Ys                    (3 << 13)
#define   XY_FAST_COPY_DST_HORIZONTAL_ALIGNMENT(n)      (n << 10)
#define   XY_FAST_COPY_DST_VERTICAL_ALIGNMENT(n)        (n <<  8)
/* dword 1 */
#define   XY_FAST_COPY_SRC_TILING_Yf                    (1 <<  31)
#define   XY_FAST_COPY_DST_TILING_Yf                    (1 <<  30)
#define   XY_FAST_COPY_COLOR_DEPTH_32                   (3  << 24)

static int is_sandybridge(int devid)
{
	int ret = 0;

	switch (devid) {
	case 0x0102:
	case 0x0112:
	case 0x0122:
	case 0x0106:
	case 0x0116:
	case 0x0126:
	case 0x010A:
		ret = 1;
		break;
	default:
		break;
	}
	return ret;
}

static int is_ivybridge(int devid)
{
	int ret = 0;

	switch (devid) {
	case 0x0156:
	case 0x0166:
	case 0x0152:
	case 0x0162:
	case 0x015a:
	case 0x016a:
		ret = 1;
		break;
	default:
		break;
	}
	return ret;
}

static int is_haswell(int devid)
{
	int ret = 0;

	switch (devid) {
	case 0x0400:
	case 0x0402:
	case 0x0404:
	case 0x0406:
	case 0x0408:
	case 0x040a:
	case 0x0412:
	case 0x0416:
	case 0x041a:
	case 0x0422:
	case 0x0426:
	case 0x042a:
	case 0x0a02:
	case 0x0a06:
	case 0x0a0a:
	case 0x0a12:
	case 0x0a16:
	case 0x0a1a:
	case 0x0a22:
	case 0x0a26:
	case 0x0a2a:
	case 0x0c02:
	case 0x0c04:
	case 0x0c06:
	case 0x0c0a:
	case 0x0c12:
	case 0x0c16:
	case 0x0c1a:
	case 0x0c22:
	case 0x0c26:
	case 0x0c2a:
	case 0x0d12:
	case 0x0d16:
	case 0x0d1a:
	case 0x0d22:
	case 0x0d26:
	case 0x0d2a:
	case 0x0d32:
	case 0x0d36:
	case 0x0d3a:
		ret = 1;
		break;
	default:
		break;
	}
	return ret;
}

static int is_broadwell(int devid)
{
    switch ((devid >> 4) & 0xf) {
    case 0:
    case 1:
    case 2:
        break;
	default:
	    return 0;
	}

    devid &= ~0xf0;

    switch (devid) {
    case 0x1602:
    case 0x1606:
    case 0x160B:
    case 0x160E:
    case 0x160A:
    case 0x160D:
        break;
    default:
        return 0;
	}

	return 1;
}

static int is_skylake(int devid)
{
    switch ((devid >> 4) & 0xf) {
    case 0:
    case 1:
    case 2:
    case 3:
        break;
    default:
        return 0;
    }

    devid &= ~0xf0;

    switch (devid) {
    case 0x1901:
    case 0x1902:
    case 0x1906:
    case 0x190B:
    case 0x190E:
    case 0x190A:
    case 0x190D:
        break;
    default:
        return 0;
    }

    return 1;
}

#define IS_INTEL(dev)       (is_sandybridge(dev) || \
                             is_ivybridge(dev)   || \
                             is_haswell(dev)     || \
                             is_broadwell(dev)   || \
                             is_skylake(dev))
static int device_id;
static bool intel_get_device_id(int fd)
{
    struct drm_i915_getparam gp;
    int devid = 0;

    memset(&gp, 0, sizeof(gp));
    gp.param = I915_PARAM_CHIPSET_ID;
    gp.value = &devid;

    if (ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp))) {
        return false;
    }

    device_id = devid;
    return true;
}

static int intel_gen_version(int fd)
{
    if (!device_id) {
        intel_get_device_id(fd);
    }
    assert(device_id != 0);
    if (is_sandybridge(device_id)) {
        return 6;
    }
    if (is_ivybridge(device_id)) {
        return 7;
    }
    if (is_haswell(device_id)) {
        return 7;
    }
    if (is_broadwell(device_id)) {
        return 8;
    }
    if (is_skylake(device_id)) {
        return 9;
    }

    /*TODO: others should set a concrete value */
    return 5;
}

#define HAS_BLT_RING     (intel_gen_version(fd) >=6 && \
                          intel_gen_version(fd) <=9)

static void
fill_object(struct drm_i915_gem_exec_object2 *obj, uint32_t gem_handle,
            struct drm_i915_gem_relocation_entry *relocs, uint32_t count)
{
    memset(obj, 0, sizeof(*obj));
    obj->handle = gem_handle;
    obj->relocation_count = count;
    obj->relocs_ptr = (uintptr_t)relocs;
}

static void
fill_relocation(struct drm_i915_gem_relocation_entry *reloc,
                uint32_t gem_handle, uint32_t offset, /* in dwords */
                uint32_t read_domains, uint32_t write_domains)
{
    reloc->target_handle = gem_handle;
    reloc->delta = 0;
    reloc->offset = offset * sizeof(uint32_t);
    reloc->presumed_offset = 0;
    reloc->read_domains = read_domains;
    reloc->write_domain = write_domains;
}

static void exec_blit(int fd,
                      struct drm_i915_gem_exec_object2 *objs, uint32_t count,
                      uint32_t batch_len /* in dwords */)
{
     struct drm_i915_gem_execbuffer2 exec;

     exec.buffers_ptr = (uint64_t)objs;
     exec.buffer_count = count;
     exec.batch_start_offset = 0;
     exec.batch_len = batch_len * 4;
     exec.DR1 = exec.DR4 = 0;
     exec.num_cliprects = 0;
     exec.cliprects_ptr = 0;
     exec.flags = HAS_BLT_RING ? I915_EXEC_BLT : 0;
     exec.rsvd1 = 0;
     exec.rsvd2 = 0;

     assert(drmIoctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec) == 0);
}

static uint32_t fast_copy_dword0(unsigned int src_tiling)
{
    uint32_t dword0 = 0;

    dword0 |= XY_FAST_COPY_BLT;

    switch (src_tiling) {
    case I915_TILING_X:
        dword0 |= XY_FAST_COPY_SRC_TILING_X;
        break;
    case I915_TILING_Y:
    case I915_TILING_Yf:
        dword0 |= XY_FAST_COPY_SRC_TILING_Yb_Yf;
        break;
    case I915_TILING_Ys:
        dword0 |= XY_FAST_COPY_SRC_TILING_Ys;
        break;
    case I915_TILING_NONE:
        default:
        break;
    }

    return dword0;
}

static uint32_t fast_copy_dword1(unsigned int src_tiling)
{
    uint32_t dword1 = 0;

    if (src_tiling == I915_TILING_Yf)
        dword1 |= XY_FAST_COPY_SRC_TILING_Yf;

    dword1 |= XY_FAST_COPY_COLOR_DEPTH_32;

    return dword1;
}

static void copy1(int fd, uint32_t dst_handle, uint32_t src_handle,
                 int src_pitch, int src_tiled, int dst_pitch,
                 int width, int height)
{
    uint32_t batch[12];
    struct drm_i915_gem_exec_object2 objs[3];
    struct drm_i915_gem_relocation_entry relocs[2];
    uint32_t batch_handle;
    uint32_t dword0, dword1;
    int i = 0;

    dword0 = fast_copy_dword0(src_tiled);
    dword1 = fast_copy_dword1(src_tiled);

    if (src_tiled) {
        src_pitch /= 4;
    }

    batch[i++] = dword0;
    batch[i++] = dword1 | dst_pitch;
    batch[i++] = 0; /* dst x1,y1 */
    batch[i++] = height << 16 | width; /* dst x2,y2 */
    batch[i++] = 0; /* dst address lower bits */
    batch[i++] = 0; /* dst address upper bits */
    batch[i++] = 0; /* src x1,y1 */
    batch[i++] = src_pitch;
    batch[i++] = 0; /* src address lower bits */
    batch[i++] = 0; /* src address upper bits */
    batch[i++] = MI_BATCH_BUFFER_END;
    batch[i++] = MI_NOOP;

    assert(i == ARRAY_SIZE(batch));

    batch_handle = gem_create(fd, 4096);
    gem_write(fd, batch_handle, 0, batch, sizeof(batch));

    fill_relocation(&relocs[0], dst_handle, 4,
            I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER);
    fill_relocation(&relocs[1], src_handle, 8, I915_GEM_DOMAIN_RENDER, 0);

    fill_object(&objs[0], dst_handle, NULL, 0);
    fill_object(&objs[1], src_handle, NULL, 0);
    fill_object(&objs[2], batch_handle, relocs, 2);

    exec_blit(fd, objs, 3, ARRAY_SIZE(batch));

    gem_close(fd, batch_handle);
}

static void copy(VGTVGAState *d, uint32_t dst, uint32_t src,
                 int src_pitch, int src_tiled, int dst_pitch,
                 int width, int height, int bpp)
{
    uint32_t batch[12];
    struct drm_i915_gem_relocation_entry relocs[2];
    struct drm_i915_gem_exec_object2 objs[3];
    uint32_t handle;
    uint32_t br13_bits = 0, cmd_bits = 0;
    int i = 0;
    int fd = d->fb_fd;
    int fb_version = intel_gen_version(fd);

    if (fb_version >= 9) {
        copy1(fd, dst, src, src_pitch, src_tiled, dst_pitch,
              width, height);
        return;
    }

    switch (bpp) {
        case 8:
            break;
        case 16:                /* supporting only RGB565, not ARGB1555 */
            br13_bits |= 1 << 24;
            break;
        case 32:
            br13_bits |= 3 << 24;
            break;
        default:
            error_report("cursor do not support this bpp: %d", bpp);
            exit(-1);
    }

    if (src_tiled) {
        src_pitch /= 4;
        cmd_bits |= XY_SRC_COPY_BLT_SRC_TILED;
    }

    batch[i++] = XY_SRC_COPY_BLT_CMD |
                 XY_SRC_COPY_BLT_WRITE_ALPHA |
                 XY_SRC_COPY_BLT_WRITE_RGB |
                 cmd_bits;
    if (fb_version >= 8) {
        batch[i - 1] |= 8;
    } else {
        batch[i - 1] |= 6;
    }

    batch[i++] = br13_bits |
                 (0xcc << 16) | /* copy ROP */
                 dst_pitch;
    batch[i++] = 0; /* dst x1,y1 */
    batch[i++] = (height << 16) | width; /* dst x2,y2 */
    batch[i++] = 0; /* dst reloc */
    if (fb_version >= 8) {
        batch[i++] = 0;
    }
    batch[i++] = 0; /* src x1,y1 */
    batch[i++] = src_pitch;
    batch[i++] = 0; /* src reloc */
    if (fb_version >= 8) {
        batch[i++] = 0;
    }
    batch[i++] = MI_BATCH_BUFFER_END;
    batch[i++] = MI_NOOP;

    handle = gem_create(fd, 4096);
    gem_write(fd, handle, 0, batch, sizeof(batch));

    fill_relocation(&relocs[0], dst, 4,
                    I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER);

    fill_relocation(&relocs[1], src, (fb_version >= 8) ? 8 : 7,
                    I915_GEM_DOMAIN_RENDER, 0);

    fill_object(&objs[0], dst, NULL, 0);
    if (src != dst) {
        fill_object(&objs[1], src, NULL, 0);
    }
    fill_object(&objs[2], handle, relocs, 2);

    exec_blit(fd, objs, 3, 0);
    gem_close(fd, handle);
}

#define TYPE_PLANE_PRIMARY        0
#define TYPE_PLANE_CURSOR         1

static void vgt_copy_to_user_buffer(VGTVGAState *vdev, uint32_t handle_src,
                                    int type,
                                    struct VGTPrimaryPlane *src_pp,
                                    struct VGTCursor *src_cs)
{
    struct VGTPrimaryPlane *pp = &vdev->pp;
    struct VGTCursor *cs = &vdev->cs;
    struct drm_i915_gem_userptr userptr;
    void *usrptr;
    int usrsize;
    uint32_t handle_dst, old_handle;

    if (type == TYPE_PLANE_PRIMARY) {
        usrptr = pp->fb_ptr;
        usrsize = pp->fb_size;
        handle_dst = pp->handle;
        old_handle = pp->old_handle;
    } else {
        usrptr = cs->fb_cursor_ptr;
        usrsize = cs->fb_cursor_size;
        handle_dst = cs->handle;
        old_handle = cs->old_handle;
    }

    if (!handle_dst) {
        memset(&userptr, 0, sizeof(userptr));
        userptr.user_ptr = (uint64_t)usrptr;
        userptr.user_size = usrsize;
        userptr.flags = 0;

        assert(drmIoctl(vdev->fb_fd, DRM_IOCTL_I915_GEM_USERPTR, &userptr) == 0);
        handle_dst = userptr.handle;
    }

    if (old_handle && (handle_src != old_handle)) {
         gem_close(vdev->fb_fd, old_handle);
    }

    if (type == TYPE_PLANE_PRIMARY) {
        pp->old_handle = handle_src;

        if (!pp->handle) {
            pp->handle = handle_dst;
        }
        DPRINTF("dst %d,src %d\n", pp->handle, handle_src);
        copy(vdev, pp->handle, handle_src,
            src_pp->new_stride, src_pp->tiled, pp->new_width * pp->new_depth / 8,
            pp->new_width, pp->new_height, pp->new_depth);
    } else {
        cs->old_handle = handle_src;

        if (!cs->handle) {
            cs->handle = handle_dst;
        }
        DPRINTF("copy cursor dst: %u, src: %u, src_pitch: %d, src_tiled: %d, "
                "dst_ptich %d, width %d, height %d", cs->handle,
                handle_src, cs->stride, 0, cs->width * cs->bpp / 8,
                cs->width, cs->height);
        copy(vdev, cs->handle, handle_src,
            src_cs->stride, 0, cs->width * cs->bpp / 8,
            cs->width, cs->height, cs->bpp);
    }
}

static pixman_format_code_t vgt_convert_pixman_format(uint32_t bpp, uint32_t format)
{
    DPRINTF("format %c%c%c%c\n", format & 0xFF, format >> 8 & 0xFF,
                                 format >> 16 &0xFF, format >> 24 & 0xFF);
    switch (format) {
    case DRM_FORMAT_XRGB8888:
        return PIXMAN_x8r8g8b8;
    case DRM_FORMAT_XBGR8888:
        return PIXMAN_x8b8g8r8;
    case DRM_FORMAT_RGBX8888:
        return PIXMAN_r8g8b8x8;
    case DRM_FORMAT_BGRX8888:
        return PIXMAN_b8g8r8x8;
    case DRM_FORMAT_ARGB8888:
        return PIXMAN_a8r8g8b8;
    case DRM_FORMAT_ABGR8888:
        return PIXMAN_a8b8g8r8;
    case DRM_FORMAT_RGBA8888:
        return PIXMAN_r8g8b8a8;
    case DRM_FORMAT_BGRA8888:
        return PIXMAN_b8g8r8a8;
    default:
        return qemu_default_pixman_format(bpp, true);
    }
}

static void vgt_cursor_define(VGTVGAState *d, struct VGTCursor *new_cs,
                              uint32_t handle_src)
{
    struct VGTCursor *cs = &d->cs;
    QEMUCursor *qc;

    cs->hot_x = new_cs->hot_x;
    cs->hot_y = new_cs->hot_y;
    cs->pos_x = new_cs->pos_x;
    cs->pos_y = new_cs->pos_y;
    cs->enabled = new_cs->enabled;
    cs->bpp = new_cs->bpp;
    cs->width = new_cs->width;
    cs->height = new_cs->height;
    cs->stride = new_cs->stride;

    /* indicate this is a new_handle */
    if (new_cs->fb_cursor_size) {
        if (cs->fb_cursor_ptr &&
            cs->fb_cursor_size != new_cs->fb_cursor_size) {
            gem_close(d->fb_fd, cs->handle);
            munmap(cs->fb_cursor_ptr, cs->fb_cursor_size);
            cs->handle = 0;
            cs->fb_cursor_ptr = NULL;
        }
        cs->fb_cursor_size = new_cs->fb_cursor_size;
    }

    if (!cs->fb_cursor_ptr) {
        cs->fb_cursor_ptr = mmap(NULL, cs->fb_cursor_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_SHARED,
                                 -1, 0);
        assert(cs->fb_cursor_ptr != MAP_FAILED);
    }

    vgt_copy_to_user_buffer(d, handle_src, 1, NULL, new_cs);

    //Update Cursor display
    qc = cursor_alloc(cs->width, cs->height);
    qc->hot_x = cs->hot_x;
    qc->hot_y = cs->hot_y;

    memcpy(qc->data, cs->fb_cursor_ptr, cs->width * cs->height * sizeof(uint32_t));

#if 0
    cursor_print_ascii_art(cs->qc, "vgt/alpha");
#endif
    dpy_cursor_define(d->vga.con, qc);
    cursor_put(qc);
    dpy_mouse_set(d->vga.con, cs->pos_x, cs->pos_y, cs->enabled);
}

static void vgt_replace_surface(VGTVGAState *vdev, struct VGTPrimaryPlane *new_pp)
{
    struct VGTPrimaryPlane *pp = &vdev->pp;
    DisplaySurface *surface;
    int stride;

    if (pp->fb_ptr) {
        gem_close(vdev->fb_fd, pp->handle);
        munmap(pp->fb_ptr, pp->fb_size);
        pp->handle = 0;
    }

    *pp = *new_pp;

    pp->fb_ptr = mmap(NULL, pp->fb_size,
                      PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_SHARED,
                      -1, 0);
    assert(pp->fb_ptr != MAP_FAILED);

    stride = (pp->new_depth * pp->new_width) / 8;
    pixman_format_code_t format =
        vgt_convert_pixman_format(pp->new_depth, pp->format);

    DPRINTF("new width: %d, new height: %d, stride: %d %p\n", pp->new_width,
            pp->new_height, stride, pp->fb_ptr);
    surface = qemu_create_displaysurface_from(pp->new_width, pp->new_height,
                                              format, stride,
                                              pp->fb_ptr);
    dpy_gfx_replace_surface(vdev->vga.con, surface);
}

static int vgt_check_size(VGTVGAState *vdev, struct VGTPrimaryPlane *pp)
{
    DisplaySurface *surface = qemu_console_surface(vdev->vga.con);
    pixman_format_code_t format =
        vgt_convert_pixman_format(pp->new_depth, pp->format);

    if (pp->new_width != surface_width(surface) ||
        pp->new_height != surface_height(surface) ||
        pp->new_depth != surface_bits_per_pixel(surface) ||
        format != surface->format) {
        return 1;
    }

    return 0;
}

static uint32_t vgt_get_gem_vgtbuffer_handle(VGTVGAState *d,
                                  struct VGTPrimaryPlane *pp)
{
    struct drm_i915_gem_vgtbuffer vcreate;
    int vm_pipe = UINT_MAX;
    uint32_t start;
    static uint32_t old_start;

    assert(pp != NULL);

    memset(pp, 0, sizeof(struct VGTPrimaryPlane));
    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    vcreate.vmid = d->domid;
    vcreate.plane_id = I915_VGT_PLANE_PRIMARY;
    vcreate.phys_pipe_id = vm_pipe;
    vcreate.flags = I915_VGTBUFFER_QUERY_ONLY;

    drmIoctl(d->fb_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate);
    start = vcreate.start;

    pp->new_width = vcreate.width;
    pp->new_height = vcreate.height;
    pp->new_stride = vcreate.stride;
    pp->new_depth = vcreate.bpp;
    pp->x_pos = vcreate.x_pos;
    pp->y_pos = vcreate.y_pos;
    pp->format = vcreate.drm_format;
    pp->tiled = vcreate.tiled;

    if (start == 0) {
        old_start = 0;
        return 0;
    }

    if (start == old_start) {
        return d->pp.old_handle;
    }

    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    vcreate.vmid = d->domid;
    vcreate.plane_id = I915_VGT_PLANE_PRIMARY;
    vcreate.phys_pipe_id = vm_pipe;
    drmIoctl(d->fb_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate);

    //pp->fb_size = vcreate.size << 12;
    pp->fb_size = TARGET_PAGE_ALIGN(pp->new_stride * pp->new_height);

    old_start = vcreate.start;
    return vcreate.handle;
}

static uint32_t vgt_get_gem_cursor_handle(VGTVGAState *d,
                                          struct VGTCursor *cs)
{
    struct drm_i915_gem_vgtbuffer vcreate;
    int vm_pipe = UINT_MAX;
    uint32_t cursorstart;
    static uint32_t cursor_oldstart;

    //dpy_mouse_set(d->vga.con, d->cs.pos_x, d->cs.pos_y, 0);

    memset(cs, 0, sizeof(struct VGTCursor));
    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    vcreate.vmid = d->domid;
    vcreate.plane_id = I915_VGT_PLANE_CURSOR;
    vcreate.phys_pipe_id = vm_pipe;
    vcreate.flags = I915_VGTBUFFER_QUERY_ONLY;
    drmIoctl(d->fb_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate);
    cursorstart = vcreate.start;

    if (cursorstart == 0||
        cursorstart == -1) {
        return 0;
    }

    if (!vcreate.width ||
        !vcreate.height) {
        return 0;
    }

    cs->hot_x = vcreate.x_offset;
    cs->hot_y = vcreate.y_offset;
    cs->pos_x = vcreate.x_pos;
    cs->pos_y = vcreate.y_pos;
    cs->enabled = vcreate.enabled;
    cs->bpp = vcreate.bpp;
    cs->width = vcreate.width;
    cs->height = vcreate.height;
    cs->stride = vcreate.stride;

#define CHECK_RANGE(x)   ((x) >= 0 && (x) < (1 << 15))
    if (!CHECK_RANGE(cs->width) ||
        !CHECK_RANGE(cs->height) ||
        !CHECK_RANGE(cs->stride)) {
        return 0;
    }

    if (cursor_oldstart == cursorstart) {
        return d->cs.old_handle;
    }

    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    vcreate.vmid = d->domid;
    vcreate.plane_id = I915_VGT_PLANE_CURSOR;
    vcreate.phys_pipe_id = vm_pipe;

    drmIoctl(d->fb_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate);
    cs->fb_cursor_size = TARGET_PAGE_ALIGN(cs->stride * cs->height);

    cursor_oldstart = cursorstart;
    return vcreate.handle;
}

static void vgtvga_update_display(void *opaque)
{
    VGTVGAState *d = opaque;
    struct VGTPrimaryPlane new_pp;
    struct VGTCursor new_qc;
    uint32_t handle_src;

    if (!d->enabled && check_vgt_uevent()) {
        d->enabled = 1;
        udev_destroy();
    }

    if (!d->enabled) {
        d->vga.hw_ops->gfx_update(&d->vga);
        return;
    }

    handle_src = vgt_get_gem_vgtbuffer_handle(d, &new_pp);
    if (handle_src) {
        if (vgt_check_size(d, &new_pp)) {
            vgt_replace_surface(d, &new_pp);
        }
        vgt_copy_to_user_buffer(d, handle_src, 0, &new_pp, NULL);
    }

    handle_src = vgt_get_gem_cursor_handle(d, &new_qc);
    if (handle_src) {
        vgt_cursor_define(d, &new_qc, handle_src);
    }

    dpy_gfx_update(d->vga.con, d->pp.x_pos, d->pp.y_pos, d->pp.new_width, d->pp.new_height);
}

static const GraphicHwOps vgtvga_ops = {
    .invalidate  = vgtvga_invalidate_display,
    .gfx_update  = vgtvga_update_display,
    .text_update = vgtvga_text_update,
};

static bool vgt_check_composite_display(VGTVGAState *d)
{
    struct drm_i915_gem_vgtbuffer vcreate;
    int fd;

    fd = open("/dev/dri/card0", O_RDWR);
    memset(&vcreate, 0, sizeof(struct drm_i915_gem_vgtbuffer));
    vcreate.flags = I915_VGTBUFFER_CHECK_CAPABILITY;
    if (!drmIoctl(fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &vcreate)) {
        d->fb_fd = fd;
        return true;
    } else {
        close(fd);
        return false;
    }
}

static int vgt_initfn(PCIDevice *dev)
{
    VGTVGAState *d = DO_UPCAST(VGTVGAState, dev, dev);

    DPRINTF("vgt_initfn\n");
    vgt_host_dev_init(dev, &d->host_dev);
    d->domid = vgt_get_domid();

    vga_common_init(&d->vga, OBJECT(dev), true);
    vga_init(&d->vga, OBJECT(dev), pci_address_space(dev),
             pci_address_space_io(dev), true);

    /* vgtbuffer enabled ? */
    if (vgt_check_composite_display(d)) {
        udev_init();
    }
    d->vga.con = graphic_console_init(DEVICE(dev), 0, &vgtvga_ops, d);

    /* compatibility with pc-0.13 and older */
    vga_init_vbe(&d->vga, OBJECT(dev), pci_address_space(dev));
    rom_add_vga(VGABIOS_FILENAME);

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

static Property vga_vgt_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", VGTVGAState,
                       vga.vram_size_mb, 16),
    DEFINE_PROP_END_OF_LIST(),
};

static void vgt_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *ic = PCI_DEVICE_CLASS(klass);
    ic->init = vgt_initfn;
    dc->reset = vgt_reset;
    ic->exit = vgt_cleanupfn;
    dc->vmsd = &vmstate_vga_common;
    dc->props = vga_vgt_properties;
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
