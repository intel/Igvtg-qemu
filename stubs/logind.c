#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "ui/logind.h"

int logind_init(void)
{
    warn_report("compiled without logind support");
    return -1;
}

void logind_fini(void)
{
}

int logind_open(const char *path)
{
    return -1;
}
