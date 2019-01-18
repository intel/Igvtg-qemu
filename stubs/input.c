#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/input.h"

InputLibinput *input_libinput_init(QemuConsole *con,
                                   struct udev *udev, const char *seat,
                                   Error **errp)
{
    error_setg(errp, "libinput support not available");
    return NULL;
}

void input_libinput_exit(InputLibinput *il)
{
}
