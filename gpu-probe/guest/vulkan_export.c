#include "vulkan_export.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint32_t select_memory_type(VkPhysicalDevice device, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(device, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; index++)
        if (bits & (1u << index)) return index;
    return UINT32_MAX;
}

void trierarch_guest_destroy_exportable_image(struct trierarch_guest_vulkan_image *image) {
    if (image->memory) image->free_memory(image->device, image->memory, NULL);
    if (image->image) image->destroy_image(image->device, image->image, NULL);
    if (image->device) image->destroy_device(image->device, NULL);
    if (image->instance) image->destroy_instance(image->instance, NULL);
    memset(image, 0, sizeof(*image));
}

static int clear_exported_image(VkDevice device, VkImage image, VkQueue queue,
        uint32_t queue_family) {
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkAllocateCommandBuffers(device, &allocation, &command) != VK_SUCCESS ||
            vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) goto fail;
    VkImageMemoryBarrier acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1 },
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &acquire);
    const VkClearColorValue color = { .float32 = { 0.12f, 0.72f, 0.36f, 1.0f } };
    const VkImageSubresourceRange range = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1, .layerCount = 1 };
    vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImageMemoryBarrier release = acquire;
    release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    release.srcQueueFamilyIndex = queue_family;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &release);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) goto fail;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command };
    if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(queue) != VK_SUCCESS) goto fail;
    vkDestroyCommandPool(device, pool, NULL);
    return 0;
fail:
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    return -1;
}

int trierarch_guest_create_exportable_image(struct trierarch_guest_vulkan_image *image,
        int *export_fd, uint32_t *stride, uint64_t *modifier_out) {
    const char *extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    VkApplicationInfo application = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Trierarch guest dma-buf probe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application };
    if (vkCreateInstance(&instance_info, NULL, &image->instance) != VK_SUCCESS) return -1;
    image->destroy_instance = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(
            image->instance, "vkDestroyInstance");
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(image->instance, &count, NULL) != VK_SUCCESS || count == 0) goto fail;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices || vkEnumeratePhysicalDevices(image->instance, &count, devices) != VK_SUCCESS) {
        free(devices); goto fail;
    }
    VkPhysicalDevice physical = devices[0];
    free(devices);
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
    VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
    if (!queues) goto fail;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues);
    uint32_t family = UINT32_MAX;
    for (uint32_t index = 0; index < queue_count; index++)
        if (queues[index].queueCount) { family = index; break; }
    free(queues);
    if (family == UINT32_MAX) goto fail;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    VkDeviceCreateInfo device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
        .ppEnabledExtensionNames = extensions };
    if (vkCreateDevice(physical, &device_info, NULL, &image->device) != VK_SUCCESS) goto fail;
    image->destroy_device = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(image->device, "vkDestroyDevice");
    image->destroy_image = (PFN_vkDestroyImage)vkGetDeviceProcAddr(image->device, "vkDestroyImage");
    image->free_memory = (PFN_vkFreeMemory)vkGetDeviceProcAddr(image->device, "vkFreeMemory");
    PFN_vkGetPhysicalDeviceFormatProperties2 get_format_properties =
            (PFN_vkGetPhysicalDeviceFormatProperties2)vkGetInstanceProcAddr(
                    image->instance, "vkGetPhysicalDeviceFormatProperties2");
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier =
            (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
                    image->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    PFN_vkGetMemoryFdKHR get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(
            image->device, "vkGetMemoryFdKHR");
    if (!get_format_properties || !get_modifier || !get_memory_fd) goto fail;
    VkDrmFormatModifierPropertiesListEXT modifiers = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 properties = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifiers };
    get_format_properties(physical, VK_FORMAT_R8G8B8A8_UNORM, &properties);
    if (!modifiers.drmFormatModifierCount) goto fail;
    VkDrmFormatModifierPropertiesEXT *available = calloc(modifiers.drmFormatModifierCount,
            sizeof(*available));
    if (!available) goto fail;
    modifiers.pDrmFormatModifierProperties = available;
    get_format_properties(physical, VK_FORMAT_R8G8B8A8_UNORM, &properties);
    uint64_t selected = available[0].drmFormatModifier;
    free(available);
    VkImageDrmFormatModifierListCreateInfoEXT list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1, .pDrmFormatModifiers = &selected };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &list,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkImageCreateInfo image_info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 64, 64, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(image->device, &image_info, NULL, &image->image) != VK_SUCCESS) goto fail;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(image->device, image->image, &requirements);
    uint32_t memory_type = select_memory_type(physical, requirements.memoryTypeBits);
    if (memory_type == UINT32_MAX) goto fail;
    VkExportMemoryAllocateInfo export_info = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info, .allocationSize = requirements.size, .memoryTypeIndex = memory_type };
    if (vkAllocateMemory(image->device, &allocation, NULL, &image->memory) != VK_SUCCESS ||
            vkBindImageMemory(image->device, image->image, image->memory, 0) != VK_SUCCESS) goto fail;
    VkQueue queue_handle = VK_NULL_HANDLE;
    vkGetDeviceQueue(image->device, family, 0, &queue_handle);
    if (!queue_handle || clear_exported_image(image->device, image->image, queue_handle, family) < 0)
        goto fail;
    VkImageDrmFormatModifierPropertiesEXT modifier_properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    VkImageSubresource subresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
    VkSubresourceLayout layout;
    VkMemoryGetFdInfoKHR fd_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = image->memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    if (get_modifier(image->device, image->image, &modifier_properties) != VK_SUCCESS) goto fail;
    vkGetImageSubresourceLayout(image->device, image->image, &subresource, &layout);
    if (get_memory_fd(image->device, &fd_info, export_fd) != VK_SUCCESS) goto fail;
    *stride = (uint32_t)layout.rowPitch;
    *modifier_out = modifier_properties.drmFormatModifier;
    return 0;
fail:
    trierarch_guest_destroy_exportable_image(image);
    return -1;
}
