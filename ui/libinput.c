/*
 * Use libinput for guest keyboard/mouse/tablet input.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "sysemu/runstate.h"
#include "ui/input.h"
#include "ui/logind.h"
#include "ui/kbd-state.h"

#include <sys/ioctl.h>
#include <libudev.h>
#include <libinput.h>
#include "standard-headers/linux/input.h"

struct InputLibinput {
    const char *seat;
    struct udev *udev;
    struct libinput *ctx;
    QKbdState *kbd;
    input_libinput_hook hook;
    void *hook_opaque;
    int device_count;
    int error_count;
    int events;
    fd_set fds;
    int fdmax;
};

static int open_direct(const char *path, int flags, void *user_data)
{
    InputLibinput *il = user_data;
    int fd;

    fd = open(path, flags);
    if (fd < 0) {
        error_report("open %s: %s", path, strerror(errno));
        il->error_count++;
        return fd;
    }

    ioctl(fd, EVIOCGRAB, 1);
    qemu_set_nonblock(fd);
    il->device_count++;
    FD_SET(fd, &il->fds);
    if (il->fdmax < fd)
        il->fdmax = fd;
    return fd;
}

static void close_direct(int fd, void *user_data)
{
    InputLibinput *il = user_data;

    FD_CLR(fd, &il->fds);
    il->device_count--;
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
}

static const struct libinput_interface interface_direct = {
    .open_restricted  = open_direct,
    .close_restricted = close_direct,
};

static int open_logind(const char *path, int flags, void *user_data)
{
    InputLibinput *il = user_data;
    int fd;

    fd = logind_open(path);
    if (fd < 0) {
        il->error_count++;
        return fd;
    }

    il->device_count++;
    return fd;
}

static void close_logind(int fd, void *user_data)
{
    InputLibinput *il = user_data;

    il->device_count--;
    close(fd);
}

static const struct libinput_interface interface_logind = {
    .open_restricted  = open_logind,
    .close_restricted = close_logind,
};

static void input_libinput_kbd_event(InputLibinput *il,
                                     struct libinput_event_keyboard *kbd)
{
    bool down = libinput_event_keyboard_get_key_state(kbd);
    int lcode = libinput_event_keyboard_get_key(kbd);
    int qcode = qemu_input_linux_to_qcode(lcode);
    bool handled = false;

    if (il->hook) {
        handled = il->hook(il->kbd, qcode, down, il->hook_opaque);
    }
    if (handled && down) {
        return;
    }

    qkbd_state_key_event(il->kbd, qcode, down);
    il->events++;
}

static void input_libinput_ptr_btn_event(InputLibinput *il,
                                         struct libinput_event_pointer *ptr)
{
    bool down = libinput_event_pointer_get_button_state(ptr);
    int lcode = libinput_event_pointer_get_button(ptr);
    InputButton btn;

    switch (lcode) {
    case BTN_LEFT:
        btn = INPUT_BUTTON_LEFT;
        break;
    case BTN_RIGHT:
        btn = INPUT_BUTTON_RIGHT;
        break;
    case BTN_MIDDLE:
        btn = INPUT_BUTTON_MIDDLE;
        break;
    case BTN_GEAR_UP:
        btn = INPUT_BUTTON_WHEEL_UP;
        break;
    case BTN_GEAR_DOWN:
        btn = INPUT_BUTTON_WHEEL_DOWN;
        break;
    case BTN_SIDE:
        btn = INPUT_BUTTON_SIDE;
        break;
    case BTN_EXTRA:
        btn = INPUT_BUTTON_EXTRA;
        break;
    default:
        return;
    };

    qemu_input_queue_btn(NULL, btn, down);
    il->events++;
}

static void input_libinput_ptr_rel_event(InputLibinput *il,
                                         struct libinput_event_pointer *ptr)
{
    int dx = libinput_event_pointer_get_dx(ptr);
    int dy = libinput_event_pointer_get_dy(ptr);

    if (dx) {
        qemu_input_queue_rel(NULL, INPUT_AXIS_X, dx);
        il->events++;
    }
    if (dy) {
        qemu_input_queue_rel(NULL, INPUT_AXIS_Y, dy);
        il->events++;
    }
}

static void input_libinput_ptr_abs_event(InputLibinput *il,
                                         struct libinput_event_pointer *ptr)
{
    int x = libinput_event_pointer_get_absolute_x_transformed(ptr, 0xffff);
    int y = libinput_event_pointer_get_absolute_y_transformed(ptr, 0xffff);

    qemu_input_queue_abs(NULL, INPUT_AXIS_X, x, 0, 0xffff);
    il->events++;
    qemu_input_queue_abs(NULL, INPUT_AXIS_Y, y, 0, 0xffff);
    il->events++;
}

static void input_libinput_event(void *opaque)
{
    InputLibinput *il = opaque;
    struct libinput_event *evt;
    struct libinput_event_keyboard *kbd;
    struct libinput_event_pointer *ptr;

    libinput_dispatch(il->ctx);
    while ((evt = libinput_get_event(il->ctx)) != NULL) {
        switch (libinput_event_get_type(evt)) {
        case LIBINPUT_EVENT_KEYBOARD_KEY:
            kbd = libinput_event_get_keyboard_event(evt);
            input_libinput_kbd_event(il, kbd);
            break;
        case LIBINPUT_EVENT_POINTER_BUTTON:
            ptr = libinput_event_get_pointer_event(evt);
            input_libinput_ptr_btn_event(il, ptr);
            break;
        case LIBINPUT_EVENT_POINTER_MOTION:
            ptr = libinput_event_get_pointer_event(evt);
            input_libinput_ptr_rel_event(il, ptr);
            break;
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
            ptr = libinput_event_get_pointer_event(evt);
            input_libinput_ptr_abs_event(il, ptr);
            break;
        default:
            /* ignore */
            break;
        }
        libinput_event_destroy(evt);
    }
    if (il->events) {
        il->events = 0;
        qemu_input_event_sync();
    }
}

InputLibinput *input_libinput_init_udev(QemuConsole *con,
                                        struct udev *udev, const char *seat,
                                        Error **errp)
{
    const struct libinput_interface *intf = &interface_direct;
    InputLibinput *il;

    il = g_new0(InputLibinput, 1);
    il->seat = seat;
    il->udev = udev;

    if (logind_init() == 0) {
        intf = &interface_logind;
    }
    il->ctx = libinput_udev_create_context(intf, il, il->udev);
    libinput_udev_assign_seat(il->ctx, il->seat);
    if (il->error_count || !il->device_count) {
        error_setg(errp, "libinput: init failed (%d devs ok, %d devs failed)",
                   il->device_count, il->error_count);
        libinput_unref(il->ctx);
        g_free(il);
        return NULL;
    }
    qemu_set_fd_handler(libinput_get_fd(il->ctx),
                        input_libinput_event, NULL, il);

    il->kbd = qkbd_state_init(con);
    return il;
}

InputLibinput *input_libinput_init_path(QemuConsole *con,
                                        Error **errp)
{
    const struct libinput_interface *intf = &interface_direct;
    InputLibinput *il;

    il = g_new0(InputLibinput, 1);
    il->ctx = libinput_path_create_context(intf, il);

    qemu_set_fd_handler(libinput_get_fd(il->ctx),
                        input_libinput_event, NULL, il);

    il->kbd = qkbd_state_init(con);
    return il;
}

void *input_libinput_path_add_device(InputLibinput *il, const char *path,
                                     Error **errp)
{
    struct libinput_device *dev;

    dev = libinput_path_add_device(il->ctx, path);
    if (dev == NULL) {
        error_setg(errp, "libinput: open %s failed", path);
    }
    return dev;
}

void input_libinput_path_del_device(InputLibinput *il, void *dev)
{
    libinput_path_remove_device(dev);
}

void input_libinput_path_set_grab(InputLibinput *il, bool enable)
{
    int fd;

    for (fd = 0; fd <= il->fdmax; fd++) {
        if (FD_ISSET(fd, &il->fds)) {
            ioctl(fd, EVIOCGRAB, enable ? 1 : 0);
        }
    }
}

void input_libinput_set_hook(InputLibinput *il,
                             input_libinput_hook hook,
                             void *opaque)
{
    il->hook = hook;
    il->hook_opaque = opaque;
}

void input_libinput_exit(InputLibinput *il)
{
    libinput_unref(il->ctx);
    g_free(il);
}
