#ifndef QEMU_UI_DRM_H
#define QEMU_UI_DRM_H

#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct QemuDRMDisplay QemuDRMDisplay;
typedef struct QemuDRMFramebuffer QemuDRMFramebuffer;

struct QemuDRMDisplay {
    const char *seat;
    struct udev *udev;

    /* qemu */
    Notifier exit;
    DisplayChangeListener dcl;
    DisplaySurface *ds;
    InputLibinput *il;

    /* drm */
    struct udev_device *device;
    int fd;
    drmModeConnector *conn;
    char cname[32];
    drmModeEncoder *enc;
    drmModeCrtc *saved_crtc;
    drmModeModeInfo *mode;
    QemuDRMFramebuffer *dumb;
    QemuDRMFramebuffer *cursor;
};

struct QemuDRMFramebuffer {
    QemuDRMDisplay *drm;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t fourcc;
    uint64_t modifier;

    uint32_t handle;
    uint32_t fbid;

    /* dumb fb */
    uint32_t size;
    void *mem;
    pixman_image_t *image;
};

/* ui/drm.c */
QemuDRMFramebuffer *drm_fb_alloc(QemuDRMDisplay *drm,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t fourcc,
                                 uint64_t modifier);
void drm_fb_print(QemuDRMFramebuffer *fb, const char *prefix);
void drm_fb_addfb(QemuDRMFramebuffer *fb, Error **errp);
void drm_fb_show(QemuDRMFramebuffer *fb, Error **errp);
void drm_fb_destroy(QemuDRMFramebuffer *fb);

void drm_dcl_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h);
void drm_dcl_switch(DisplayChangeListener *dcl,
                    DisplaySurface *surface);
void drm_dcl_refresh(DisplayChangeListener *dcl);
void drm_dcl_mouse_set(DisplayChangeListener *dcl,
                       int x, int y, int visible);
void drm_dcl_cursor_define(DisplayChangeListener *dcl,
                           QEMUCursor *c);

#endif /* QEMU_UI_DRM_H */
