/*
 * Talk to logind, via dbus, using the systemd dbus library.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/error-report.h"
#include "ui/logind.h"

#include <systemd/sd-bus.h>

/* ---------------------------------------------------------------------- */

static sd_bus *logind_dbus;

static int logind_take_control(void)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r;

    r = sd_bus_call_method(logind_dbus,
                           "org.freedesktop.login1",
                           "/org/freedesktop/login1/session/self",
                           "org.freedesktop.login1.Session",
                           "TakeControl",
                           &error,
                           &m,
                           "b",
                           false);
    if (r < 0) {
        error_report("logind: TakeControl failed: %s",
                     error.message);
        sd_bus_error_free(&error);
    }
    sd_bus_message_unref(m);

    return r;
}

static int logind_release_control(void)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r;

    r = sd_bus_call_method(logind_dbus,
                           "org.freedesktop.login1",
                           "/org/freedesktop/login1/session/self",
                           "org.freedesktop.login1.Session",
                           "ReleaseControl",
                           &error,
                           &m,
                           "");
    if (r < 0) {
        error_report("logind: ReleaseControl failed: %s",
                     error.message);
        sd_bus_error_free(&error);
    }
    sd_bus_message_unref(m);

    return r;
}

int logind_init(void)
{
    const char *session_id, *seat;
    int r;

    if (logind_dbus) {
        return 0;
    }

    seat = getenv("XDG_SEAT");
    session_id = getenv("XDG_SESSION_ID");
    if (!seat || !session_id) {
        return -1;
    }

    r = sd_bus_open_system(&logind_dbus);
    if (r < 0) {
        error_report("logind: dbus connect failed: %s",
                     strerror(-r));
        return -1;
    }

    r = logind_take_control();
    if (r < 0) {
        goto err;
    }

    return 0;

err:
    sd_bus_unref(logind_dbus);
    logind_dbus = NULL;
    return -1;
}

void logind_fini(void)
{
    if (!logind_dbus) {
        return;
    }

    logind_release_control();
    sd_bus_unref(logind_dbus);
    logind_dbus = NULL;
}

int logind_open(const char *path)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    struct stat st;
    unsigned int maj, min;
    int inactive;
    int handle, fd, r;

    if (!logind_dbus) {
        return -1;
    }

    r = stat(path, &st);
    if (r < 0) {
        error_report("stat %s failed: %s", path, strerror(errno));
        return -1;
    }

    maj = major(st.st_rdev);
    min = minor(st.st_rdev);
    r = sd_bus_call_method(logind_dbus,
                           "org.freedesktop.login1",
                           "/org/freedesktop/login1/session/self",
                           "org.freedesktop.login1.Session",
                           "TakeDevice",
                           &error,
                           &m,
                           "uu",
                           maj,
                           min);
    if (r < 0) {
        error_report("logind: TakeDevice failed: %s",
                     error.message);
        sd_bus_error_free(&error);
        return -1;
    }

    handle = -1;
    inactive = -1;
    r = sd_bus_message_read(m, "hb", &handle, &inactive);
    if (r < 0) {
        fd = -1;
        error_report("logind: Parsing TakeDevice reply failed: %s",
                     strerror(-r));
    } else {
        fd = fcntl(handle, F_DUPFD_CLOEXEC, 0);
    }
    sd_bus_message_unref(m);

    return fd;
}
