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
#include "sysemu/sysemu.h"
#include "ui/drm.h"
#include "ui/egl-context.h"
#include "ui/egl-helpers.h"
#include "standard-headers/drm/drm_fourcc.h"

static const char *glmode_name[] = {
    [QEMU_GL_RENDER_SURFACE] = "render-surface",
    [QEMU_GL_DIRECT_DMABUF]  = "direct-dmabuf",
    [QEMU_GL_RENDER_DMABUF]  = "render-dmabuf",
};

/* ----------------------------------------------------------------------- */

static void drm_egl_make_current(QemuDRMDisplay *drm)
{
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   qemu_egl_rn_ctx);
}

static void drm_egl_release_dmabuf(QemuDmaBuf *dmabuf)
{
    egl_dmabuf_release_texture(dmabuf);
    close(dmabuf->fd);
    g_free(dmabuf);
}

static QemuDmaBuf *drm_egl_export_texture(int id, int width, int height)
{
    EGLint stride, fourcc;
    EGLuint64KHR modifier;
    QemuDmaBuf *dmabuf;

    dmabuf = g_new0(QemuDmaBuf, 1);
    dmabuf->width = width;
    dmabuf->height = height;
    dmabuf->fd = egl_get_fd_for_texture(id, &stride, &fourcc, &modifier);
    if (dmabuf->fd < 0) {
        g_free(dmabuf);
        return NULL;
    }
    dmabuf->stride = stride;
    dmabuf->fourcc = fourcc;
    dmabuf->modifier = modifier;

    return dmabuf;
}

static QemuDRMFramebuffer *drm_egl_import_dmabuf(QemuDRMDisplay *drm,
                                                 QemuDmaBuf *dmabuf,
                                                 Error **errp)
{
    struct gbm_import_fd_modifier_data import = {
        .width      = dmabuf->width,
        .height     = dmabuf->height,
        .format     = dmabuf->fourcc,
        .modifier   = dmabuf->modifier,
        .num_fds    = 1,
        .fds[0]     = dmabuf->fd,
        .strides[0] = dmabuf->stride,
    };
    QemuDRMFramebuffer *fb;

    fb = drm_fb_alloc(drm, dmabuf->width, dmabuf->height,
                      dmabuf->fourcc, dmabuf->modifier);

    fb->gbm_bo = gbm_bo_import(drm->gbm_dev,
                               GBM_BO_IMPORT_FD_MODIFIER,
                               &import,
                               GBM_BO_USE_SCANOUT);
    if (!fb->gbm_bo) {
        error_setg(errp, "drm: gbm_bo_import() failed");
        drm_fb_destroy(fb);
        return NULL;
    }
    fb->stride = gbm_bo_get_stride(fb->gbm_bo);
    fb->handle = gbm_bo_get_handle(fb->gbm_bo).u32;

    drm_fb_addfb(fb, errp);
    if (*errp) {
        drm_fb_destroy(fb);
        return NULL;
    }

    return fb;
}

static void drm_egl_show_dmabuf(QemuDRMDisplay *drm,
                                QemuDmaBuf *dmabuf,
                                Error **errp)
{
    QemuDRMFramebuffer *fb;

    fb = drm_egl_import_dmabuf(drm, dmabuf, errp);
    if (*errp) {
        return;
    }
    assert(fb != NULL);

    drm_fb_print(fb, __func__);
    drm_fb_show(fb, errp);
    if (*errp) {
        drm_fb_destroy(fb);
        return;
    }

    if (drm->gbm_fb) {
        drm_fb_destroy(drm->gbm_fb);
    }
    drm->gbm_fb = fb;
}

static QemuDRMFramebuffer *drm_egl_fb_create(QemuDRMDisplay *drm,
                                             int width, int height,
                                             Error **errp)
{
    QemuDRMFramebuffer *fb;

    fb = drm_fb_alloc(drm, width, height, DRM_FORMAT_XRGB8888,
                      DRM_FORMAT_MOD_LINEAR);

    fb->gbm_bo = gbm_bo_create(drm->gbm_dev,
                               width, height,
                               DRM_FORMAT_XRGB8888,
                               GBM_BO_USE_RENDERING |
                               GBM_BO_USE_SCANOUT);
    if (!fb->gbm_bo) {
        error_report("drm: gbm_bo_create() failed");
        drm_fb_destroy(fb);
        return NULL;
    }
    fb->stride = gbm_bo_get_stride(fb->gbm_bo);
    fb->handle = gbm_bo_get_handle(fb->gbm_bo).u32;

    drm_fb_addfb(fb, errp);
    if (*errp) {
        drm_fb_destroy(fb);
        return NULL;
    }

    return fb;
}

static QemuDmaBuf *drm_egl_fb_export(QemuDRMFramebuffer *fb)
{
    QemuDmaBuf *dmabuf;

    dmabuf = g_new0(QemuDmaBuf, 1);
    dmabuf->width = fb->width;
    dmabuf->height = fb->height;
    dmabuf->stride = fb->stride;
    dmabuf->fourcc = fb->fourcc;
    dmabuf->modifier = fb->modifier;
    dmabuf->fd = gbm_bo_get_fd(fb->gbm_bo);
    if (dmabuf->fd < 0) {
        g_free(dmabuf);
        return NULL;
    }

    return dmabuf;
}

static void drm_egl_setup_blit_fb(QemuDRMDisplay *drm,
                                  int width, int height,
                                  Error **errp)
{
    QemuDRMFramebuffer *fb;
    QemuDmaBuf *dmabuf;

    if (drm->blit_fb.width == width &&
        drm->blit_fb.height == height) {
        return;
    }

    egl_fb_destroy(&drm->blit_fb);

    fb = drm_egl_fb_create(drm, width, height, errp);
    if (*errp) {
        return;
    }
    assert(fb != NULL);

    drm_fb_print(fb, __func__);
    dmabuf = drm_egl_fb_export(fb);
    if (!dmabuf) {
        error_setg(errp, "drm: drm_egl_fb_export() failed");
        goto err_fb;
    }
    egl_dmabuf_print(dmabuf, __func__);

    egl_dmabuf_import_texture(dmabuf);
    if (!dmabuf->texture) {
        error_setg(errp, "drm: egl_dmabuf_import_texture() failed");
        goto err_dmabuf;
    }
    egl_fb_setup_for_tex(&drm->blit_fb, width, height,
                         dmabuf->texture, false);

    drm_fb_show(fb, errp);
    if (*errp) {
        goto err_dmabuf;
    }

    if (drm->gbm_fb) {
        drm_fb_destroy(drm->gbm_fb);
    }
    drm->gbm_fb = fb;

    if (drm->blit) {
        drm_egl_release_dmabuf(drm->blit);
    }
    drm->blit = dmabuf;
    return;

err_dmabuf:
    drm_egl_release_dmabuf(dmabuf);
err_fb:
    drm_fb_destroy(fb);
}

static void drm_egl_set_mode(QemuDRMDisplay *drm, QemuGLMode glmode)
{
    Error *local_err = NULL;

    if (drm->glmode == glmode) {
        return;
    }

    fprintf(stderr, "%s: %s -> %s\n", __func__,
            glmode_name[drm->glmode],
            glmode_name[glmode]);
    drm->glmode = glmode;

    switch (drm->glmode) {
    case QEMU_GL_RENDER_SURFACE:
        egl_fb_destroy(&drm->blit_fb);
        drm_fb_destroy(drm->gbm_fb);
        drm->gbm_fb = NULL;
        drm_fb_show(drm->dumb, &local_err);
        break;
    case QEMU_GL_RENDER_DMABUF:
    case QEMU_GL_DIRECT_DMABUF:
        /* nothing */
        break;
    }
}

static void drm_egl_update(QemuDRMDisplay *drm)
{
    if (drm->glmode == QEMU_GL_RENDER_DMABUF) {
        drm_egl_make_current(drm);
        egl_fb_blit(&drm->blit_fb, &drm->guest_fb, drm->blit_flip);
    }
    if (drm->glmode == QEMU_GL_DIRECT_DMABUF) {
        drmModeDirtyFB(drm->fd, drm->gbm_fb->fbid, 0, 0);
    }
}

/* ----------------------------------------------------------------------- */

static void drm_egl_dcl_refresh(DisplayChangeListener *dcl)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);

    graphic_hw_update(dcl->con);
    drm_egl_update(drm);
}

static QEMUGLContext drm_egl_dcl_create_context(DisplayChangeListener *dcl,
                                                QEMUGLParams *params)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);

    drm_egl_make_current(drm);
    return qemu_egl_create_context(dcl, params);
}

static void drm_egl_dcl_scanout_disable(DisplayChangeListener *dcl)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);

    fprintf(stderr, "%s:\n", __func__);

    drm_egl_set_mode(drm, QEMU_GL_RENDER_SURFACE);
}

static void drm_egl_dcl_scanout_texture(DisplayChangeListener *dcl,
                                        uint32_t backing_id,
                                        bool backing_y_0_top,
                                        uint32_t backing_width,
                                        uint32_t backing_height,
                                        uint32_t x, uint32_t y,
                                        uint32_t w, uint32_t h)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);
    Error *local_err = NULL;
    QemuDmaBuf *dmabuf;

    fprintf(stderr, "%s: %dx%d (rect %dx%d+%d+%d, id %d)\n", __func__,
            backing_width, backing_height, w, h, x, y, backing_id);

    drm_egl_make_current(drm);
    if (!drm->enable_direct) {
        goto indirect;
    }

    dmabuf = drm_egl_export_texture(backing_id,
                                    backing_width,
                                    backing_height);
    if (!dmabuf) {
        goto indirect;
    }

    drm_egl_show_dmabuf(drm, dmabuf, &local_err);
    if (local_err) {
        if (1 /* debug */) {
            error_report_err(local_err);
            fprintf(stderr, "%s: try fallback to indirect\n", __func__);
        } else {
            error_free(local_err);
        }
        local_err = NULL;
        drm_egl_release_dmabuf(dmabuf);
        goto indirect;
    }
    drm_egl_set_mode(drm, QEMU_GL_DIRECT_DMABUF);

    if (drm->guest) {
        drm_egl_release_dmabuf(drm->guest);
    }
    drm->guest = dmabuf;
    return;

indirect:
    drm_egl_setup_blit_fb(drm, backing_width, backing_height, &local_err);
    if (local_err) {
        error_report_err(local_err);
        return;
    }

    egl_fb_setup_for_tex(&drm->guest_fb,
                         backing_width, backing_height, backing_id, false);

    drm->blit_flip = backing_y_0_top;
    drm_egl_set_mode(drm, QEMU_GL_RENDER_DMABUF);
}

static void drm_egl_dcl_scanout_dmabuf(DisplayChangeListener *dcl,
                                       QemuDmaBuf *dmabuf)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);
    Error *local_err = NULL;

    egl_dmabuf_print(dmabuf, __func__);

    drm_egl_make_current(drm);
    if (!drm->enable_direct) {
        goto indirect;
    }

    drm_egl_show_dmabuf(drm, dmabuf, &local_err);
    if (local_err) {
        if (1 /* debug */) {
            error_report_err(local_err);
            fprintf(stderr, "%s: try fallback to indirect\n", __func__);
        } else {
            error_free(local_err);
        }
        local_err = NULL;
        goto indirect;
    }
    drm_egl_set_mode(drm, QEMU_GL_DIRECT_DMABUF);
    return;

indirect:
    drm_egl_setup_blit_fb(drm, dmabuf->width, dmabuf->height, &local_err);
    if (local_err) {
        error_report_err(local_err);
        return;
    }

    egl_dmabuf_import_texture(dmabuf);
    egl_fb_setup_for_tex(&drm->guest_fb,
                         dmabuf->width, dmabuf->height,
                         dmabuf->texture, false);

    drm->blit_flip = false;
    drm_egl_set_mode(drm, QEMU_GL_RENDER_DMABUF);
}

static void drm_egl_dcl_cursor_dmabuf(DisplayChangeListener *dcl,
                                      QemuDmaBuf *dmabuf, bool have_hot,
                                      uint32_t hot_x, uint32_t hot_y)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);
    QemuDRMFramebuffer *fb;
    Error *local_err = NULL;

    if (!dmabuf) {
        drmModeSetCursor(drm->fd, drm->enc->crtc_id, 0, 0, 0);
        if (drm->gbm_cursor) {
            drm_fb_destroy(drm->gbm_cursor);
            drm->gbm_cursor = NULL;
        }
        return;
    }

    fb = drm_egl_import_dmabuf(drm, dmabuf, &local_err);
    if (local_err) {
        error_report_err(local_err);
        return;
    }
    assert(fb != NULL);

    if (have_hot) {
        fprintf(stderr, "%s: set (%dx%d, hotspot %+d%+d)\n", __func__,
                dmabuf->width, dmabuf->height, hot_x, hot_y);
        drmModeSetCursor2(drm->fd, drm->enc->crtc_id, fb->handle,
                          dmabuf->width, dmabuf->height, hot_x, hot_y);
    } else {
        fprintf(stderr, "%s: set (%dx%d, no hotspot)\n", __func__,
                dmabuf->width, dmabuf->height);
        drmModeSetCursor2(drm->fd, drm->enc->crtc_id, fb->handle,
                          dmabuf->width, dmabuf->height, 0, 0);
    }

    if (drm->gbm_cursor) {
        drm_fb_destroy(drm->gbm_cursor);
    }
    drm->gbm_cursor = fb;
}

static void drm_egl_dcl_release_dmabuf(DisplayChangeListener *dcl,
                                       QemuDmaBuf *dmabuf)
{
    egl_dmabuf_release_texture(dmabuf);
}

static void drm_egl_dcl_cursor_position(DisplayChangeListener *dcl,
                                        uint32_t pos_x, uint32_t pos_y)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);

    if (drm->gbm_cursor) {
        drmModeMoveCursor(drm->fd, drm->enc->crtc_id, pos_x, pos_y);
    }
}

static void drm_egl_dcl_update(DisplayChangeListener *dcl,
                               uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h)
{
    QemuDRMDisplay *drm = container_of(dcl, QemuDRMDisplay, dcl);

    drm_egl_update(drm);
}

const DisplayChangeListenerOps drm_egl_dcl_ops = {
    .dpy_name                = "drm-egl",
    .dpy_gfx_update          = drm_dcl_update,
    .dpy_gfx_switch          = drm_dcl_switch,
    .dpy_refresh             = drm_egl_dcl_refresh,
    .dpy_mouse_set           = drm_dcl_mouse_set,
    .dpy_cursor_define       = drm_dcl_cursor_define,

    .dpy_gl_ctx_create       = drm_egl_dcl_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,
    .dpy_gl_ctx_get_current  = qemu_egl_get_current_context,

    .dpy_gl_scanout_disable  = drm_egl_dcl_scanout_disable,
    .dpy_gl_scanout_texture  = drm_egl_dcl_scanout_texture,
    .dpy_gl_scanout_dmabuf   = drm_egl_dcl_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = drm_egl_dcl_cursor_dmabuf,
    .dpy_gl_cursor_position  = drm_egl_dcl_cursor_position,
    .dpy_gl_release_dmabuf   = drm_egl_dcl_release_dmabuf,
    .dpy_gl_update           = drm_egl_dcl_update,
};

int drm_egl_init(QemuDRMDisplay *drm, DisplayOptions *opts, Error **errp)
{
    fprintf(stderr, "%s:\n", __func__);

    drm->gbm_dev = gbm_create_device(drm->fd);
    if (!drm->gbm_dev) {
        error_setg(errp, "drm: gbm_create_device failed");
        return -1;
    }

    if (egl_rendernode_init(NULL, opts->gl) < 0) {
        error_setg(errp, "drm: egl initialization failed");
        return -1;
    }

    if (opts->u.drm.has_direct) {
        drm->enable_direct = opts->u.drm.direct;
    } else {
        drm->enable_direct = true;
    }

    drm->gls = qemu_gl_init_shader();
    return 0;
}
