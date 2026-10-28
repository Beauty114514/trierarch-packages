#ifndef TRIERARCH_TVG_SCANOUT_H
#define TRIERARCH_TVG_SCANOUT_H

#include <stdint.h>
#include <sys/types.h>

/*
 * Host-side token service for the virgl "last cell" POC.
 *
 * The guest libgbm shim asks this service for a buffer; it gets back a memfd
 * token plus stride/format.  The service owns the AHardwareBuffer that is
 * actually displayed.  The token fd is only an identity: Mesa's vtest winsys
 * creates a regular virgl texture for the image and never reads the token.
 */

struct tvg_scanout_token {
    int fd;
    dev_t device;
    ino_t inode;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    void *ahb; /* AHardwareBuffer * */
    int res_id; /* filled by the vtest token registration later */
};

int tvg_scanout_start(const char *socket_path);

void tvg_scanout_stop(void);

/* Returns the token owning this fd, or NULL.  The pointer stays valid until
 * tvg_scanout_stop(). */
const struct tvg_scanout_token *tvg_scanout_lookup_fd(int fd);

/* Ask the vtest server to blit the registered resource into the token AHB. */
void tvg_scanout_present_fd(int fd);

/* Tell the vtest server that the host has finished sampling every AHB that was
 * presented since the last call.  Called from the renderer after its frame is
 * submitted, so the server does not overwrite a buffer still in use. */
void tvg_scanout_release_presented(void);

#endif
