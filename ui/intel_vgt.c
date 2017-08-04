/*
 * QEMU Intel GVT-g indirect display support
 *
 * Copyright (c) Intel
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#define __user
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "qemu/osdep.h"
#include <SDL.h>
#include <SDL_syswm.h>

#include <libudev.h>

#include "sysemu/sysemu.h"
#include "qemu-common.h"
#include "ui/console.h"
#include "hw/xen/xen.h"

#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <hw/vfio/vfio-common.h>
#include <sys/ioctl.h>
#include <linux/vfio.h>

/* we hard code the vgpuid to 1
 * we use the vgpuid to check the vm's display ready event once getting
 * the event switching to use intel vgpu display before that we use the
 * emulated grahics card. This may not needed while using other display
 * method such as spice.
 */
int vgpuid = 1;

static GLuint textureId = 0;
static GLuint cursortextureId = 0;
static int fbWidth = 0;
static int fbHeight = 0;

static int winWidth = 1024;
static int winHeight = 768;

const int PRIMARY_LIST_LEN = 6;
const int CURSOR_LIST_LEN = 4;

static uint32_t current_cursor_fb_addr = 0;
static GLuint current_textureId;
static GLuint current_cursor_textureId;
static bool cursor_ready = false;

static int kvmgt_fd = 0;

typedef struct buffer_rec{
    uint32_t start;
    GLuint textureId;
    int age;
    uint8_t tiled;
    uint32_t size;
    int fd;
} buffer_rec;

typedef struct buffer_list{
    struct buffer_rec *l;
    int len;
} buffer_list;

struct buffer_list primary_list;
struct buffer_list cursor_list;

static EGLDisplay dpy;
static EGLContext ctx;
static EGLSurface sur;

PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

static int kvmgt_fd_ioctl(int fd, int type, ...)
{
    int ret;
    void *arg;
    va_list ap;

    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);

    ret = ioctl(fd, type, arg);
    if (ret < 0) {
        ret = -errno;
    }

    return ret;
}

static void drawRect(void)
{
    /*
     * The framebuffer is top-down flipped.
     * Set flipped texture coords to correct it.
     */

    glBegin(GL_QUADS);
    glTexCoord2f(0, 1);  glVertex3f(-1, -1, 0);
    glTexCoord2f(1, 1);  glVertex3f(1, -1, 0);
    glTexCoord2f(1, 0);  glVertex3f(1, 1, 0);
    glTexCoord2f(0, 0);  glVertex3f(-1, 1, 0);
    glEnd();
}

static void draw(int x, int y)
{
    float fx, fy;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();

    /* Draw the primary plane in a rectangle. */
    glBindTexture(GL_TEXTURE_2D, current_textureId);
    glDisable(GL_BLEND);
    drawRect();

    if (cursor_ready) {
        /* Calcuate the cursor position. */
        fx = -1 + 2 * (float)(x + 32) / (float)fbWidth;
        fy = 1 - 2 * (float)(y + 32) / (float)fbHeight;
        glTranslatef(fx, fy, 0);
        glScalef(64 / (float)fbWidth, 64 / (float)fbHeight, 1);

        /* Draw the cursor plane in another rectangle */
        glBindTexture(GL_TEXTURE_2D, current_cursor_textureId);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        drawRect();
    }

    glFlush();
}

/* new window size or exposure */
static void reshape(int width, int height)
{
    glViewport(0, 0, width, height);
}

static int find_rec(struct buffer_list *l, int fd)
{
    int i, r;

    r = -1;
    for (i = 0; i < l->len; i++) {
        if (l->l[i].fd == fd) {
            r = i;
            break;
        }
    }

    return r;
}

static int oldest_rec(struct buffer_list *l)
{
    int i = 1, a = l->l[0].age, r = 0;

    for (i = 1; i < l->len; i++) {
        if (l->l[i].age > a) {
            a = l->l[i].age;
            r = i;
        }
    }

    return r;
}

static void age_list(struct buffer_list *l)
{
    int i;

    for (i = 0; i < l->len; i++) {
        if (l->l[i].age != INT_MAX) {
            l->l[i].age++;
        }
    }
}

static void create_cursor_buffer(int *x, int *y)
{
    struct vfio_device_gfx_plane_info dmabuf;

    int r, ret;
    EGLImageKHR namedimage;
    unsigned long name;

    glGenTextures(1, &cursortextureId);
    glBindTexture(GL_TEXTURE_2D, cursortextureId);

    current_cursor_textureId = cursortextureId;

    memset(&dmabuf, 0, sizeof(struct vfio_device_gfx_plane_info));
    dmabuf.argsz = sizeof(dmabuf);
    dmabuf.drm_plane_type = DRM_PLANE_TYPE_CURSOR;
    dmabuf.flags = VFIO_GFX_PLANE_TYPE_DMABUF;
    ret = kvmgt_fd_ioctl(kvmgt_fd, VFIO_DEVICE_QUERY_GFX_PLANE, &dmabuf);
    if (ret != 0 ) {
	current_cursor_fb_addr = 0;
	cursor_ready = false;
	printf("cursor create buffer failed:%d\r", ret);
        return;
    }
    cursor_ready = true;
    name = dmabuf.fd;
    *x = dmabuf.x_pos;
    *y = dmabuf.y_pos;
    r = find_rec(&cursor_list, name);

    age_list(&cursor_list);
    if (r >= 0) {
        cursor_list.l[r].age = 0;
        current_cursor_textureId = cursor_list.l[r].textureId;
	return;
    }

    r = oldest_rec(&cursor_list);
    if ((cursor_list.l[r].fd > 0) && (cursor_list.l[r].fd != name)) {
	printf("close cursor fd:%d/n", cursor_list.l[r].fd);
        close(cursor_list.l[r].fd);
    }

    cursor_list.l[r].start = current_cursor_fb_addr;
    cursor_list.l[r].textureId = cursortextureId;
    cursor_list.l[r].age = 0;
    cursor_list.l[r].tiled = 0;
    cursor_list.l[r].size = dmabuf.size;
    cursor_list.l[r].fd = dmabuf.fd;

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);

    EGLint attribs[] = {
        EGL_WIDTH, dmabuf.width,
        EGL_HEIGHT, dmabuf.height,
        EGL_LINUX_DRM_FOURCC_EXT,
        dmabuf.drm_format > 0 ? dmabuf.drm_format : DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, (int)name,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, dmabuf.stride,
        EGL_NONE
    };
    namedimage = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                   NULL, attribs);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, namedimage);
    eglDestroyImageKHR(dpy, namedimage);
}

#if 0
static void dump_gfx_plane_type(void)
{
    struct vfio_device_gfx_plane_info dmabuf;
    int ret;
    memset(&dmabuf, 0, sizeof(struct vfio_device_gfx_plane_info));
    dmabuf.argsz = sizeof(dmabuf);
    dmabuf.flags = VFIO_GFX_PLANE_TYPE_PROBE | VFIO_GFX_PLANE_TYPE_DMABUF;
    ret = kvmgt_fd_ioctl(kvmgt_fd, VFIO_DEVICE_QUERY_GFX_PLANE, &dmabuf);
    if (ret != 0 ) {
        printf("cannot dump gfx_plane_info ret:%d\n", ret);
        return;
    }
    printf("gfx_plane_info is dmabuf\n");
}
#endif

static int create_primary_buffer(void)
{
    struct vfio_device_gfx_plane_info dmabuf;
    EGLImageKHR namedimage;
    unsigned long name;
    int ret;
    int r;

    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);


    current_textureId = textureId;

    memset(&dmabuf, 0, sizeof(struct vfio_device_gfx_plane_info));
    dmabuf.argsz = sizeof(dmabuf);
    dmabuf.drm_plane_type = DRM_PLANE_TYPE_PRIMARY;
    dmabuf.flags = VFIO_GFX_PLANE_TYPE_DMABUF;
    ret = kvmgt_fd_ioctl(kvmgt_fd, VFIO_DEVICE_QUERY_GFX_PLANE, &dmabuf);
    if (ret != 0 ) {
	printf("primary create buffer failed using ioctl:%d\n", ret);
        return -1;
    }
    name = dmabuf.fd;
    r = find_rec(&primary_list, name);
    age_list(&primary_list);
    if (r >= 0) {
        primary_list.l[r].age = 0;
        current_textureId = primary_list.l[r].textureId;
	return 0;
    }

    r = oldest_rec(&primary_list);
    if ((primary_list.l[r].fd > 0) && (primary_list.l[r].fd != name)) {
        printf("close primary fd:%d\n", primary_list.l[r].fd);
        close(primary_list.l[r].fd);
    }

    primary_list.l[r].textureId = textureId;
    primary_list.l[r].age = 0;
    primary_list.l[r].size = dmabuf.size;
    primary_list.l[r].fd = dmabuf.fd;
    fbWidth = dmabuf.width;
    fbHeight = dmabuf.height;

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);

    EGLint attribs[] = {
        EGL_WIDTH, dmabuf.width,
        EGL_HEIGHT, dmabuf.height,
        EGL_LINUX_DRM_FOURCC_EXT,
        dmabuf.drm_format > 0 ? dmabuf.drm_format : DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, name,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, dmabuf.stride,
        EGL_NONE
    };
    namedimage = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                       NULL, attribs);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, namedimage);
    eglDestroyImageKHR(dpy, namedimage);

    return 0;
}


/********** UDEV Part ************/
static struct udev * udev;
static struct udev_monitor * mon;

static int udev_init(void) {
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

    ret = udev_monitor_filter_add_match_subsystem_devtype(mon, "drm", NULL);
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

static void udev_destroy(void) {
    udev_monitor_unref(mon);
    udev_unref(udev);
}

static int check_vgt_uevent(void) {
    int ret = 0;
    const char * value;
    struct udev_device * dev;

    dev = udev_monitor_receive_device(mon);
    if (!dev) {
        goto out;
    }

    value = udev_device_get_property_value(dev, "GVT_DISPLAY_READY");
    if (!value || strcmp(value, "1")) {
        goto out;
    }
    value = udev_device_get_property_value(dev, "VMID");
    if (!value || 1 != sscanf(value, "%d", &ret) || ret != vgpuid) {
        goto out;
    }
    ret = 1;
out:
    udev_device_unref(dev);

    return ret;
}

/********** SDL Part ************/
static DisplayChangeListener *dcl;
/*
extern int last_vm_running;

void sdl_update_caption(void);
*/
void handle_keydown(SDL_Event *ev);
void handle_keyup(SDL_Event *ev);
void handle_mousemotion(SDL_Event *ev);
void handle_mousebutton(SDL_Event *ev);
void handle_activation(SDL_Event *ev);

static void intel_vgt_refresh(DisplayChangeListener *dcl)
{
    SDL_Event event;
    int x =0, y = 0;
    //int ret;
/*
    if (last_vm_running != runstate_is_running()) {
        last_vm_running = runstate_is_running();
        sdl_update_caption();
    }
*/
    SDL_EnableUNICODE(!qemu_console_is_graphic(NULL));

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_VIDEORESIZE:
            SDL_SetVideoMode(event.resize.w, event.resize.h, 16,
                             SDL_OPENGL | SDL_RESIZABLE);
            reshape(event.resize.w, event.resize.h);
            break;
        case SDL_KEYDOWN:
            handle_keydown(&event);
            break;
        case SDL_KEYUP:
            handle_keyup(&event);
            break;
        case SDL_QUIT:
            if (!no_quit) {
                no_shutdown = 0;
                qemu_system_shutdown_request();
            }
            break;
        case SDL_MOUSEMOTION:
            handle_mousemotion(&event);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            handle_mousebutton(&event);
            break;
        case SDL_ACTIVEEVENT:
            handle_activation(&event);
            break;
        default:
            break;
        }
    }

    if ( create_primary_buffer() == -1)
	return ;

    create_cursor_buffer(&x, &y);
    draw(x, y);
    eglSwapBuffers(dpy, sur);
}

static void vgt_init(void)
{
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
    eglGetProcAddress("glEGLImageTargetTexture2DOES");
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
    eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
    eglGetProcAddress("eglDestroyImageKHR");
    int i;
    EGLint attribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT, /* may be changed later */
    EGL_RED_SIZE, 1,
    EGL_GREEN_SIZE, 1,
    EGL_BLUE_SIZE, 1,
    EGL_DEPTH_SIZE, 1,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_NONE
    };
    EGLint num_conf;
    SDL_SysWMinfo info;
    EGLConfig conf;

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_SetVideoMode(winWidth, winHeight, 32, SDL_OPENGL | SDL_RESIZABLE);

    SDL_VERSION(&info.version);
    SDL_GetWMInfo(&info);

    eglBindAPI(EGL_OPENGL_API);
    dpy = eglGetDisplay(info.info.x11.display);
    eglInitialize(dpy, NULL, NULL);

    eglChooseConfig(dpy, attribs, &conf, 1, &num_conf);
    ctx = eglCreateContext(dpy, conf, EGL_NO_CONTEXT, NULL);
    sur = eglCreateWindowSurface(dpy, conf, info.info.x11.window, NULL);
    eglMakeCurrent(dpy, sur, sur, ctx);

    primary_list.l = malloc(PRIMARY_LIST_LEN*sizeof(struct buffer_rec));
    if (primary_list.l == NULL) {
        fprintf(stderr, "allocate primary list failed\n");
        exit(1);
    }
    primary_list.len = PRIMARY_LIST_LEN;
    cursor_list.l = malloc(CURSOR_LIST_LEN*sizeof(struct buffer_rec));
    if (cursor_list.l == NULL) {
        fprintf(stderr, "allocate cursor list failed\n");
        exit(1);
    }
    cursor_list.len = CURSOR_LIST_LEN;
    for (i = 0; i < primary_list.len; i++) {
        primary_list.l[i].start = 0;
        primary_list.l[i].textureId = 0;
        primary_list.l[i].age = INT_MAX;
        primary_list.l[i].fd = 0;
        primary_list.l[i].size = 0;
        primary_list.l[i].tiled = 0;
    }
    for (i = 0; i < cursor_list.len; i++) {
        cursor_list.l[i].start = 0;
        cursor_list.l[i].textureId = 0;
        cursor_list.l[i].age = INT_MAX;
        cursor_list.l[i].fd = 0;
        cursor_list.l[i].size = 0;
        cursor_list.l[i].tiled = 0;
    }

    glEnable(GL_TEXTURE_2D);
    glClearColor(0, 0, 0, 0);
    glColor3f(1, 1, 1);

    glMatrixMode(GL_PROJECTION);
    glOrtho(-1, 1, -1, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);

    reshape(winWidth, winHeight);
    }

static void intel_vgt_detect(DisplayChangeListener *dcl);
static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name = "intel_vgt",
    .dpy_refresh = intel_vgt_detect,
};

static const DisplayChangeListenerOps dcl_ops2 = {
    .dpy_name = "intel_vgt2",
    .dpy_refresh = intel_vgt_refresh,
};

static void intel_vgt_detect(DisplayChangeListener *dcl)
{
    if (check_vgt_uevent()) {
        udev_destroy();
        unregister_displaychangelistener(dcl->next.le_next);
        vgt_init();
        dcl->ops = &dcl_ops2;
        kvmgt_fd = vfio_get_dmabuf_device_fd();
        printf("kvmgt: intel ui: get device fd:%d\n", kvmgt_fd);
/*        ret = close(kvmgt_fd);
        printf("kvmgt: intel ui: close mgr fd:%d ret:%d\n", kvmgt_fd, ret);
*/
    }
}

static bool check_egl(void)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    bool ret = true;
    char *egl_ext;

    if (!eglInitialize(d, NULL, NULL)) {
        fprintf(stderr, "eglInitialize failed\n");
        exit(1);
    }
    egl_ext = (char *)eglQueryString(d, EGL_EXTENSIONS);
    if (!strstr(egl_ext, "EGL_KHR_image_base")) {
        fprintf(stderr, "no egl extensions found. Intel GVT-g indirect display will be disabled\n");
        ret = false;
    } else {
        fprintf(stderr, "egl extensions found. Intel GVT-g indirect display will be enabled\n");
        if (strstr(egl_ext, "EGL_EXT_image_dma_buf_import") &&
            strstr(egl_ext, "EGL_MESA_image_dma_buf_export")) {
            fprintf(stderr, "Use dma-buf to get guest framebuffer\n");
        } else {
            fprintf(stderr, "Use flink to get guest framebuffer\n");
        }
    }
    eglTerminate(d);

    return ret;
}

void intel_vgpu_display_init(DisplayState *ds, int full_screen, int no_frame)
{
    sdl_display_init(ds, full_screen, no_frame);
    if (!check_egl()) {
        return;
    }
    udev_init();
    dcl = g_malloc0(sizeof(DisplayChangeListener));
    dcl->ops = &dcl_ops;
    register_displaychangelistener(dcl);
}
