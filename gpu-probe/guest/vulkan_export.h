#ifndef TRIERARCH_GUEST_VULKAN_EXPORT_H
#define TRIERARCH_GUEST_VULKAN_EXPORT_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#define TRIERARCH_GUEST_DRM_FORMAT_ABGR8888 0x34324241u

struct trierarch_guest_vulkan_image {
    VkInstance instance;
    VkDevice device;
    VkImage image;
    VkDeviceMemory memory;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkDestroyImage destroy_image;
    PFN_vkFreeMemory free_memory;
};

int trierarch_guest_create_exportable_image(struct trierarch_guest_vulkan_image *image,
        int *export_fd, uint32_t *stride, uint64_t *modifier_out);
void trierarch_guest_destroy_exportable_image(struct trierarch_guest_vulkan_image *image);

#endif
