#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/drm.h"

/* ----------------------------------------------------------------------- */
/* xorg xserver drm lease support code                                     */

#include <X11/X.h>
#include <xcb/randr.h>

#ifdef CONFIG_XCB_RANDR

typedef struct xserver_t {
    xcb_connection_t *conn;
    xcb_window_t root;

    const xcb_query_extension_reply_t *ext_r;

    xcb_randr_output_t randr_output;
    xcb_randr_crtc_t randr_crtc;
    xcb_randr_lease_t randr_lease;
    xcb_randr_mode_info_t randr_mode;
    xcb_randr_get_screen_resources_reply_t *gsr_r;
} xserver_t;

static const char *core_err[] = {
    [Success]     = "Success",
    [BadRequest]  = "BadRequest",
    [BadValue]    = "BadValue",
    [BadWindow]   = "BadWindow",
    [BadPixmap]   = "BadPixmap",
    [BadAtom]     = "BadAtom",
    [BadCursor]   = "BadCursor",
    [BadFont]     = "BadFont",
    [BadMatch]    = "BadMatch",
    [BadDrawable] = "BadDrawable",
    [BadAccess]   = "BadAccess",
    [BadAlloc]    = "BadAlloc",
    [BadColor]    = "BadColor",
    [BadGC]       = "BadGC",
    [BadIDChoice] = "BadIDChoice",
    [BadName]     = "BadName",
    [BadLength]   = "BadLength",
    [BadImplementation] = "BadImplementation",
};

static const char *xrandr_err[] = {
    [XCB_RANDR_BAD_OUTPUT]   = "BadOutput",
    [XCB_RANDR_BAD_CRTC]     = "BadCRTC",
    [XCB_RANDR_BAD_MODE]     = "BadMode",
    [XCB_RANDR_BAD_PROVIDER] = "BadProvider",
};

static char *xserver_error_msg(xserver_t *x,
                               xcb_generic_error_t *error)
{
    const char *name;

    if (error->error_code >= x->ext_r->first_error &&
        error->error_code < x->ext_r->first_error + ARRAY_SIZE(xrandr_err)) {
        name = xrandr_err[error->error_code - x->ext_r->first_error];
    } else if (error->error_code < ARRAY_SIZE(core_err)) {
        name = core_err[error->error_code];
    } else {
        name = "unknown";
    }
    return g_strdup_printf("%s (code %d, op %d:%d)", name, error->error_code,
                           error->major_code, error->minor_code);
}

static xcb_randr_output_t xserver_find_output(xserver_t *x,
                                              const char *output_name)
{
    xcb_randr_get_output_info_cookie_t goi_c;
    xcb_randr_get_output_info_reply_t *goi_r;
    xcb_randr_output_t output = XCB_NONE;
    xcb_randr_output_t *ro;
    char *name;
    int o, len;

    ro = xcb_randr_get_screen_resources_outputs(x->gsr_r);
    for (o = 0; o < x->gsr_r->num_outputs; o++) {
        goi_c = xcb_randr_get_output_info(x->conn, ro[o], x->gsr_r->config_timestamp);
        goi_r = xcb_randr_get_output_info_reply(x->conn, goi_c, NULL);
        name = (char*)xcb_randr_get_output_info_name(goi_r);
        len = xcb_randr_get_output_info_name_length(goi_r);
        if (output_name == NULL || strncmp(name, output_name, len) == 0) {
            output = ro[o];
        }
        free(goi_r);
    }
    return output;
}

static xcb_randr_crtc_t xserver_find_crtc(xserver_t *x, xcb_randr_output_t output)
{
    xcb_randr_crtc_t *rc = xcb_randr_get_screen_resources_crtcs(x->gsr_r);
    xcb_randr_get_crtc_info_cookie_t gci_c;
    xcb_randr_get_crtc_info_reply_t *gci_r;
    int c;

    for (c = 0; c < x->gsr_r->num_crtcs; c++) {
        gci_c = xcb_randr_get_crtc_info(x->conn, rc[c], x->gsr_r->config_timestamp);
        gci_r = xcb_randr_get_crtc_info_reply(x->conn, gci_c, NULL);
        if (gci_r->mode) {
            int num_outputs = xcb_randr_get_crtc_info_outputs_length(gci_r);
            xcb_randr_output_t *outputs = xcb_randr_get_crtc_info_outputs(gci_r);
            if (num_outputs == 1 && outputs[0] == output) {
                return rc[c];
            }
        } else {
            int num_possible = xcb_randr_get_crtc_info_possible_length(gci_r);
            xcb_randr_output_t *possible = xcb_randr_get_crtc_info_possible(gci_r);
            for (int p = 0; p < num_possible; p++) {
                if (possible[p] == output) {
                    return rc[c];
                }
            }
        }
        free(gci_r);
    }
    return XCB_NONE;
}

static void xserver_lease_setup(xserver_t *x, const char *output_name,
                                Error **errp)
{
    const xcb_setup_t *setup;
    xcb_screen_iterator_t iter;
    xcb_generic_error_t *error;
    xcb_randr_query_version_cookie_t ver_c;
    xcb_randr_query_version_reply_t *ver_r;
    xcb_randr_get_screen_resources_cookie_t gsr_c;
    xcb_randr_output_t output;
    xcb_randr_crtc_t crtc;
    int screen;

    x->conn = xcb_connect(NULL, &screen);
    if (!x->conn) {
        error_setg(errp, "drm-lease: can not connect to X server");
        return;
    }

    /* find root window */
    setup = xcb_get_setup(x->conn);
    for (iter = xcb_setup_roots_iterator(setup); iter.rem; xcb_screen_next(&iter)) {
        if (screen == 0) {
            x->root = iter.data->root;
            break;
        }
        screen--;
    }

    /* check for xrandr extension */
    x->ext_r = xcb_get_extension_data(x->conn, &xcb_randr_id);
    if (!x->ext_r->present) {
        error_setg(errp, "drm-lease: xrandr extension not present");
        return;
    }

    ver_c = xcb_randr_query_version(x->conn,
                                    XCB_RANDR_MAJOR_VERSION,
                                    XCB_RANDR_MINOR_VERSION);
    ver_r = xcb_randr_query_version_reply(x->conn, ver_c, &error);
    if (!ver_r) {
        error_setg(errp, "drm-lease: xrandr version query error");
        return;
    }
    info_report("drm-lease: xrandr: client v%d.%x, server v%d.%x",
                XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION,
                ver_r->major_version, ver_r->minor_version);
    free(ver_r);

    /* Get RandR resources */
    gsr_c = xcb_randr_get_screen_resources(x->conn, x->root);
    x->gsr_r = xcb_randr_get_screen_resources_reply(x->conn, gsr_c, &error);
    if (!x->gsr_r) {
        error_setg(errp, "drm-lease: xrandr: can not get screen resources");
        return;
    }

    output = xserver_find_output(x, output_name);
    if (output == XCB_NONE) {
        error_setg(errp, "drm-lease: xrandr: no output found");
        return;
    }
    info_report("drm-lease: xrandr: using output 0x%x", output);

    crtc = xserver_find_crtc(x, output);
    if (crtc == XCB_NONE) {
        error_setg(errp, "drm-lease: xrandr: crtc not found");
        return;
    }
    info_report("drm-lease: xrandr: using crtc 0x%x", crtc);

    /* Generate RandR lease id */
    x->randr_lease = xcb_generate_id(x->conn);
    x->randr_output = output;
    x->randr_crtc = crtc;
}

static int xserver_make_lease(xserver_t *x, Error **errp)
{
    xcb_randr_create_lease_cookie_t cl_c;
    xcb_randr_create_lease_reply_t *cl_r;
    xcb_generic_error_t *error;
    char *msg;
    int *rcl_f;
    int fd = -1;

    cl_c = xcb_randr_create_lease(x->conn, x->root,
                                  x->randr_lease,
                                  1, 1,
                                  &x->randr_crtc,
                                  &x->randr_output);
    cl_r = xcb_randr_create_lease_reply(x->conn, cl_c, &error);
    if (!cl_r) {
        msg = xserver_error_msg(x, error);
        error_setg(errp, "drm-lease: xrandr: create lease failed: %s", msg);
        g_free(msg);
        return -1;
    }

    if (cl_r->nfd > 0) {
        rcl_f = xcb_randr_create_lease_reply_fds(x->conn, cl_r);
        fd = rcl_f[0];
    }
    free (cl_r);

    if (fd < 0) {
        error_setg(errp, "drm-lease: xrandr: lease returned invalid fd");
        return -1;
    }

    return fd;
}

#else /* CONFIG_XCB_RANDR */

typedef struct xserver_t {
} xserver_t;

static void xserver_lease_setup(xserver_t *x, const char *output_name,
                                Error **errp)
{
    error_setg(errp, "drm-lease: xrandr: compiled without lease support");
}

static int xserver_make_lease(xserver_t *x, Error **errp)
{
    error_setg(errp, "drm-lease: xrandr: compiled without lease support");
    return -1;
}

#endif /* CONFIG_XCB_RANDR */

/* ----------------------------------------------------------------------- */
/* interface for the rest of qemu                                          */

int drm_lease_xserver(const char *output, Error **errp)
{
    xserver_t *x = g_new0(xserver_t, 1);
    int fd;

    xserver_lease_setup(x, output, errp);
    if (*errp)
        return -1;

    fd = xserver_make_lease(x, errp);
    if (*errp)
        return -1;

    return fd;
}
