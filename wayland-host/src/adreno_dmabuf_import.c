#include "adreno_dmabuf_import.h"

#include <dlfcn.h>
#include <android/hardware_buffer.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
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
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahardware_buffer_properties;
    PFN_vkCreateCommandPool create_command_pool;
    PFN_vkDestroyCommandPool destroy_command_pool;
    PFN_vkAllocateCommandBuffers allocate_command_buffers;
    PFN_vkBeginCommandBuffer begin_command_buffer;
    PFN_vkEndCommandBuffer end_command_buffer;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier;
    PFN_vkCmdCopyImage cmd_copy_image;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkQueueWaitIdle queue_wait_idle;
};

struct trierarch_adreno_imported_image {
    struct trierarch_adreno_importer *importer;
    VkImage image;
    VkDeviceMemory memory;
    uint32_t width;
    uint32_t height;
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

static bool has_device_extension(const VkExtensionProperties *extensions, uint32_t count,
        const char *name) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(extensions[index].extensionName, name) == 0) return true;
    }
    return false;
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
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_properties =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)importer->get_instance_proc_addr(
                    importer->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t queue_count = 0;
    if (!queue_properties) {
        set_error(error, error_size, "load Vulkan queue properties");
        return false;
    }
    queue_properties(importer->physical_device, &queue_count, NULL);
    VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
    if (!queues) {
        set_error(error, error_size, "allocate Vulkan queue properties");
        return false;
    }
    queue_properties(importer->physical_device, &queue_count, queues);
    importer->queue_family = UINT32_MAX;
    for (uint32_t index = 0; index < queue_count; ++index) {
        if (queues[index].queueCount) { importer->queue_family = index; break; }
    }
    free(queues);
    if (importer->queue_family == UINT32_MAX) {
        set_error(error, error_size, "find Vulkan queue family");
        return false;
    }

    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions =
            (PFN_vkEnumerateDeviceExtensionProperties)importer->get_instance_proc_addr(
                    importer->instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t extension_count = 0;
    VkExtensionProperties *available_extensions = NULL;
    if (!enumerate_extensions ||
            enumerate_extensions(importer->physical_device, NULL, &extension_count, NULL) != VK_SUCCESS ||
            !(available_extensions = calloc(extension_count, sizeof(*available_extensions))) ||
            enumerate_extensions(importer->physical_device, NULL, &extension_count,
                    available_extensions) != VK_SUCCESS) {
        free(available_extensions);
        set_error(error, error_size, "enumerate Vulkan device extensions");
        return false;
    }

    const char *required_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    };
    for (size_t index = 0; index < sizeof(required_extensions) / sizeof(required_extensions[0]);
            ++index) {
        if (!has_device_extension(available_extensions, extension_count, required_extensions[index])) {
            set_error(error, error_size, "missing Vulkan device extension: %s",
                    required_extensions[index]);
            free(available_extensions);
            return false;
        }
    }
    free(available_extensions);
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = importer->queue_family,
        .queueCount = 1,
        .pQueuePriorities = (float[]) { 1.0f },
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = sizeof(required_extensions) / sizeof(required_extensions[0]),
        .ppEnabledExtensionNames = required_extensions,
    };
    result = create_device(importer->physical_device, &device_info, NULL, &importer->device);
    if (result != VK_SUCCESS) {
        set_error(error, error_size, "create Vulkan device: %d", result);
        return false;
    }
    importer->destroy_device = (PFN_vkDestroyDevice)importer->get_instance_proc_addr(
            importer->instance, "vkDestroyDevice");
    importer->get_device_proc_addr = (PFN_vkGetDeviceProcAddr)importer->get_instance_proc_addr(
            importer->instance, "vkGetDeviceProcAddr");
    if (!importer->get_device_proc_addr) {
        set_error(error, error_size, "load vkGetDeviceProcAddr");
        return false;
    }
#define LOAD_DEVICE_PROC(member, type, name) \
    importer->member = (type)importer->get_device_proc_addr(importer->device, name)
    LOAD_DEVICE_PROC(create_image, PFN_vkCreateImage, "vkCreateImage");
    LOAD_DEVICE_PROC(destroy_image, PFN_vkDestroyImage, "vkDestroyImage");
    LOAD_DEVICE_PROC(get_image_memory_requirements, PFN_vkGetImageMemoryRequirements,
            "vkGetImageMemoryRequirements");
    LOAD_DEVICE_PROC(allocate_memory, PFN_vkAllocateMemory, "vkAllocateMemory");
    LOAD_DEVICE_PROC(free_memory, PFN_vkFreeMemory, "vkFreeMemory");
    LOAD_DEVICE_PROC(bind_image_memory, PFN_vkBindImageMemory, "vkBindImageMemory");
    LOAD_DEVICE_PROC(get_ahardware_buffer_properties, PFN_vkGetAndroidHardwareBufferPropertiesANDROID,
            "vkGetAndroidHardwareBufferPropertiesANDROID");
    LOAD_DEVICE_PROC(create_command_pool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
    LOAD_DEVICE_PROC(destroy_command_pool, PFN_vkDestroyCommandPool, "vkDestroyCommandPool");
    LOAD_DEVICE_PROC(allocate_command_buffers, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    LOAD_DEVICE_PROC(begin_command_buffer, PFN_vkBeginCommandBuffer, "vkBeginCommandBuffer");
    LOAD_DEVICE_PROC(end_command_buffer, PFN_vkEndCommandBuffer, "vkEndCommandBuffer");
    LOAD_DEVICE_PROC(cmd_pipeline_barrier, PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    LOAD_DEVICE_PROC(cmd_copy_image, PFN_vkCmdCopyImage, "vkCmdCopyImage");
    LOAD_DEVICE_PROC(queue_submit, PFN_vkQueueSubmit, "vkQueueSubmit");
    LOAD_DEVICE_PROC(queue_wait_idle, PFN_vkQueueWaitIdle, "vkQueueWaitIdle");
#undef LOAD_DEVICE_PROC
    PFN_vkGetDeviceQueue get_device_queue = (PFN_vkGetDeviceQueue)
            importer->get_device_proc_addr(importer->device, "vkGetDeviceQueue");
    if (get_device_queue) get_device_queue(importer->device, importer->queue_family, 0, &importer->queue);
    if (!importer->destroy_device || !importer->create_image || !importer->destroy_image ||
            !importer->get_image_memory_requirements || !importer->allocate_memory ||
            !importer->free_memory || !importer->bind_image_memory || !importer->queue ||
            !importer->get_ahardware_buffer_properties || !importer->create_command_pool ||
            !importer->destroy_command_pool || !importer->allocate_command_buffers ||
            !importer->begin_command_buffer || !importer->end_command_buffer ||
            !importer->cmd_pipeline_barrier || !importer->cmd_copy_image ||
            !importer->queue_submit || !importer->queue_wait_idle) {
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

struct trierarch_adreno_imported_image *trierarch_adreno_importer_import(
        struct trierarch_adreno_importer *importer,
        const struct trierarch_dmabuf_descriptor *buffer, char *error, unsigned error_size) {
    if (!importer || !buffer || buffer->fd < 0 || buffer->width == 0 || buffer->height == 0 ||
            buffer->modifier == UINT64_MAX || vk_format(buffer->format) == VK_FORMAT_UNDEFINED) {
        set_error(error, error_size, "invalid dma-buf descriptor");
        return NULL;
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
        return NULL;
    }
    VkMemoryRequirements requirements;
    importer->get_image_memory_requirements(importer->device, image, &requirements);
    uint32_t memory_type = 0;
    if (!find_memory_type(importer, requirements.memoryTypeBits, &memory_type)) {
        importer->destroy_image(importer->device, image, NULL);
        set_error(error, error_size, "find dma-buf memory type");
        return NULL;
    }
    int fd = dup(buffer->fd);
    if (fd < 0) {
        importer->destroy_image(importer->device, image, NULL);
        set_error(error, error_size, "duplicate dma-buf fd");
        return NULL;
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
        return NULL;
    }
    result = importer->bind_image_memory(importer->device, image, memory, 0);
    if (result != VK_SUCCESS) {
        importer->free_memory(importer->device, memory, NULL);
        importer->destroy_image(importer->device, image, NULL);
        set_error(error, error_size, "bind dma-buf image memory: %d", result);
        return NULL;
    }
    struct trierarch_adreno_imported_image *imported = calloc(1, sizeof(*imported));
    if (!imported) {
        importer->destroy_image(importer->device, image, NULL);
        importer->free_memory(importer->device, memory, NULL);
        set_error(error, error_size, "allocate imported image");
        return NULL;
    }
    imported->importer = importer;
    imported->image = image;
    imported->memory = memory;
    imported->width = buffer->width;
    imported->height = buffer->height;
    set_error(error, error_size, "import accepted");
    return imported;
}

void trierarch_adreno_imported_image_destroy(struct trierarch_adreno_imported_image *image) {
    if (!image) return;
    struct trierarch_adreno_importer *importer = image->importer;
    if (importer && importer->device) {
        if (image->image) importer->destroy_image(importer->device, image->image, NULL);
        if (image->memory) importer->free_memory(importer->device, image->memory, NULL);
    }
    free(image);
}

AHardwareBuffer *trierarch_adreno_imported_image_copy_to_ahardware_buffer(
        struct trierarch_adreno_imported_image *image, char *error, unsigned error_size) {
    if (!image || !image->importer || !image->image) {
        set_error(error, error_size, "invalid imported image");
        return NULL;
    }
    struct trierarch_adreno_importer *importer = image->importer;
    const AHardwareBuffer_Desc descriptor = {
        .width = image->width, .height = image->height, .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
    };
    AHardwareBuffer *hardware_buffer = NULL;
    VkImage output = VK_NULL_HANDLE;
    VkDeviceMemory output_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (AHardwareBuffer_allocate(&descriptor, &hardware_buffer) != 0 || !hardware_buffer) {
        set_error(error, error_size, "allocate Android hardware buffer");
        return NULL;
    }
    VkAndroidHardwareBufferFormatPropertiesANDROID format = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
    };
    VkAndroidHardwareBufferPropertiesANDROID properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        .pNext = &format,
    };
    if (importer->get_ahardware_buffer_properties(importer->device, hardware_buffer,
            &properties) != VK_SUCCESS || format.format == VK_FORMAT_UNDEFINED) {
        set_error(error, error_size, "read Android hardware buffer Vulkan properties");
        goto fail;
    }
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageCreateInfo output_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format.format,
        .extent = { image->width, image->height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (importer->create_image(importer->device, &output_info, NULL, &output) != VK_SUCCESS) {
        set_error(error, error_size, "create Android hardware buffer image");
        goto fail;
    }
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = output,
    };
    VkImportAndroidHardwareBufferInfoANDROID import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicated,
        .buffer = hardware_buffer,
    };
    uint32_t memory_type = 0;
    if (!find_memory_type(importer, properties.memoryTypeBits, &memory_type)) {
        set_error(error, error_size, "find Android hardware buffer memory type");
        goto fail;
    }
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import,
        .allocationSize = properties.allocationSize,
        .memoryTypeIndex = memory_type,
    };
    if (importer->allocate_memory(importer->device, &allocation, NULL, &output_memory) != VK_SUCCESS ||
            importer->bind_image_memory(importer->device, output, output_memory, 0) != VK_SUCCESS) {
        set_error(error, error_size, "bind Android hardware buffer image");
        goto fail;
    }
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = importer->queue_family,
    };
    if (importer->create_command_pool(importer->device, &pool_info, NULL, &pool) != VK_SUCCESS) {
        set_error(error, error_size, "create copy command pool");
        goto fail;
    }
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo command_allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (importer->allocate_command_buffers(importer->device, &command_allocation, &command) != VK_SUCCESS ||
            importer->begin_command_buffer(command, &begin) != VK_SUCCESS) {
        set_error(error, error_size, "begin copy command");
        goto fail;
    }
    VkImageMemoryBarrier barriers[2] = {
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
          .dstQueueFamilyIndex = importer->queue_family, .image = image->image,
          .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 } },
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = output,
          .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 } },
    };
    importer->cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
    VkImageCopy region = { .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .extent = { image->width, image->height, 1 } };
    importer->cmd_copy_image(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = 0;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = importer->queue_family;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barriers[0].image = output;
    importer->cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, barriers);
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command };
    if (importer->end_command_buffer(command) != VK_SUCCESS ||
            importer->queue_submit(importer->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
            importer->queue_wait_idle(importer->queue) != VK_SUCCESS) {
        set_error(error, error_size, "copy guest image to Android hardware buffer");
        goto fail;
    }
    importer->destroy_command_pool(importer->device, pool, NULL);
    importer->destroy_image(importer->device, output, NULL);
    importer->free_memory(importer->device, output_memory, NULL);
    set_error(error, error_size, "copied guest image to Android hardware buffer");
    return hardware_buffer;
fail:
    if (pool) importer->destroy_command_pool(importer->device, pool, NULL);
    if (output) importer->destroy_image(importer->device, output, NULL);
    if (output_memory) importer->free_memory(importer->device, output_memory, NULL);
    AHardwareBuffer_release(hardware_buffer);
    return NULL;
}

bool trierarch_adreno_importer_validate(struct trierarch_adreno_importer *importer,
        const struct trierarch_dmabuf_descriptor *buffer, char *error, unsigned error_size) {
    struct trierarch_adreno_imported_image *image = trierarch_adreno_importer_import(
            importer, buffer, error, error_size);
    if (!image) return false;
    trierarch_adreno_imported_image_destroy(image);
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
