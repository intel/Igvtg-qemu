#include "ui/drm-lease.h"

static void xserver_lease_init(window_manager_t *wm) {
	wm->wm_opaque = malloc(sizeof(xserver_t));
}

static int xserver_lease_setup(window_manager_t *wm) {
	xserver_t *x = wm->wm_opaque;
	x->name = "Xorg";

	int screen;
	x->conn = xcb_connect(NULL, &screen);

	if (!x->conn) {
	    fprintf(stderr, "Cannot connect to X server\n");
	    return -1;
	}

	const xcb_setup_t *setup = xcb_get_setup(x->conn);

	/* Find out root window */
	xcb_screen_iterator_t iter;
	for (iter = xcb_setup_roots_iterator(setup); iter.rem; xcb_screen_next(&iter)) {
	    if (screen == 0) {
		x->root = iter.data->root;
		break;
	    }
	    --screen;
	}

	printf("root window %x\n", x->root);

	/* Get RandR resources */
	xcb_randr_get_screen_resources_cookie_t gsr_c = xcb_randr_get_screen_resources(x->conn, x->root);
	xcb_generic_error_t *error;
	x->gsr_r = xcb_randr_get_screen_resources_reply(x->conn, gsr_c, &error);
	if (!x->gsr_r) {
	    printf("Cannot get screen resources\n");
	    return -1;
	}

	/* Pick a output */
	xcb_randr_output_t *ro = xcb_randr_get_screen_resources_outputs(x->gsr_r);
	int num_outputs = x->gsr_r->num_outputs;
	int o;
	xcb_randr_output_t output = XCB_NONE;

	for (o = 0; o < num_outputs; o++) {
	    xcb_randr_get_output_info_cookie_t goi_c = xcb_randr_get_output_info(x->conn, ro[o], x->gsr_r->config_timestamp);
	    xcb_randr_get_output_info_reply_t *goi_r = xcb_randr_get_output_info_reply(x->conn, goi_c, NULL);

	    uint8_t *output_name = xcb_randr_get_output_info_name(goi_r);
	    unsigned int output_name_length = xcb_randr_get_output_info_name_length(goi_r);

	    if (output_name_length == strlen(wm->output_name) &&
		memcmp(output_name, wm->output_name, output_name_length) == 0)
	    {
		    output = ro[o];
		    break;
	    }
	}

	if (!output) {
	    fprintf(stderr, "%s: no such output\n", wm->output_name);
	    return -1;
	}

	printf("Output %x\n", output);

	xcb_randr_crtc_t crtc = XCB_NONE;

	/* Pick a crtc */
	if (crtc == XCB_NONE) {
	    xcb_randr_crtc_t    *rc = xcb_randr_get_screen_resources_crtcs(x->gsr_r);

	    xcb_randr_crtc_t    idle_crtc = XCB_NONE;
	    xcb_randr_crtc_t    active_crtc = XCB_NONE;

	    /* Find either a crtc already connected to the desired output or idle */
	    for (int c = 0; active_crtc == XCB_NONE && c < x->gsr_r->num_crtcs; c++) {
		xcb_randr_get_crtc_info_cookie_t gci_c = xcb_randr_get_crtc_info(x->conn, rc[c], x->gsr_r->config_timestamp);

		xcb_randr_get_crtc_info_reply_t *gci_r = xcb_randr_get_crtc_info_reply(x->conn, gci_c, NULL);

		if (gci_r->mode) {
			int num_outputs = xcb_randr_get_crtc_info_outputs_length(gci_r);
			xcb_randr_output_t *outputs = xcb_randr_get_crtc_info_outputs(gci_r);
			for (int o = 0; o < num_outputs; o++)
				if (outputs[o] == output && num_outputs == 1) {
					active_crtc = rc[c];
					break;
				}

		} else if (idle_crtc == 0) {
			int num_possible = xcb_randr_get_crtc_info_possible_length(gci_r);
			xcb_randr_output_t *possible = xcb_randr_get_crtc_info_possible(gci_r);
			for (int p = 0; p < num_possible; p++)
				if (possible[p] == output) {
					idle_crtc = rc[c];
					break;
				}
		}

		free(gci_r);
	    }
	    if (active_crtc)
		crtc = active_crtc;
	    else
		crtc = idle_crtc;
	}

	if (crtc == XCB_NONE) {
	    printf("Cannot find usable CRTC\n");
	    return -1;
	}

	printf("CRTC %x\n", crtc);

	/* Generate RandR lease id */
	xcb_randr_lease_t randr_lease = xcb_generate_id(x->conn);

	x->randr_output = output;
	x->randr_crtc = crtc;
	x->randr_lease = randr_lease;

	return 0;
}

static int xserver_make_lease(window_manager_t *wm, lease_t *lease) {
	xserver_t *x = wm->wm_opaque;
	xcb_generic_error_t *error;

	xcb_randr_create_lease_cookie_t cl_c = xcb_randr_create_lease(x->conn, x->root, x->randr_lease, 1, 1, &x->randr_crtc, &x->randr_output);

	xcb_randr_create_lease_reply_t *cl_r = xcb_randr_create_lease_reply(x->conn, cl_c, &error);
	if (!cl_r) {
	    printf ("Create lease failed\n");
	    return -1;
	}

	int fd = -1;
	if (cl_r->nfd > 0) {
		int *rcl_f = xcb_randr_create_lease_reply_fds(x->conn, cl_r);

		fd = rcl_f[0];
	}
	free (cl_r);

	if (fd < 0) {
		printf("Lease returned invalid fd\n");
		return -1;
	}

	lease->fd = fd;
	return 0;
}

static void xserver_free_lease(window_manager_t *wm, lease_t *lease) {
	xserver_t *x = wm->wm_opaque;

	if (x->randr_lease) {
	    xcb_randr_free_lease(x->conn, x->randr_lease, 0);
	    x->randr_lease = 0;
	}
	
	if (lease->fd >= 0) {
	    close(lease->fd);
	    lease->fd = -1;
	}
}

static void xserver_release(window_manager_t *wm) {
	xserver_t *x = wm->wm_opaque;
	xcb_disconnect(x->conn);
	free(wm->wm_opaque);
}

wm_lease_ops xserver_lease_ops = {
	.init = xserver_lease_init,
	.setup = xserver_lease_setup,
	.make_lease = xserver_make_lease,
	.free_lease = xserver_free_lease,
	.release = xserver_release
};

int prepare_drm_lease(const char *wm_name, window_manager_t *wm) {
	int ret = -1;

	if (!strcmp(wm_name, "Xorg")) {
	    wm->ops = &xserver_lease_ops;
	    fprintf(stderr, "Window Manager: Xorg\n");
	}

	wm->ops->init(wm);
	ret = wm->ops->setup(wm);

	return ret;
}

