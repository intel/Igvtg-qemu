/*
 * display support for mdev based vgpu devices
 *
 * Copyright Red Hat, Inc. 2017
 *
 * Authors:
 *    Gerd Hoffmann
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <linux/vfio.h>
#include <sys/ioctl.h>

#include "sysemu/sysemu.h"
#include "ui/console.h"
#include "pci.h"

/* ---------------------------------------------------------------------- */

static void vfio_display_region_update(void *opaque)
{
    VFIOPCIDevice *vdev = opaque;
    struct vfio_device_gfx_plane_info plane;
    struct vfio_region_info *region = NULL;
    pixman_format_code_t format = PIXMAN_x8r8g8b8;
    int ret;

    memset(&plane, 0, sizeof(plane));
    plane.argsz = sizeof(plane);
    plane.flags = VFIO_GFX_PLANE_TYPE_REGION;
    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_QUERY_GFX_PLANE, &plane);
    if (ret < 0) {
        fprintf(stderr, "ioctl VFIO_DEVICE_QUERY_GFX_PLANE: %s\n",
                strerror(errno));
        return;
    }
    if (!plane.drm_format || !plane.size) {
        return;
    }
    format = qemu_drm_format_to_pixman(plane.drm_format);

    if (vdev->region_mmap && vdev->region_index != plane.region_index) {
        /* region changed */
        munmap(vdev->region_mmap, vdev->region_size);
        vdev->region_mmap = NULL;
        vdev->region_surface = NULL;
    }

    if (vdev->region_surface &&
        (surface_width(vdev->region_surface) != plane.width ||
         surface_height(vdev->region_surface) != plane.height ||
         surface_format(vdev->region_surface) != format)) {
        /* size changed */
        vdev->region_surface = NULL;
    }

    if (vdev->region_mmap == NULL) {
        /* mmap region */
        ret = vfio_get_region_info(&vdev->vbasedev, plane.region_index,
                                   &region);
        if (ret != 0) {
            fprintf(stderr, "%s: vfio_get_region_info(%d): %s\n",
                    __func__, plane.region_index, strerror(-ret));
            return;
        }
        vdev->region_size = region->size;
        vdev->region_mmap = mmap(NULL, region->size,
                                 PROT_READ, MAP_SHARED,
                                 vdev->vbasedev.fd,
                                 region->offset);
        if (vdev->region_mmap == MAP_FAILED) {
            fprintf(stderr, "%s: mmap region %d: %s\n", __func__,
                    plane.region_index, strerror(errno));
            vdev->region_mmap = NULL;
            g_free(region);
            return;
        }
        g_free(region);
    }

    if (vdev->region_surface == NULL) {
        /* create surface */
        vdev->region_surface = qemu_create_displaysurface_from
            (plane.width, plane.height, format,
             plane.stride, vdev->region_mmap);
        dpy_gfx_replace_surface(vdev->display_con, vdev->region_surface);
    }

    /* full screen update */
    dpy_gfx_update(vdev->display_con, 0, 0,
                   surface_width(vdev->region_surface),
                   surface_height(vdev->region_surface));

}

static const GraphicHwOps vfio_display_region_ops = {
    .gfx_update = vfio_display_region_update,
};

static int vfio_display_region_init(VFIOPCIDevice *vdev, Error **errp)
{
    vdev->display_con = graphic_console_init(DEVICE(vdev), 0,
                                             &vfio_display_region_ops,
                                             vdev);
    /* TODO: disable hotplug (there is no graphic_console_close) */
    return 0;
}

/* ---------------------------------------------------------------------- */

int vfio_display_probe(VFIOPCIDevice *vdev, Error **errp)
{
    struct vfio_device_gfx_plane_info probe;
    int ret;

    memset(&probe, 0, sizeof(probe));
    probe.argsz = sizeof(probe);
    probe.flags = VFIO_GFX_PLANE_TYPE_PROBE | VFIO_GFX_PLANE_TYPE_DMABUF;
    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_QUERY_GFX_PLANE, &probe);
    if (ret == 0) {
        error_setg(errp, "vfio-display: dmabuf support not implemented yet");
        return -1;
    }

    memset(&probe, 0, sizeof(probe));
    probe.argsz = sizeof(probe);
    probe.flags = VFIO_GFX_PLANE_TYPE_PROBE | VFIO_GFX_PLANE_TYPE_REGION;
    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_QUERY_GFX_PLANE, &probe);
    if (ret == 0) {
        return vfio_display_region_init(vdev, errp);
    }

    error_setg(errp, "vfio: device doesn't support any (known) display method");
    return -1;
}
