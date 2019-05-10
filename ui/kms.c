#include "qemu/osdep.h"
#include "qemu-common.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <sys/ioctl.h>

#include "drm_fourcc.h"
#include "/usr/include/drm/i915_drm.h"

struct drm_display {
    int fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t primary_id;

    int crtc_index;
    drmModeModeInfo mode;
} drm_display;

/* static DisplayChangeListener *dcl; */
/* static bool direct_flip = false; */

static bool assigned = false;


static void kms_refresh(DisplayChangeListener *dcl)
{
    if (assigned)
        return;

    graphic_hw_update(dcl->con);
}

static void kms_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    ;
}

static void kms_gfx_switch(DisplayChangeListener *dcl,
                           struct DisplaySurface *new_surface)
{
    ;
}

static void release_handle(int fd, uint32_t handle)
{
    struct drm_gem_close c_gem;
    int ret;

    if (handle == 0)
        return;

    c_gem.handle = handle;
    c_gem.pad = 0;

    ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c_gem);
    if (ret) {
        printf("[DRM] cannot release the handle :%s\n",strerror(errno));
    }
}

static void kms_scanout_dmabuf(DisplayChangeListener *dcl,
                                  QemuDmaBuf *dmabuf)
{
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t modifier[4] = {0};
    int ret;

    if (dmabuf->handle == 0) {
        ret = drmPrimeFDToHandle(drm_display.fd, dmabuf->fd, &dmabuf->handle);
        if (ret) {
            printf("[Prime] FD to Handle failed:%s\n",strerror(errno));
            return;
        }
    }

    if (dmabuf->drm_id == 0) {
        handles[0] = dmabuf->handle;
        pitches[0] = dmabuf->stride;
        modifier[0] = dmabuf->format_mod;
        ret = drmModeAddFB2WithModifiers(drm_display.fd, dmabuf->width, dmabuf->height,
                                         dmabuf->format, handles, pitches, offsets, modifier,
                                         &dmabuf->drm_id, DRM_MODE_FB_MODIFIERS);
        if (ret) {
            printf("[KMS] cannot create framebuffer:%s\n", strerror(errno));
            release_handle(drm_display.fd, dmabuf->handle);
            dmabuf->handle = 0;
        }
    }
    ret = drmModeSetCrtc(drm_display.fd, drm_display.crtc_id, dmabuf->drm_id, 0, 0, &drm_display.connector_id, 1, &drm_display.mode);
    if (ret)
        printf("Cannot Set Crtc\n");

    assigned = true;
}

static void kms_cursor_dmabuf(DisplayChangeListener *dcl,
                                 QemuDmaBuf *dmabuf, bool have_hot,
                                 uint32_t hot_x, uint32_t hot_y)
{
    ;
}

static void kms_cursor_position(DisplayChangeListener *dcl,
                                   uint32_t pos_x, uint32_t pos_y)
{
    ;
}


static void kms_release_dmabuf(DisplayChangeListener *dcl,
                                  QemuDmaBuf *dmabuf)
{
    /* Release the drm_framebuffer */
    drmModeRmFB(drm_display.fd, dmabuf->drm_id);
    dmabuf->drm_id = 0;
    release_handle(drm_display.fd, dmabuf->handle);
    dmabuf->handle = 0;

}

static void kms_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    ;
}

static QEMUGLContext kms_create_context(DisplayChangeListener *dcl,
                                      QEMUGLParams *params)
{
    return NULL;
}

static const DisplayChangeListenerOps kms_ops = {
    .dpy_name                = "kms",
    .dpy_refresh             = kms_refresh,
    .dpy_gl_ctx_create       = kms_create_context,
    .dpy_gfx_update          = kms_gfx_update,
    .dpy_gfx_switch          = kms_gfx_switch,
    .dpy_gl_scanout_dmabuf   = kms_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = kms_cursor_dmabuf,
    .dpy_gl_cursor_position  = kms_cursor_position,
    .dpy_gl_release_dmabuf   = kms_release_dmabuf,
    .dpy_gl_update           = kms_scanout_flush,
};

static int display_init(void)
{

    const char *card = "/dev/dri/card0";
    drmModeRes *res;
    drmModeConnector *conn;
    drmModeEncoderPtr enc;
    drmModePlaneRes *plane_res;
    int found_primary = 0;
    unsigned int i, j;
    int ret;

    /* Open card */
    drm_display.fd = open(card, O_RDWR | O_CLOEXEC);
    if (drm_display.fd < 0) {
        ret = -errno;
        printf("KMS: cannot open card0 \n");
        return ret;
    }

    /* Retrieve resources */
    res = drmModeGetResources(drm_display.fd);
    if (!res) {
        fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
                errno);
        return -errno;
    }

    /* Iterate all connectors */
    for (i = 0; i < res->count_connectors; ++i) {
	/* Get information for each connector */
        conn = drmModeGetConnector(drm_display.fd, res->connectors[i]);
        if (!conn) {
            fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
                    i, res->connectors[i], errno);
            drmModeFreeConnector(conn);
            continue;
        }

        /* Check if a monitor is connected */
        if (conn->connection != DRM_MODE_CONNECTED) {
            fprintf(stderr, "ignoring unused connector %u\n",
                    conn->connector_id);
            drmModeFreeConnector(conn);
            continue;
        }

        /* Check if there is at least one valid mode */
        if (conn->count_modes == 0) {
            fprintf(stderr, "no valid mode for connector %u\n",
                    conn->connector_id);
            drmModeFreeConnector(conn);
            continue;
        }

        drm_display.connector_id = conn->connector_id;
        memcpy(&drm_display.mode, &conn->modes[0], sizeof(drm_display.mode));
        break;
    }

    if (!drm_display.connector_id) {
        return -1;
    }

    /* Find encoder: */
    for (i = 0; i < res->count_encoders; i++) {
        enc = drmModeGetEncoder(drm_display.fd, res->encoders[i]);
        if (enc->encoder_id == conn->encoder_id)
            break;
        drmModeFreeEncoder(enc);
        enc = NULL;
    }

    if (enc) {
        drm_display.crtc_id = enc->crtc_id;
    }

    for (i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == drm_display.crtc_id) {
            drm_display.crtc_index = i;
            break;
        }
    }

    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    /* Set atomic */
    drmSetClientCap(drm_display.fd, DRM_CLIENT_CAP_ATOMIC, 1);

    /* Get Plane resources */
    plane_res = drmModeGetPlaneResources(drm_display.fd);
    if (!plane_res) {
        printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
        return -1;
    }

    for (i = 0; (i < plane_res->count_planes) && !found_primary; i++) {
        uint32_t id = plane_res->planes[i];
        drmModePlanePtr plane = drmModeGetPlane(drm_display.fd, id);
        if (!plane) {
            printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
            continue;
        }

        if (plane->possible_crtcs & (1 << drm_display.crtc_index)) {
            drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(drm_display.fd, id, DRM_MODE_OBJECT_PLANE);

            for (j = 0; j < props->count_props; j++) {
                drmModePropertyPtr p =
                    drmModeGetProperty(drm_display.fd, props->props[j]);

                if ((strcmp(p->name, "type") == 0) &&
                    (props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)) {
                    found_primary = 1;
                    drm_display.primary_id = id;
                }

                drmModeFreeProperty(p);
            }

            drmModeFreeObjectProperties(props);
}

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(plane_res);

    printf("Find connector_id %d, crtc_id %d, primary plane id %d\n", drm_display.connector_id,
           drm_display.crtc_id, drm_display.primary_id);

    return 0;
}

static void kms_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    DisplayChangeListener *dcl;
    int idx;

    if (display_init() < 0) {
        error_report("kms: init failed");
        exit(1);
    }

    for (idx = 0;; idx++) {
        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        dcl = g_new0(DisplayChangeListener, 1);
        dcl->ops = &kms_ops;
        dcl->con = con;
        register_displaychangelistener(dcl);
    }

}

static QemuDisplay qemu_display_kms = {
    .type       = DISPLAY_TYPE_KMS,
    .init       = kms_init,
};

static void register_kms(void)
{
    qemu_display_register(&qemu_display_kms);
}

type_init(register_kms);
