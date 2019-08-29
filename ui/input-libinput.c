/*
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/config-file.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/bitmap.h"
#include "qapi/qapi-types-ui.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "qom/object_interfaces.h"
#include "sysemu/iothread.h"
#include "block/aio.h"

#include <sys/ioctl.h>
#include "standard-headers/linux/input.h"

#include <libudev.h>

#define TYPE_INPUT_LIBINPUT "input-libinput"
#define INPUT_LIBINPUT(obj) \
    OBJECT_CHECK(InputLibinputObj, (obj), TYPE_INPUT_LIBINPUT)
#define INPUT_LIBINPUT_GET_CLASS(obj) \
    OBJECT_GET_CLASS(InputLibinputClass, (obj), TYPE_INPUT_LIBINPUT)
#define INPUT_LIBINPUT_CLASS(klass) \
    OBJECT_CLASS_CHECK(InputLibinputClass, (klass), TYPE_INPUT_LIBINPUT)

typedef struct InputLibinputObj InputLibinputObj;
typedef struct InputLibinputClass InputLibinputClass;

struct InputLibinputObj {
    Object parent;
    char   *evdev;
    void   *handle;
};

struct InputLibinputClass {
    ObjectClass parent_class;
};

static InputLibinput *libinput;
static DECLARE_BITMAP(hostkeys, Q_KEY_CODE__MAX);
static enum GrabToggleKeys grab_toggle;
static bool grab_request;
static bool grab_active = true;

static bool input_libinput_grab_toggle_hook(QKbdState *state, int qcode,
                                            bool down, void *opaque)
{
    bool toggle = false;
    bool handled = false;

    /*
     * QKbdState has the *guests* view on kbd state, we need the
     * *host* view too as we selectively forward key events to the
     * guest depending on grab_active.  So we have our own tracking
     * here ...
     */
    if (down) {
        set_bit(qcode, hostkeys);
    } else {
        clear_bit(qcode, hostkeys);
    }

    switch (grab_toggle) {
    case GRAB_TOGGLE_KEYS_CTRL_CTRL:
        toggle = (test_bit(Q_KEY_CODE_CTRL, hostkeys) &&
                  test_bit(Q_KEY_CODE_CTRL_R, hostkeys));
        break;
    case GRAB_TOGGLE_KEYS_ALT_ALT:
        toggle = (test_bit(Q_KEY_CODE_ALT, hostkeys) &&
                  test_bit(Q_KEY_CODE_ALT_R, hostkeys));
        break;
    case GRAB_TOGGLE_KEYS_SHIFT_SHIFT:
        toggle = (test_bit(Q_KEY_CODE_SHIFT, hostkeys) &&
                  test_bit(Q_KEY_CODE_SHIFT_R, hostkeys));
        break;
    case GRAB_TOGGLE_KEYS_META_META:
        toggle = (test_bit(Q_KEY_CODE_META_L, hostkeys) &&
                  test_bit(Q_KEY_CODE_META_R, hostkeys));
        break;
    case GRAB_TOGGLE_KEYS_SCROLLLOCK:
        toggle = test_bit(Q_KEY_CODE_SCROLL_LOCK, hostkeys);
        break;
    case GRAB_TOGGLE_KEYS_CTRL_SCROLLLOCK:
        toggle = test_bit(Q_KEY_CODE_SCROLL_LOCK, hostkeys) &&
            (test_bit(Q_KEY_CODE_CTRL, hostkeys) &&
             test_bit(Q_KEY_CODE_CTRL_R, hostkeys));
        break;
    case GRAB_TOGGLE_KEYS__MAX:
        /* avoid gcc error */
        break;
    }
    if (toggle) {
        grab_request = true;
    }

    if (grab_request &&
        find_last_bit(hostkeys, Q_KEY_CODE__MAX) == Q_KEY_CODE__MAX) {
        grab_request = false;
        grab_active = !grab_active;
        input_libinput_path_set_grab(libinput, grab_active);
    }

    if (!grab_active) {
        /* don't forward keys to guest if host owns keyboard */
        handled = true;
    }
    if ((grab_toggle == GRAB_TOGGLE_KEYS_SCROLLLOCK ||
         grab_toggle == GRAB_TOGGLE_KEYS_CTRL_SCROLLLOCK) &&
        qcode == Q_KEY_CODE_SCROLL_LOCK) {
        /* don't forward scroll-lock to guest if used as hotkey  */
        handled = true;
    }

    return handled;
}

static void input_libinput_complete(UserCreatable *uc, Error **errp)
{
    InputLibinputObj *il = INPUT_LIBINPUT(uc);

    if (!il->evdev) {
        error_setg(errp, "evdev not specified");
        return;
    }

    if (!libinput) {
        /* first object */
        libinput = input_libinput_init_path(NULL, errp);
        if (!libinput) {
            return;
        }
        input_libinput_set_hook(libinput,
                                input_libinput_grab_toggle_hook, NULL);
    }

    il->handle = input_libinput_path_add_device(libinput, il->evdev, errp);
    return;
}

static void input_libinput_instance_finalize(Object *obj)
{
    InputLibinputObj *il = INPUT_LIBINPUT(obj);

    input_libinput_path_del_device(libinput, il->handle);
    g_free(il->evdev);
}

static char *input_libinput_get_evdev(Object *obj, Error **errp)
{
    InputLibinputObj *il = INPUT_LIBINPUT(obj);

    return g_strdup(il->evdev);
}

static void input_libinput_set_evdev(Object *obj, const char *value,
                                  Error **errp)
{
    InputLibinputObj *il = INPUT_LIBINPUT(obj);

    if (il->evdev) {
        error_setg(errp, "evdev property already set");
        return;
    }
    il->evdev = g_strdup(value);
}

static int input_libinput_get_grab_toggle(Object *obj, Error **errp)
{
    return grab_toggle;
}

static void input_libinput_set_grab_toggle(Object *obj, int value,
                                           Error **errp)
{
    grab_toggle = value;
}

static void input_libinput_instance_init(Object *obj)
{
    object_property_add_str(obj, "evdev",
                            input_libinput_get_evdev,
                            input_libinput_set_evdev, NULL);
    object_property_add_enum(obj, "grab-toggle", "GrabToggleKeys",
                             &GrabToggleKeys_lookup,
                             input_libinput_get_grab_toggle,
                             input_libinput_set_grab_toggle, NULL);
}

static void input_libinput_class_init(ObjectClass *oc, void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = input_libinput_complete;
}

static const TypeInfo input_libinput_info = {
    .name = TYPE_INPUT_LIBINPUT,
    .parent = TYPE_OBJECT,
    .class_size = sizeof(InputLibinputClass),
    .class_init = input_libinput_class_init,
    .instance_size = sizeof(InputLibinputObj),
    .instance_init = input_libinput_instance_init,
    .instance_finalize = input_libinput_instance_finalize,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void register_types(void)
{
    type_register_static(&input_libinput_info);
}

type_init(register_types);
