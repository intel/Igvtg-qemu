#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/input.h"

InputLibinput *input_libinput_init_udev(QemuConsole *con,
                                        struct udev *udev, const char *seat,
                                        Error **errp)
{
    error_setg(errp, "libinput support not available");
    return NULL;
}

InputLibinput *input_libinput_init_path(QemuConsole *con,
                                        Error **errp)
{
    error_setg(errp, "libinput support not available");
    return NULL;
}

void *input_libinput_path_add_device(InputLibinput *il, const char *path,
                                     Error **errp)
{
    error_setg(errp, "libinput support not available");
    return NULL;
}

void input_libinput_path_del_device(InputLibinput *il, void *dev)
{
}

void input_libinput_path_set_grab(InputLibinput *il, bool enable)
{
}

void input_libinput_set_hook(InputLibinput *il,
                             input_libinput_hook hook,
                             void *opaque)
{
}

void input_libinput_exit(InputLibinput *il)
{
}
