#include "qemu/osdep.h"

#include "hw/vfio/vfio-common.h"
#include "migration/blocker.h"
#include "migration/register.h"
#include "qapi/error.h"
#include "pci.h"
#include "sysemu/kvm.h"
#include "exec/ram_addr.h"

#define VFIO_SAVE_FLAG_SETUP 0
#define VFIO_SAVE_FLAG_PCI 1
#define VFIO_SAVE_FLAG_DEVCONFIG 2
#define VFIO_SAVE_FLAG_DEVMEMORY 4
#define VFIO_SAVE_FLAG_CONTINUE 8

static int vfio_device_state_region_setup(VFIOPCIDevice *vdev,
        VFIORegion *region, uint32_t subtype, const char *name)
{
    VFIODevice *vbasedev = &vdev->vbasedev;
    struct vfio_region_info *info;
    int ret;

    ret = vfio_get_dev_region_info(vbasedev, VFIO_REGION_TYPE_DEVICE_STATE,
            subtype, &info);
    if (ret) {
        error_report("Failed to get info of region %s", name);
        return ret;
    }

    if (vfio_region_setup(OBJECT(vdev), vbasedev,
            region, info->index, name)) {
        error_report("Failed to setup migrtion region %s", name);
        return ret;
    }

    if (vfio_region_mmap(region)) {
        error_report("Failed to mmap migrtion region %s", name);
    }

    return 0;
}

bool vfio_device_data_cap_system_memory(VFIOPCIDevice *vdev)
{
   return !!(vdev->migration->data_caps & VFIO_DEVICE_DATA_CAP_SYSTEM_MEMORY);
}

bool vfio_device_data_cap_device_memory(VFIOPCIDevice *vdev)
{
   return !!(vdev->migration->data_caps & VFIO_DEVICE_DATA_CAP_DEVICE_MEMORY);
}

static bool vfio_device_state_region_mmaped(VFIORegion *region)
{
    bool mmaped = true;
    if (region->nr_mmaps != 1 || region->mmaps[0].offset ||
            (region->size != region->mmaps[0].size) ||
            (region->mmaps[0].mmap == NULL)) {
        mmaped = false;
    }

    return mmaped;
}

static int vfio_get_device_config_size(VFIOPCIDevice *vdev)
{
    VFIODevice *vbasedev = &vdev->vbasedev;
    VFIORegion *region_ctl =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL];
    VFIORegion *region_config =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_DATA_CONFIG];
    uint64_t len;
    int sz;

    sz = sizeof(len);
    if (pread(vbasedev->fd, &len, sz,
                region_ctl->fd_offset +
                offsetof(struct vfio_device_state_ctl, device_config.size))
            != sz) {
        error_report("vfio: Failed to get length of device config");
        return -1;
    }
    if (len > region_config->size) {
        error_report("vfio: Error device config length");
        return -1;
    }
    vdev->migration->devconfig_size = len;

    return 0;
}

static int vfio_set_device_config_size(VFIOPCIDevice *vdev, uint64_t size)
{
    VFIODevice *vbasedev = &vdev->vbasedev;
    VFIORegion *region_ctl =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL];
    VFIORegion *region_config =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_DATA_CONFIG];
    int sz;

    if (size > region_config->size) {
        return -1;
    }

    sz = sizeof(size);
    if (pwrite(vbasedev->fd, &size, sz,
                region_ctl->fd_offset +
                offsetof(struct vfio_device_state_ctl, device_config.size))
            != sz) {
        error_report("vfio: Failed to set length of device config");
        return -1;
    }
    vdev->migration->devconfig_size = size;
    return 0;
}

static int vfio_save_data_device_config(VFIOPCIDevice *vdev, QEMUFile *f)
{
    VFIODevice *vbasedev = &vdev->vbasedev;
    VFIORegion *region_ctl =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL];
    VFIORegion *region_config =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_DATA_CONFIG];
    void *dest;
    uint32_t sz;
    uint8_t *buf = NULL;
    uint32_t action = VFIO_DEVICE_DATA_ACTION_GET_BUFFER;
    uint64_t len = vdev->migration->devconfig_size;

    qemu_put_be64(f, len);

    sz = sizeof(action);
    if (pwrite(vbasedev->fd, &action, sz,
                region_ctl->fd_offset +
                offsetof(struct vfio_device_state_ctl, device_config.action))
            != sz) {
        error_report("vfio: action failure for device config get buffer");
        return -1;
    }

    if (!vfio_device_state_region_mmaped(region_config)) {
        buf = g_malloc(len);
        if (buf == NULL) {
            error_report("vfio: Failed to allocate memory for migrate");
            return -1;
        }
        if (pread(vbasedev->fd, buf, len, region_config->fd_offset) != len) {
            error_report("vfio: Failed read device config buffer");
            return -1;
        }
        qemu_put_buffer(f, buf, len);
        g_free(buf);
    } else {
        dest = region_config->mmaps[0].mmap;
        qemu_put_buffer(f, dest, len);
    }
    return 0;
}

static int vfio_load_data_device_config(VFIOPCIDevice *vdev,
                            QEMUFile *f, uint64_t len)
{
    VFIODevice *vbasedev = &vdev->vbasedev;
    VFIORegion *region_ctl =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL];
    VFIORegion *region_config =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_DATA_CONFIG];
    void *dest;
    uint32_t sz;
    uint8_t *buf = NULL;
    uint32_t action = VFIO_DEVICE_DATA_ACTION_SET_BUFFER;

    vfio_set_device_config_size(vdev, len);

    if (!vfio_device_state_region_mmaped(region_config)) {
        buf = g_malloc(len);
        if (buf == NULL) {
            error_report("vfio: Failed to allocate memory for migrate");
            return -1;
        }
        qemu_get_buffer(f, buf, len);
        if (pwrite(vbasedev->fd, buf, len,
                    region_config->fd_offset) != len) {
            error_report("vfio: Failed to write devie config buffer");
            return -1;
        }
        g_free(buf);
    } else {
        dest = region_config->mmaps[0].mmap;
        qemu_get_buffer(f, dest, len);
    }

    sz = sizeof(action);
    if (pwrite(vbasedev->fd, &action, sz,
                region_ctl->fd_offset +
                offsetof(struct vfio_device_state_ctl, device_config.action))
            != sz) {
        error_report("vfio: action failure for device config set buffer");
        return -1;
    }

    return 0;
}

static int vfio_set_dirty_page_bitmap_chunk(VFIOPCIDevice *vdev,
        uint64_t start_addr, uint64_t page_nr)
{

    VFIODevice *vbasedev = &vdev->vbasedev;
    VFIORegion *region_ctl =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL];
    VFIORegion *region_bitmap =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_DATA_BITMAP];
    unsigned long bitmap_size =
                    BITS_TO_LONGS(page_nr) * sizeof(unsigned long);
    uint32_t sz;

    struct {
        __u64 start_addr;
        __u64 page_nr;
    } system_memory;
    system_memory.start_addr = start_addr;
    system_memory.page_nr = page_nr;
    sz = sizeof(system_memory);
    if (pwrite(vbasedev->fd, &system_memory, sz,
                region_ctl->fd_offset +
                offsetof(struct vfio_device_state_ctl, system_memory))
            != sz) {
        error_report("vfio: Failed to set system memory range for dirty pages");
        return -1;
    }

    if (!vfio_device_state_region_mmaped(region_bitmap)) {
        void *bitmap = g_malloc0(bitmap_size);

        if (pread(vbasedev->fd, bitmap, bitmap_size,
                    region_bitmap->fd_offset) != bitmap_size) {
            error_report("vfio: Failed to read dirty bitmap data");
            return -1;
        }

        cpu_physical_memory_set_dirty_lebitmap(bitmap, start_addr, page_nr);

        g_free(bitmap);
    } else {
        cpu_physical_memory_set_dirty_lebitmap(
                    region_bitmap->mmaps[0].mmap,
                    start_addr, page_nr);
    }
   return 0;
}

int vfio_set_dirty_page_bitmap(VFIOPCIDevice *vdev,
        uint64_t start_addr, uint64_t page_nr)
{
    VFIORegion *region_bitmap =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_DATA_BITMAP];
    unsigned long chunk_size = region_bitmap->size;
    uint64_t chunk_pg_nr = (chunk_size / sizeof(unsigned long)) *
                                BITS_PER_LONG;

    uint64_t cnt_left;
    int rc = 0;

    cnt_left = page_nr;

    while (cnt_left >= chunk_pg_nr) {
        rc = vfio_set_dirty_page_bitmap_chunk(vdev, start_addr, chunk_pg_nr);
        if (rc) {
            goto exit;
        }
        cnt_left -= chunk_pg_nr;
        start_addr += start_addr;
   }
   rc = vfio_set_dirty_page_bitmap_chunk(vdev, start_addr, cnt_left);

exit:
   return rc;
}

static int vfio_set_device_state(VFIOPCIDevice *vdev,
        uint32_t dev_state)
{
    VFIODevice *vbasedev = &vdev->vbasedev;
    VFIORegion *region =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL];
    uint32_t sz = sizeof(dev_state);

    if (!vdev->migration) {
        return -1;
    }

    if (pwrite(vbasedev->fd, &dev_state, sz,
              region->fd_offset +
              offsetof(struct vfio_device_state_ctl, device_state))
            != sz) {
        error_report("vfio: Failed to set device state %d", dev_state);
        return -1;
    }
    vdev->migration->device_state = dev_state;
    return 0;
}

static int vfio_get_device_data_caps(VFIOPCIDevice *vdev)
{
    VFIODevice *vbasedev = &vdev->vbasedev;
    VFIORegion *region =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL];

    uint32_t caps;
    uint32_t size = sizeof(caps);

    if (pread(vbasedev->fd, &caps, size,
                region->fd_offset +
                offsetof(struct vfio_device_state_ctl, caps))
            != size) {
        error_report("%s Failed to read data caps of device states",
                vbasedev->name);
        return -1;
    }
    vdev->migration->data_caps = caps;
    return 0;
}


static int vfio_check_devstate_version(VFIOPCIDevice *vdev)
{
    VFIODevice *vbasedev = &vdev->vbasedev;
    VFIORegion *region =
        &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL];

    uint32_t version;
    uint32_t size = sizeof(version);

    if (pread(vbasedev->fd, &version, size,
                region->fd_offset +
                offsetof(struct vfio_device_state_ctl, version))
            != size) {
        error_report("%s Failed to read version of device state interfaces",
                vbasedev->name);
        return -1;
    }

    if (version != VFIO_DEVICE_STATE_INTERFACE_VERSION) {
        error_report("%s migration version mismatch, right version is %d",
                vbasedev->name, VFIO_DEVICE_STATE_INTERFACE_VERSION);
        return -1;
    }

    return 0;
}

static void vfio_vm_change_state_handler(void *pv, int running, RunState state)
{
    VFIOPCIDevice *vdev = pv;
    uint32_t dev_state = vdev->migration->device_state;

    if (!running) {
        dev_state |= VFIO_DEVICE_STATE_STOP;
    } else {
        dev_state &= ~VFIO_DEVICE_STATE_STOP;
    }

    vfio_set_device_state(vdev, dev_state);
}

static void vfio_save_live_pending(QEMUFile *f, void *opaque,
                                   uint64_t max_size,
                                   uint64_t *res_precopy_only,
                                   uint64_t *res_compatible,
                                   uint64_t *res_post_copy_only)
{
    VFIOPCIDevice *vdev = opaque;

    if (!vfio_device_data_cap_device_memory(vdev)) {
        return;
    }

    return;
}

static int vfio_save_iterate(QEMUFile *f, void *opaque)
{
    VFIOPCIDevice *vdev = opaque;

    if (!vfio_device_data_cap_device_memory(vdev)) {
        return 0;
    }

    return 0;
}

static void vfio_pci_load_config(VFIOPCIDevice *vdev, QEMUFile *f)
{
    PCIDevice *pdev = &vdev->pdev;
    uint32_t ctl, msi_lo, msi_hi, msi_data, bar_cfg, i;
    bool msi_64bit;

    /* retore pci bar configuration */
    ctl = pci_default_read_config(pdev, PCI_COMMAND, 2);
    vfio_pci_write_config(pdev, PCI_COMMAND,
            ctl & (!(PCI_COMMAND_IO | PCI_COMMAND_MEMORY)), 2);
    for (i = 0; i < PCI_ROM_SLOT; i++) {
        bar_cfg = qemu_get_be32(f);
        vfio_pci_write_config(pdev, PCI_BASE_ADDRESS_0 + i * 4, bar_cfg, 4);
    }
    vfio_pci_write_config(pdev, PCI_COMMAND,
            ctl | PCI_COMMAND_IO | PCI_COMMAND_MEMORY, 2);

    /* restore msi configuration */
    ctl = pci_default_read_config(pdev, pdev->msi_cap + PCI_MSI_FLAGS, 2);
    msi_64bit = !!(ctl & PCI_MSI_FLAGS_64BIT);

    vfio_pci_write_config(&vdev->pdev,
            pdev->msi_cap + PCI_MSI_FLAGS,
            ctl & (!PCI_MSI_FLAGS_ENABLE), 2);

    msi_lo = qemu_get_be32(f);
    vfio_pci_write_config(pdev, pdev->msi_cap + PCI_MSI_ADDRESS_LO, msi_lo, 4);

    if (msi_64bit) {
        msi_hi = qemu_get_be32(f);
        vfio_pci_write_config(pdev, pdev->msi_cap + PCI_MSI_ADDRESS_HI,
                msi_hi, 4);
    }
    msi_data = qemu_get_be32(f);
    vfio_pci_write_config(pdev,
            pdev->msi_cap + (msi_64bit ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32),
            msi_data, 2);

    vfio_pci_write_config(&vdev->pdev, pdev->msi_cap + PCI_MSI_FLAGS,
            ctl | PCI_MSI_FLAGS_ENABLE, 2);

}

static int vfio_load_state(QEMUFile *f, void *opaque, int version_id)
{
    VFIOPCIDevice *vdev = opaque;
    int flag;
    uint64_t len;
    int ret = 0;

    if (version_id != VFIO_DEVICE_STATE_INTERFACE_VERSION) {
        return -EINVAL;
    }

    do {
        flag = qemu_get_byte(f);

        switch (flag & ~VFIO_SAVE_FLAG_CONTINUE) {
        case VFIO_SAVE_FLAG_SETUP:
            break;
        case VFIO_SAVE_FLAG_PCI:
            vfio_pci_load_config(vdev, f);
            break;
        case VFIO_SAVE_FLAG_DEVCONFIG:
            len = qemu_get_be64(f);
            vfio_load_data_device_config(vdev, f, len);
            break;
        default:
            ret = -EINVAL;
        }
    } while (flag & VFIO_SAVE_FLAG_CONTINUE);

    return ret;
}

static void vfio_pci_save_config(VFIOPCIDevice *vdev, QEMUFile *f)
{
    PCIDevice *pdev = &vdev->pdev;
    uint32_t msi_cfg, msi_lo, msi_hi, msi_data, bar_cfg, i;
    bool msi_64bit;

    for (i = 0; i < PCI_ROM_SLOT; i++) {
        bar_cfg = pci_default_read_config(pdev, PCI_BASE_ADDRESS_0 + i * 4, 4);
        qemu_put_be32(f, bar_cfg);
    }

    msi_cfg = pci_default_read_config(pdev, pdev->msi_cap + PCI_MSI_FLAGS, 2);
    msi_64bit = !!(msi_cfg & PCI_MSI_FLAGS_64BIT);

    msi_lo = pci_default_read_config(pdev,
            pdev->msi_cap + PCI_MSI_ADDRESS_LO, 4);
    qemu_put_be32(f, msi_lo);

    if (msi_64bit) {
        msi_hi = pci_default_read_config(pdev,
                pdev->msi_cap + PCI_MSI_ADDRESS_HI,
                4);
        qemu_put_be32(f, msi_hi);
    }

    msi_data = pci_default_read_config(pdev,
            pdev->msi_cap + (msi_64bit ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32),
            2);
    qemu_put_be32(f, msi_data);

}

static int vfio_save_complete_precopy(QEMUFile *f, void *opaque)
{
    VFIOPCIDevice *vdev = opaque;
    int rc = 0;

    qemu_put_byte(f, VFIO_SAVE_FLAG_PCI | VFIO_SAVE_FLAG_CONTINUE);
    vfio_pci_save_config(vdev, f);

    qemu_put_byte(f, VFIO_SAVE_FLAG_DEVCONFIG);
    rc += vfio_get_device_config_size(vdev);
    rc += vfio_save_data_device_config(vdev, f);

    return rc;
}

static int vfio_save_setup(QEMUFile *f, void *opaque)
{
    VFIOPCIDevice *vdev = opaque;
    qemu_put_byte(f, VFIO_SAVE_FLAG_SETUP);

    vfio_set_device_state(vdev, VFIO_DEVICE_STATE_RUNNING |
            VFIO_DEVICE_STATE_LOGGING);
    return 0;
}

static int vfio_load_setup(QEMUFile *f, void *opaque)
{
    return 0;
}

static void vfio_save_cleanup(void *opaque)
{
    VFIOPCIDevice *vdev = opaque;
    uint32_t dev_state = vdev->migration->device_state;

    dev_state &= ~VFIO_DEVICE_STATE_LOGGING;

    vfio_set_device_state(vdev, dev_state);
}

static SaveVMHandlers savevm_vfio_handlers = {
    .save_setup = vfio_save_setup,
    .save_live_pending = vfio_save_live_pending,
    .save_live_iterate = vfio_save_iterate,
    .save_live_complete_precopy = vfio_save_complete_precopy,
    .save_cleanup = vfio_save_cleanup,
    .load_setup = vfio_load_setup,
    .load_state = vfio_load_state,
};

int vfio_migration_init(VFIOPCIDevice *vdev, Error **errp)
{
    int ret;
    Error *local_err = NULL;
    vdev->migration = g_new0(VFIOMigration, 1);

    if (vfio_device_state_region_setup(vdev,
              &vdev->migration->region[VFIO_DEVSTATE_REGION_CTL],
              VFIO_REGION_SUBTYPE_DEVICE_STATE_CTL,
              "device-state-ctl")) {
        goto error;
    }

    if (vfio_check_devstate_version(vdev)) {
        goto error;
    }

    if (vfio_get_device_data_caps(vdev)) {
        goto error;
    }

    if (vfio_device_state_region_setup(vdev,
              &vdev->migration->region[VFIO_DEVSTATE_REGION_DATA_CONFIG],
              VFIO_REGION_SUBTYPE_DEVICE_STATE_DATA_CONFIG,
              "device-state-data-device-config")) {
        goto error;
    }

    if (vfio_device_data_cap_device_memory(vdev)) {
        error_report("No suppport of data cap device memory Yet");
        goto error;
    }

    if (vfio_device_data_cap_system_memory(vdev) &&
            vfio_device_state_region_setup(vdev,
              &vdev->migration->region[VFIO_DEVSTATE_REGION_DATA_BITMAP],
              VFIO_REGION_SUBTYPE_DEVICE_STATE_DATA_DIRTYBITMAP,
              "device-state-data-dirtybitmap")) {
        goto error;
    }

    vdev->migration->device_state = VFIO_DEVICE_STATE_RUNNING;

    register_savevm_live(NULL, TYPE_VFIO_PCI, -1,
            VFIO_DEVICE_STATE_INTERFACE_VERSION,
            &savevm_vfio_handlers,
            vdev);

    vdev->migration->vm_state =
        qemu_add_vm_change_state_handler(vfio_vm_change_state_handler, vdev);

    return 0;
error:
    error_setg(&vdev->migration_blocker,
            "VFIO device doesn't support migration");
    ret = migrate_add_blocker(vdev->migration_blocker, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        error_free(vdev->migration_blocker);
    }

    g_free(vdev->migration);
    vdev->migration = NULL;

    return ret;
}

void vfio_migration_finalize(VFIOPCIDevice *vdev)
{
    if (vdev->migration) {
        int i;
        qemu_del_vm_change_state_handler(vdev->migration->vm_state);
        unregister_savevm(NULL, TYPE_VFIO_PCI, vdev);
        for (i = 0; i < VFIO_DEVSTATE_REGION_NUM; i++) {
            vfio_region_finalize(&vdev->migration->region[i]);
        }
        g_free(vdev->migration);
        vdev->migration = NULL;
    } else if (vdev->migration_blocker) {
        migrate_del_blocker(vdev->migration_blocker);
        error_free(vdev->migration_blocker);
    }
}
