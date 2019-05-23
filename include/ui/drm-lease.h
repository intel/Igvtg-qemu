#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/randr.h>


/* Xorg server corresponding resources */
typedef struct xserver_t {
	const char *name;
	xcb_connection_t *conn;
	xcb_window_t root;

	/* X RandR resources */
	xcb_randr_output_t randr_output;
	xcb_randr_crtc_t randr_crtc;
	xcb_randr_lease_t randr_lease;
	xcb_randr_mode_info_t randr_mode;
	xcb_randr_get_screen_resources_reply_t *gsr_r;
} xserver_t;

/* DRM lease */
typedef struct lease_t {
	int fd;
} lease_t;

typedef struct wm_lease_ops wm_lease_ops;
typedef struct window_manager_t window_manager_t;

/* Global information for various window manager to lease */
typedef struct window_manager_t {
	const char *output_name;
	wm_lease_ops *ops;
	void *wm_opaque;
} window_manager_t;

/* DRM lease opeartions */
typedef struct wm_lease_ops {
	void (*init) (window_manager_t *wm);
	int (*setup) (window_manager_t *wm);
	int (*make_lease) (window_manager_t *wm, lease_t  *lease);
	void (*free_lease) (window_manager_t *wm, lease_t  *lease);
	void (*release) (window_manager_t *wm);
} wm_lease_ops;

/* General function for preparing DRM lease setup with various window managers */
int prepare_drm_lease(const char *wm_name, window_manager_t *wm);

