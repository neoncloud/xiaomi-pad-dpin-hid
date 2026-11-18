#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc;
    const char *real = "/odm/bin/vendor.xiaomi.usb.dp-service.real";
    if (setenv("LD_LIBRARY_PATH", "/odm/lib64", 1) != 0 ||
        setenv("LD_PRELOAD", "/odm/lib64/libshadowhook.so:/odm/lib64/libptp_auto.so", 1) != 0) {
        dprintf(2, "ptp launcher setenv failed: %s\n", strerror(errno));
        return 126;
    }
    execv(real, argv);
    dprintf(2, "ptp launcher exec failed: %s\n", strerror(errno));
    return 127;
}
