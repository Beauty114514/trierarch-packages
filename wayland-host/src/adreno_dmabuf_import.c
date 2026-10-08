#include "adreno_dmabuf_import.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

enum { ADRENOTOOLS_DRIVER_CUSTOM = 1 << 0 };
enum { DRM_FORMAT_ARGB8888 = 0x34325241, DRM_FORMAT_XRGB8888 = 0x34325258,
       DRM_FORMAT_ABGR8888 = 0x34324241, DRM_FORMAT_XBGR8888 = 0x34324258 };
typedef void *(*open_libvulkan_fn)(int, int, const char *, const char *, const char *,
        const char *, const char *, void **);

struct trierarch_adreno_importer {
    void *adrenotools;
    void *vulkan;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
};

static void set_error(char *error, unsigned size, const char *format, ...) {
    if (!error || size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static VkFormat vk_format(uint32_t drm_format) {
    switch (drm_format) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

static bool find_memory_type(struct trierarch_adreno_importer *importer, uint32_t bits,
        uint32_t *memory_type) {
    VkPhysicalDeviceMemoryProperties properties;
    importer->get_memory_properties(importer->physical_device, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; index++) {
        if (bits & (1u << index)) {
            *memory_type = index;
            return true;
        }
    }
    return false;
}

static bool initialize_device(struct trierarch_adreno_importer *importer, char *error,
        unsigned error_size) {
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
            importer->get_instance_proc_addr(NULL, "vkCreateInstance");
    if (!create_instance) {
        set_error(error, error_size, "find vkCreateInstance");
        return false;
    }
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Trierarch dma-buf import",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkResult result = create_instance(&create_info, NULL, &importer->instance);
    if (result != VK_SUCCESS) {
        set_error(error, error_size, "create Vulkan instance: %d", result);
        return false;
    }
    importer->destroy_instance = (PFN_vkDestroyInstance)importer->get_instance_proc_addr(
            importer->instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enumerate_devices = (PFN_vkEnumeratePhysicalDevices)
            importer->get_instance_proc_addr(importer->instance, "vkEnumeratePhysicalDevices");
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)importer->get_instance_proc_addr(
            importer->instance, "vkCreateDevice");
    importer->get_memory_properties = (PFN_vkGetPhysicalDeviceMemoryProperties)
            importer->get_instance_proc_addr(importer->instance,
                    "vkGetPhysicalDeviceMemoryProperties");
    uint32_t count = 0;
    if (!importer->destroy_instance || !enumerate_devices || !create_device ||
            !importer->get_memory_properties ||
            enumerate_devices(importer->instance, &count, NULL) != VK_SUCCESS || count == 0) {
        set_error(error, error_size, "enumerate Vulkan devices");
        return false;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices || enumerate_devices(importer->instance, &count, devices) != VK_SUCCESS) {
        free(devices);
        set_error(error, error_size, "read Vulkan device list");
        return false;
    }
    importer->physical_device = devices[0];
    free(devices);

    const char *extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = (float[]) { 1.0f },
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
        .ppEnabledExtensionNames = extensions,
    };
    result = create_device(importer->physical_device, &device_info, NULL, &importer->device);
    if (result != VK_SUCCESS) {
        set_error(error, error_size, "create Vulkan device: %d", result);
        return false;
    }
    importer->destroy_device = (PFN_vkDestroyDevice)importer->get_instance_proc_addr(
            importer->instance, "vkDestroyDevice");
    importer->create_image = (PFN_vkCreateImage)importer->get_instance_proc_addr(
            importer->instance, "vkCreateImage");
    importer->destroy_image = (PFN_vkDestroyImage)importer->get_instance_proc_addr(
            importer->instance, "vkDestroyImage");
    importer->get_image_memory_requirements = (PFN_vkGetImageMemoryRequirements)
            importer->get_instance_proc_addr(importer->instance, "vkGetImageMemoryRequirements");
    importer->allocate_memory = (PFN_vkAllocateMemory)importer->get_instance_proc_addr(
            importer->instance, "vkAllocateMemory");
    importer->free_memory = (PFN_vkFreeMemory)importer->get_instance_proc_addr(
            importer->instance, "vkFreeMemory");
    importer->bind_image_memory = (PFN_vkBindImageMemory)importer->get_instance_proc_addr(
            importer->instance, "vkBindImageMemory");
    if (!importer->destroy_device || !importer->create_image || !importer->destroy_image ||
            !importer->get_image_memory_requirements || !importer->allocate_memory ||
            !importer->free_memory || !importer->bind_image_memory) {
        set_error(error, error_size, "load Vulkan device functions");
        return false;
    }
    return true;
}

struct trierarch_adreno_importer *trierarch_adreno_importer_create(
        const char *hook_library_dir, const char *driver_dir, const char *driver_name,
        char *error, unsigned error_size) {
    struct trierarch_adreno_importer *importer = calloc(1, sizeof(*importer));
    if (!importer) {
        set_error(error, error_size, "allocate importer");
        return NULL;
    }
    importer->adrenotools = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
    open_libvulkan_fn open_libvulkan = importer->adrenotools
            ? (open_libvulkan_fn)dlsym(importer->adrenotools, "adrenotools_open_libvulkan") : NULL;
    if (!open_libvulkan) {
        set_error(error, error_size, "load AdrenoTools: %s", dlerror());
        trierarch_adreno_importer_destroy(importer);
        return NULL;
    }
    importer->vulkan = open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
            NULL, hook_library_dir, driver_dir, driver_name, NULL, NULL);
    importer->get_instance_proc_addr = importer->vulkan ? (PFN_vkGetInstanceProcAddr)
            dlsym(importer->vulkan, "vkGetInstanceProcAddr") : NULL;
    if (!importer->get_instance_proc_addr || !initialize_device(importer, error, error_size)) {
        trierarch_adreno_importer_destroy(importer);
        return NULL;
    }
    return importer;
}

bool trierarch_adreno_importer_validate(struct trierarch_adreno_importer *importer,
        const struct trierarch_dmabuf_descriptor *buffer, char *error, unsigned error_size) {
    if (!importer || !buffer || buffer->fd < 0 || buffer->width == 0 || buffer->height == 0 ||
            buffer->modifier == UINT64_MAX || vk_format(buffer->format) == VK_FORMAT_UNDEFINED) {
        set_error(error, error_size, "invalid dma-buf descriptor");
        return false;
    }
    VkSubresourceLayout layout = { .offset = buffer->offset, .rowPitch = buffer->stride };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = buffer->modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &layout,
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format(buffer->format),
        .extent = { buffer->width, buffer->height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    VkResult result = importer->create_image(importer->device, &image_info, NULL, &image);
    if (result != VK_SUCCESS) {
        set_error(error, error_size, "create dma-buf image: %d", result);
        return false;
    }
    VkMemoryRequirements requirements;
    importer->get_image_memory_requirements(importer->device, image, &requirements);
    uint32_t memory_type = 0;
    if (!find_memory_type(importer, requirements.memoryTypeBits, &memory_type)) {
        importer->destroy_image(importer->device, image, NULL);
        set_error(error, error_size, "find dma-buf memory type");
        return false;
    }
    int fd = dup(buffer->fd);
    if (fd < 0) {
        importer->destroy_image(importer->device, image, NULL);
        set_error(error, error_size, "duplicate dma-buf fd");
        return false;
    }
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    result = importer->allocate_memory(importer->device, &allocation, NULL, &memory);
    if (result != VK_SUCCESS) {
        importer->destroy_image(importer->device, image, NULL);
        set_error(error, error_size, "import dma-buf memory: %d", result);
        return false;
    }
    result = importer->bind_image_memory(importer->device, image, memory, 0);
    importer->free_memory(importer->device, memory, NULL);
    importer->destroy_image(importer->device, image, NULL);
    if (result != VK_SUCCESS) {
        set_error(error, error_size, "bind dma-buf image memory: %d", result);
        return false;
    }
    set_error(error, error_size, "import accepted");
    return true;
}

void trierarch_adreno_importer_destroy(struct trierarch_adreno_importer *importer) {
    if (!importer) return;
    if (importer->device && importer->destroy_device)
        importer->destroy_device(importer->device, NULL);
    if (importer->instance && importer->destroy_instance)
        importer->destroy_instance(importer->instance, NULL);
    if (importer->vulkan) dlclose(importer->vulkan);
    if (importer->adrenotools) dlclose(importer->adrenotools);
    free(importer);
}
