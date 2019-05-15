/*
 * DRM (linux kernel mode setting) user interface.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu-common.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/qemu-pixman.h"
#include "ui/kbd-state.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "ui/drm.h"
#include "standard-headers/drm/drm_fourcc.h"

#include <libudev.h>

#define DEFAULT_SEAT "seat0"

/* ----------------------------------------------------------------------- */

static const char *conn_type[] = {
    /*
     * Names are taken from the xorg modesetting driver, so the output
     * names are matching the ones ypu can see with xrandr.
     */
    [DRM_MODE_CONNECTOR_Unknown]     = "None",
    [DRM_MODE_CONNECTOR_VGA]         = "VGA",
    [DRM_MODE_CONNECTOR_DVII]        = "DVI-I",
    [DRM_MODE_CONNECTOR_DVID]        = "DVI-D",
    [DRM_MODE_CONNECTOR_DVIA]        = "DVI-A",
    [DRM_MODE_CONNECTOR_Composite]   = "Composite",
    [DRM_MODE_CONNECTOR_SVIDEO]      = "SVIDEO",
    [DRM_MODE_CONNECTOR_LVDS]        = "LVDS",
    [DRM_MODE_CONNECTOR_Component]   = "Component",
    [DRM_MODE_CONNECTOR_9PinDIN]     = "DIN",
    [DRM_MODE_CONNECTOR_DisplayPort] = "DP",
    [DRM_MODE_CONNECTOR_HDMIA]       = "HDMI",
    [DRM_MODE_CONNECTOR_HDMIB]       = "HDMI-B",
    [DRM_MODE_CONNECTOR_TV]          = "TV",
    [DRM_MODE_CONNECTOR_eDP]         = "eDP",
    [DRM_MODE_CONNECTOR_VIRTUAL]     = "Virtual",
    [DRM_MODE_CONNECTOR_DSI]         = "DSI",
    [DRM_MODE_CONNECTOR_DPI]         = "DPI",
};

static void drm_conn_name(drmModeConnector *conn, char *dest, int dlen)
{
    const char *type;

    if (conn->connector_type_id < ARRAY_SIZE(conn_type) &&
        conn_type[conn->connector_type]) {
        type = conn_type[conn->connector_type];
    } else {
        type = "unknown";
    }
    snprintf(dest, dlen, "%s-%d", type, conn->connector_type_id);
}

static void drm_conn_find(QemuDRMDisplay *drm, char *output)
{
    drmModeRes *res;
    drmModeConnector *conn;
    char cname[32];
    int i;

    res = drmModeGetResources(drm->fd);
    if (res == NULL) {
        error_report("drm: drmModeGetResources() failed");
        return;
    }
    for (i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm->fd, res->connectors[i]);
        if (!conn ||
            conn->connection != DRM_MODE_CONNECTED ||
            !conn->count_modes) {
            continue;
        }
        if (output) {
            drm_conn_name(conn, cname, sizeof(cname));
            if (strcmp(cname, output) != 0) {
                continue;
            }
        }
        drm->conn = conn;
        drm_conn_name(drm->conn, drm->cname, sizeof(drm->cname));
        return;
    }
}

static void drm_conn_list(QemuDRMDisplay *drm)
{
    drmModeRes *res;
    drmModeConnector *conn;
    char cname[32];
    int i;

    res = drmModeGetResources(drm->fd);
    if (res == NULL) {
        return;
    }
    fprintf(stderr, "available drm connectors:\n");
    for (i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm->fd, res->connectors[i]);
        if (!conn) {
            continue;
        }
        drm_conn_name(conn, cname, sizeof(cname));
        if (conn->connection != DRM_MODE_CONNECTED) {
            fprintf(stderr, "  %s : not connected\n", cname);
        } else if (!conn->count_modes) {
            fprintf(stderr, "  %s : no video modes\n", cname);
        } else {
            fprintf(stderr, "  %s : %dx%d\n", cname,
                    conn->modes[0].hdisplay,
                    conn->modes[0].vdisplay);
        }
    }
}

static drmModeModeInfo *drm_mode_find(QemuDRMDisplay *drm,
                                      int width, int height)
{
    int i;

    for (i = 0; i < drm->conn->count_modes; i++) {
        if (drm->conn->modes[i].hdisplay == width &&
            drm->conn->modes[i].vdisplay == height) {
            return &drm->conn->modes[i];
        }
    }
    return NULL;
}

static void drm_mode_init(QemuDRMDisplay *drm, char *modename)
{
    drmModeModeInfo *mode = &drm->conn->modes[0];
    unsigned int width, height;

    if (modename) {
        if (sscanf(modename, "%ux%u", &width, &height) == 2) {
            mode = drm_mode_find(drm, width, height);
        }
    }
    drm->mode = mode;
}

/* ----------------------------------------------------------------------- */

void drm_fb_destroy(QemuDRMFramebuffer *fb)
{
    struct drm_mode_destroy_dumb destroy = {
        .handle = fb->handle,
    };

#if defined(CONFIG_OPENGL_DMABUF)
    if (fb->gbm_bo) {
        gbm_bo_destroy(fb->gbm_bo);
        destroy.handle = 0;
    }
#endif
    if (fb->image) {
        pixman_image_unref(fb->image);
    }
    if (fb->mem) {
        munmap(fb->mem, fb->size);
    }
    if (fb->fbid) {
        drmModeRmFB(fb->drm->fd, fb->fbid);
    }
    if (destroy.handle) {
        drmIoctl(fb->drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    g_free(fb);
}

QemuDRMFramebuffer *drm_fb_alloc(QemuDRMDisplay *drm,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t fourcc,
                                 uint64_t modifier)
{
    QemuDRMFramebuffer *fb;

    fb = g_new0(QemuDRMFramebuffer, 1);
    fb->drm = drm;
    fb->width = width;
    fb->height = height;
    fb->fourcc = fourcc;
    fb->modifier = modifier;
    return fb;
}

void drm_fb_print(QemuDRMFramebuffer *fb, const char *prefix)
{
    fprintf(stderr, "%s: fb %dx%d, stride %d, format %c%c%c%c, "
            "modifier %d,0x%" PRIx64 " | handle %d, fbid %d\n",
            prefix,
            fb->width, fb->height, fb->stride,
            (fb->fourcc >>  0) & 0xff,
            (fb->fourcc >>  8) & 0xff,
            (fb->fourcc >> 16) & 0xff,
            (fb->fourcc >> 24) & 0xff,
            (int)(fb->modifier >> 56),
            (uint64_t)(fb->modifier & 0x00ffffffffffffffLL),
            fb->handle, fb->fbid);
}

void drm_fb_addfb(QemuDRMFramebuffer *fb, Error **errp)
{
    uint32_t handles[4] = { fb->handle };
    uint32_t strides[4] = { fb->stride };
    uint32_t offsets[4] = { 0 };
    uint64_t modifiers[4] = { fb->modifier };
    int rc;

    rc = drmModeAddFB2WithModifiers(fb->drm->fd,
                                    fb->width, fb->height, fb->fourcc,
                                    handles, strides, offsets,
                                    modifiers,
                                    &fb->fbid,
                                    fb->modifier ? DRM_MODE_FB_MODIFIERS : 0);
    if (rc < 0) {
        drm_fb_print(fb, "addfb error");
        error_setg_errno(errp, errno,
                         "drm: drmModeAddFB2WithModifiers() failed");
    }
}

static QemuDRMFramebuffer *drm_fb_create_dumb(QemuDRMDisplay *drm,
                                              int width, int height,
                                              Error **errp)
{
    struct drm_mode_create_dumb creq = {
        .width  = width,
        .height = height,
        .bpp = 32,
    };
    struct drm_mode_map_dumb mreq = {};
    QemuDRMFramebuffer *fb;
    int rc;

    fb = drm_fb_alloc(drm, width, height, DRM_FORMAT_XRGB8888,
                      DRM_FORMAT_MOD_LINEAR);

    rc = drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (rc < 0) {
        error_setg_errno(errp, errno, "drm: DRM_IOCTL_MODE_CREATE_DUMB");
        drm_fb_destroy(fb);
        return NULL;
    }
    fb->stride = creq.pitch;
    fb->handle = creq.handle;
    fb->size   = creq.size;

    mreq.handle = fb->handle;
    rc = drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (rc < 0) {
        error_setg_errno(errp, errno, "drm: DRM_IOCTL_MODE_MAP_DUMB");
        drm_fb_destroy(fb);
        return NULL;
    }

    fb->mem = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   drm->fd, mreq.offset);
    if (fb->mem == MAP_FAILED) {
        error_setg_errno(errp, errno, "drm: mmap()");
        drm_fb_destroy(fb);
        return NULL;
    }

    fb->image = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                         fb->width,
                                         fb->height,
                                         fb->mem,
                                         fb->stride);

    drm_fb_addfb(fb, errp);
    if (*errp) {
        drm_fb_destroy(fb);
        return NULL;
    }

    return fb;
}

static void drm_fb_clear(QemuDRMFramebuffer *fb)
{
    pixman_color_t darkgray = {
        .red   = 0x1000,
        .green = 0x1000,
        .blue  = 0x1000,
    };
    pixman_image_t *bg = pixman_image_create_solid_fill(&darkgray);

    pixman_image_composite(PIXMAN_OP_SRC, bg, NULL, fb->image,
                           0, 0, 0, 0, 0, 0,
                           fb->width, fb->height);
    pixman_image_unref(bg);
}

void drm_fb_show(QemuDRMFramebuffer *fb, Error **errp)
{
    QemuDRMDisplay *drm = fb->drm;
    drmModeModeInfo *mode;
    int rc;

    mode = drm_mode_find(fb->drm, fb->width, fb->height);
    rc = drmModeSetCrtc(drm->fd, drm->enc->crtc_id,
                        fb->fbid, 0, 0,
                        &drm->conn->connector_id, 1,
                        mode ?: drm->mode);
    if (rc < 0) {
        error_setg_errno(errp, errno, "drm: drmModeSetCrtc()");
    }
}

/* ----------------------------------------------------------------------- */

static void drm_ui_info(QemuDRMDisplay *drm)
{
    QemuUIInfo info = {
        .width = drm->mode->hdisplay,
        .height = drm->mode->vdisplay,
    };

    dpy_set_ui_info(drm->dcl.con, &info);
}

void drm_dcl_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);
    int xoff = (drm->dumb->width - surface_width(drm->ds)) / 2;
    int yoff = (drm->dumb->height - surface_height(drm->ds)) / 2;

    pixman_image_composite(PIXMAN_OP_SRC,
                           drm->ds->image, NULL, drm->dumb->image,
                           x, y, x, y,
                           x + xoff, y + yoff, w, h);
    drmModeDirtyFB(drm->fd, drm->dumb->fbid, 0, 0);
}

void drm_dcl_switch(DisplayChangeListener *dcl,
                    DisplaySurface *surface)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);

    if (!drm->ds || !surface ||
        surface_width(drm->ds) != surface_width(surface) ||
        surface_height(drm->ds) != surface_height(surface)) {
        /* resize -> clear screen */
        drm_fb_clear(drm->dumb);
    }

    drm->ds = surface;
    drm_dcl_update(dcl, 0, 0,
                   surface_width(drm->ds),
                   surface_height(drm->ds));
}

void drm_dcl_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

void drm_dcl_mouse_set(DisplayChangeListener *dcl,
                       int x, int y, int visible)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);

    if (!visible) {
        drmModeSetCursor(drm->fd, drm->enc->crtc_id, 0, 0, 0);
    }
    drmModeMoveCursor(drm->fd, drm->enc->crtc_id, x, y);
}

void drm_dcl_cursor_define(DisplayChangeListener *dcl,
                           QEMUCursor *c)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);
    pixman_image_t *cimg;
    Error *local_err = NULL;

    if (drm->cursor && (drm->cursor->width != c->width ||
                        drm->cursor->height != c->height)) {
        drm_fb_destroy(drm->cursor);
        drm->cursor = NULL;
    }
    if (!drm->cursor) {
        drm->cursor = drm_fb_create_dumb(drm, c->width, c->height,
                                         &local_err);
        if (local_err) {
            error_report_err(local_err);
            return;
        }
    }

    cimg = pixman_image_create_bits(PIXMAN_a8r8g8b8,
                                    c->width, c->height,
                                    c->data, c->width * 4);
    pixman_image_composite(PIXMAN_OP_SRC,
                           cimg, NULL, drm->cursor->image,
                           0, 0, 0, 0, 0, 0,
                           c->width, c->height);
    pixman_image_unref(cimg);

    drmModeSetCursor2(drm->fd, drm->enc->crtc_id, drm->cursor->handle,
                      c->width, c->height, c->hot_x, c->hot_y);
}

static const DisplayChangeListenerOps drm_dcl_ops = {
    .dpy_name          = "drm",
    .dpy_gfx_update    = drm_dcl_update,
    .dpy_gfx_switch    = drm_dcl_switch,
    .dpy_refresh       = drm_dcl_refresh,
    .dpy_mouse_set     = drm_dcl_mouse_set,
    .dpy_cursor_define = drm_dcl_cursor_define,
};

static void drm_display_restore_crtc(QemuDRMDisplay *drm)
{
    if (!drm->saved_crtc) {
        return;
    }

    drmModeSetCrtc(drm->fd,
                   drm->saved_crtc->crtc_id,
                   drm->saved_crtc->buffer_id,
                   drm->saved_crtc->x,
                   drm->saved_crtc->y,
                   &drm->conn->connector_id, 1,
                   &drm->saved_crtc->mode);
    drmModeSetCursor(drm->fd, drm->enc->crtc_id, 0, 0, 0);
}

static void drm_display_exit_notifier(struct Notifier *n, void *data)
{
    QemuDRMDisplay *drm = container_of(n, QemuDRMDisplay, exit);

    drm_display_restore_crtc(drm);
}

static bool drm_display_input_hook(QKbdState *state, int qcode,
                                   bool down, void *opaque)
{
    if (qkbd_state_modifier_get(state, QKBD_MOD_CTRL) &&
        qkbd_state_modifier_get(state, QKBD_MOD_ALT) &&
        down) {
        /* ctrl-alt-<hotkey> */
        if (qcode == Q_KEY_CODE_BACKSPACE) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
            return true;
        }
    }
    return false;
}

static void drm_display_init(DisplayState *ds, DisplayOptions *o)
{
    Error **errp = &error_fatal;
    QemuDRMDisplay *drm;
    QemuConsole *con = qemu_console_lookup_by_index(0);
    struct udev_enumerate *uenum;
    struct udev_list_entry *ulist, *uentry;
    bool use_libinput = true;

    drm = g_new0(QemuDRMDisplay, 1);
    drm->seat = getenv("XDG_SEAT");
    if (!drm->seat) {
        drm->seat = DEFAULT_SEAT;
    }

    /* find & open drm device */
    drm->udev = udev_new();
    uenum = udev_enumerate_new(drm->udev);
    udev_enumerate_add_match_subsystem(uenum, "drm");
    udev_enumerate_add_match_tag(uenum, "seat");
    udev_enumerate_scan_devices(uenum);
    ulist = udev_enumerate_get_list_entry(uenum);
    udev_list_entry_foreach(uentry, ulist) {
        const char *path = udev_list_entry_get_name(uentry);
        struct udev_device *device =
            udev_device_new_from_syspath(drm->udev, path);
        const char *node = udev_device_get_devnode(device);
        const char *seat = udev_device_get_property_value(device, "ID_SEAT");
        if (!node) {
            continue;
        }
        if (strcmp(seat ?: DEFAULT_SEAT, drm->seat) != 0) {
            continue;
        }
        drm->device = device;
        break;
    }
    drm->fd = open(udev_device_get_devnode(drm->device), O_RDWR);
    if (drm->fd < 0) {
        error_setg_errno(errp, errno, "drm: open %s",
                         udev_device_get_devnode(drm->device));
        goto err_free_drm;
    }

    drm_conn_find(drm, o->u.drm.has_output ? o->u.drm.output : NULL);
    if (!drm->conn) {
        drm_conn_list(drm);
        error_setg(errp, "drm: no useable connector found");
        goto err_close_drm;
    }
    drm->enc = drmModeGetEncoder(drm->fd, drm->conn->encoder_id);
    if (!drm->enc) {
        error_setg(errp, "drm: no useable encoder found");
        goto err_close_drm;
    }
    drm->saved_crtc = drmModeGetCrtc(drm->fd, drm->enc->crtc_id);
    drm_mode_init(drm, o->u.drm.has_mode ? o->u.drm.mode : NULL);

    if (o->u.drm.has_libinput) {
        use_libinput = o->u.drm.libinput;
    }
    if (use_libinput) {
        drm->il = input_libinput_init_udev(con, drm->udev, drm->seat, errp);
        if (!drm->il) {
            goto err_close_drm;
        }
        input_libinput_set_hook(drm->il, drm_display_input_hook, drm);
    }

    if (o->has_gl && o->gl != DISPLAYGL_MODE_OFF) {
#if defined(CONFIG_OPENGL_DMABUF)
        if (drm_egl_init(drm, o, errp) < 0) {
            goto err_close_drm;
        }
        drm->dcl.ops = &drm_egl_dcl_ops;
#else
        error_setg(errp, "drm: compiled without opengl support");
#endif
    } else {
        drm->dcl.ops = &drm_dcl_ops;
    }

    drm->dumb = drm_fb_create_dumb(drm,
                                   drm->mode->hdisplay,
                                   drm->mode->vdisplay,
                                   errp);
    if (!drm->dumb) {
        goto err_libinput_exit;
    }
    drm_fb_show(drm->dumb, errp);
    if (*errp) {
        goto err_destroy_dumb;
    }

    drm->exit.notify = drm_display_exit_notifier;
    qemu_add_exit_notifier(&drm->exit);
    drm->dcl.con = con;
    register_displaychangelistener(&drm->dcl);
    drm_ui_info(drm);
    return;

err_destroy_dumb:
    drm_fb_destroy(drm->dumb);
err_libinput_exit:
    input_libinput_exit(drm->il);
err_close_drm:
    close(drm->fd);
err_free_drm:
    g_free(drm);
    return;
}

static void early_drm_display_init(DisplayOptions *opts)
{
#if defined(CONFIG_OPENGL_DMABUF)
    if (opts->has_gl && opts->gl != DISPLAYGL_MODE_OFF) {
        display_opengl = 1;
    }
#endif
}

static QemuDisplay qemu_display_drm = {
    .type       = DISPLAY_TYPE_DRM,
    .init       = drm_display_init,
    .early_init = early_drm_display_init,
};

static void register_drm(void)
{
    qemu_display_register(&qemu_display_drm);
}

type_init(register_drm);
