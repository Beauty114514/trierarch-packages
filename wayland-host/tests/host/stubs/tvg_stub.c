#include "tvg_scanout.h"
#include <stddef.h>
int tvg_scanout_start(const char *path) { (void)path; return 0; }
void tvg_scanout_stop(void) {}
const struct tvg_scanout_token *tvg_scanout_lookup_fd(int fd) { (void)fd; return NULL; }
void tvg_scanout_present_fd(int fd) { (void)fd; }
void tvg_scanout_begin_frame(void) {}
void tvg_scanout_end_frame(void) {}
void tvg_scanout_release_presented(void) {}
