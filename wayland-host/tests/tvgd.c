#include "tvg_scanout.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/data/local/tmp/tvgd.sock";
    int result = tvg_scanout_start(path);
    if (result != 0) {
        fprintf(stderr, "tvg_scanout_start(%s) failed: %d\n", path, result);
        return 1;
    }
    printf("tvgd ready: %s\n", path);
    fflush(stdout);
    pause();
    tvg_scanout_stop();
    return 0;
}
