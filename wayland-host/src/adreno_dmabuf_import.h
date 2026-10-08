#ifndef TRIERARCH_ADRENO_DMABUF_IMPORT_H
#define TRIERARCH_ADRENO_DMABUF_IMPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <android/hardware_buffer.h>

struct trierarch_adreno_importer;
struct trierarch_adreno_imported_image;

struct trierarch_dmabuf_descriptor {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
};

struct trierarch_adreno_importer *trierarch_adreno_importer_create(
        const char *hook_library_dir, const char *driver_dir, const char *driver_name,
        char *error, unsigned error_size);
/* The returned image owns its Vulkan image and imported memory.  It must be
 * released before its importer is destroyed. */
struct trierarch_adreno_imported_image *trierarch_adreno_importer_import(
        struct trierarch_adreno_importer *importer,
        const struct trierarch_dmabuf_descriptor *buffer, char *error, unsigned error_size);
void trierarch_adreno_imported_image_destroy(struct trierarch_adreno_imported_image *image);
/* Produces an Android-owned copy suitable for import by the existing EGL
 * presenter. The caller owns the returned AHardwareBuffer reference. */
AHardwareBuffer *trierarch_adreno_imported_image_copy_to_ahardware_buffer(
        struct trierarch_adreno_imported_image *image, char *error, unsigned error_size);
bool trierarch_adreno_importer_validate(struct trierarch_adreno_importer *importer,
        const struct trierarch_dmabuf_descriptor *buffer, char *error, unsigned error_size);
void trierarch_adreno_importer_destroy(struct trierarch_adreno_importer *importer);

#endif
