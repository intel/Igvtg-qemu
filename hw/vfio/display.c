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

/* FIXME */
#ifndef DRM_PLANE_TYPE_PRIMARY
# define DRM_PLANE_TYPE_PRIMARY 1
# define DRM_PLANE_TYPE_CURSOR  2
#endif

/*
 * ui/console.c calls this to poll for updates.
 * Typically 30 calls per second (GUI_REFRESH_INTERVAL_DEFAULT).
 *
 * Using this is not required though, in case there is some kind of
 * notification support it is also possible to call the dpy_gl_*
 * functions in other code paths and leave this function empty.
 */
static VFIODMABuf *vfio_display_get_dmabuf(VFIOPCIDevice *vdev,
                                           uint32_t plane_type)
{
    struct vfio_device_gfx_plane_info plane;
    VFIODMABuf *dmabuf;
    static int errcnt;
    int dmabuf_fd, ret;

    memset(&plane, 0, sizeof(plane));
    plane.argsz = sizeof(plane);
    plane.flags = VFIO_GFX_PLANE_TYPE_DMABUF;
    plane.drm_plane_type = plane_type;
    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_QUERY_GFX_PLANE, &plane);
    if (ret < 0) {
        fprintf(stderr, "(%d) ioctl VFIO_DEVICE_QUERY_GFX_PLANE(%s): %s\r",
                ++errcnt,
                (plane_type == DRM_PLANE_TYPE_PRIMARY) ? "primary" : "cursor",
                strerror(errno));
        fflush(stderr);
        return NULL;
    }
    if (!plane.drm_format || !plane.size) {
        fprintf(stderr, "(%d) %s plane not initialized by guest\r",
                ++errcnt,
                (plane_type == DRM_PLANE_TYPE_PRIMARY) ? "primary" : "cursor");
        fflush(stderr);
        return NULL;
    }

    QTAILQ_FOREACH(dmabuf, &vdev->dmabufs, next) {
        if (dmabuf->dmabuf_id == plane.dmabuf_id) {
            /* found in list, move to head, return it */
            QTAILQ_REMOVE(&vdev->dmabufs, dmabuf, next);
            QTAILQ_INSERT_HEAD(&vdev->dmabufs, dmabuf, next);
            if (plane_type == DRM_PLANE_TYPE_CURSOR) {
                dmabuf->pos_x      = plane.x_pos;
                dmabuf->pos_y      = plane.y_pos;
            }
#if 1
            if (plane.width != dmabuf->buf.width ||
                plane.height != dmabuf->buf.height) {
                fprintf(stderr, "%s: cached dmabuf mismatch: id %d, "
                        "kernel %dx%d, cached %dx%d, plane %s\n",
                        __func__, plane.dmabuf_id,
                        plane.width, plane.height,
                        dmabuf->buf.width, dmabuf->buf.height,
                        (plane_type == DRM_PLANE_TYPE_PRIMARY)
                        ? "primary" : "cursor");
                abort();
            }
#endif
            return dmabuf;
        }
    }

    dmabuf_fd = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_GET_GFX_DMABUF, &plane.dmabuf_id);
    if (dmabuf_fd < 0) {
        fprintf(stderr, "(%d) ioctl VFIO_DEVICE_GET_GFX_DMABUF: %s\r",
                ++errcnt, strerror(errno));
        return NULL;
    }

    fprintf(stderr, "%s: new dmabuf: id %d, res %dx%d, "
            "format %c%c%c%c, plane %s, fd %d, hot +%d+%d\n",
            __func__, plane.dmabuf_id,
            plane.width, plane.height,
            (plane.drm_format >>  0) & 0xff,
            (plane.drm_format >>  8) & 0xff,
            (plane.drm_format >> 16) & 0xff,
            (plane.drm_format >> 24) & 0xff,
            (plane_type == DRM_PLANE_TYPE_PRIMARY) ? "primary" : "cursor",
            dmabuf_fd,
            plane.x_pos, plane.y_pos);

    dmabuf = g_new0(VFIODMABuf, 1);
    dmabuf->dmabuf_id  = plane.dmabuf_id;
    dmabuf->buf.width  = plane.width;
    dmabuf->buf.height = plane.height;
    dmabuf->buf.stride = plane.stride;
    dmabuf->buf.fourcc = plane.drm_format;
    dmabuf->buf.fd     = dmabuf_fd;
    if (plane_type == DRM_PLANE_TYPE_CURSOR) {
        dmabuf->pos_x      = plane.x_pos;
        dmabuf->pos_y      = plane.y_pos;
        dmabuf->hot_x      = plane.x_hot;
        dmabuf->hot_y      = plane.y_hot;
    }

    QTAILQ_INSERT_HEAD(&vdev->dmabufs, dmabuf, next);
    return dmabuf;
}

static void vfio_display_free_dmabufs(VFIOPCIDevice *vdev)
{
    char log[128]; int pos = 0;
    VFIODMABuf *dmabuf, *tmp;
    uint32_t keep = 5;

    QTAILQ_FOREACH_SAFE(dmabuf, &vdev->dmabufs, next, tmp) {
        if (keep > 0) {
            pos += sprintf(log + pos, " %d", dmabuf->buf.fd);
            keep--;
            continue;
        }
        assert(dmabuf != vdev->primary);
        QTAILQ_REMOVE(&vdev->dmabufs, dmabuf, next);
        fprintf(stderr, "%s: free dmabuf: fd %d (keep%s)\n",
                __func__, dmabuf->buf.fd, log);
        dpy_gl_release_dmabuf(vdev->display_con, &dmabuf->buf);
        close(dmabuf->buf.fd);
        g_free(dmabuf);
    }
}

static void vfio_display_dmabuf_update(void *opaque)
{
    VFIOPCIDevice *vdev = opaque;
    VFIODMABuf *primary, *cursor;
    bool free_bufs = false;

    primary = vfio_display_get_dmabuf(vdev, DRM_PLANE_TYPE_PRIMARY);
    if (primary == NULL) {
        return;
    }

    if (vdev->primary != primary) {
        vdev->primary = primary;
        qemu_console_resize(vdev->display_con,
                            primary->buf.width, primary->buf.height);
        dpy_gl_scanout_dmabuf(vdev->display_con,
                              &primary->buf);
        free_bufs = true;
    }

    cursor = vfio_display_get_dmabuf(vdev, DRM_PLANE_TYPE_CURSOR);
    if (vdev->cursor != cursor) {
        vdev->cursor = cursor;
        dpy_gl_cursor_dmabuf(vdev->display_con,
                             &cursor->buf);
        free_bufs = true;
    }
    if (cursor != NULL) {
        bool have_hot = (cursor->hot_x != 0xffffffff &&
                         cursor->hot_y != 0xffffffff);
        dpy_gl_cursor_position(vdev->display_con,
                               have_hot, true,
                               cursor->hot_x,
                               cursor->hot_y,
                               cursor->pos_x,
                               cursor->pos_y);
    }

    dpy_gl_update(vdev->display_con, 0, 0,
                  primary->buf.width, primary->buf.height);

    if (free_bufs) {
        vfio_display_free_dmabufs(vdev);
    }
}

static const GraphicHwOps vfio_display_dmabuf_ops = {
    .gfx_update = vfio_display_dmabuf_update,
};

static int vfio_display_dmabuf_init(VFIOPCIDevice *vdev, Error **errp)
{
    if (!display_opengl) {
        error_setg(errp, "vfio-display-dmabuf: opengl not available");
        return -1;
    }

    vdev->display_con = graphic_console_init(DEVICE(vdev), 0,
                                             &vfio_display_dmabuf_ops,
                                             vdev);
    /* TODO: disable hotplug (there is no graphic_console_close) */
    return 0;
}

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
        return vfio_display_dmabuf_init(vdev, errp);
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
