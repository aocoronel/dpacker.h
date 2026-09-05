#include "dpacker.h"

#if true
#include "alpm.h"
char *native[] = { "grub", "bash", "gcc", "zsh", "linux", NULL };
#else
#include "xbps.h"
char *native[] = { "base-container-full", "gcc", "bash", "libxbps-devel", "zsh", "gdb", NULL };
#endif

char *user[] = { NULL };

int main(int argc, char **argv) {
    printf("Make sure to understand how to use this, before using. This program is destructive\n");
    exit(0);
#if true
    DPacker_Interface interface;
    interface.init = dpacker_alpm_init;
    interface.collect = dpacker_alpm_collect;

    return dpacker(interface, native, user, argc, argv);
#else
    DPacker_Interface interface;
    interface.init = dpacker_xbps_init;
    interface.collect = dpacker_xbps_collect;

    // user in here is unimplemented
    return dpacker(interface, native, NULL, argc, argv);
#endif
}
