#define _GNU_SOURCE
#include "../protocol.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define DRM_FORMAT_ABGR8888 0x34324241u

struct vulkan_image {
    VkInstance instance;
    VkDevice device;
    VkImage image;
    VkDeviceMemory memory;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkDestroyImage destroy_image;
    PFN_vkFreeMemory free_memory;
};

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (fd < 0 || strlen(path) >= sizeof(address.sun_path)) return -1;
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_hello(int fd) {
    const struct trierarch_gpu_probe_hello hello = {
        .magic = TRIERARCH_GPU_PROBE_MAGIC, .version = TRIERARCH_GPU_PROBE_VERSION,
        .type = TRIERARCH_GPU_PROBE_HELLO, .direction = TRIERARCH_GPU_PROBE_GUEST_TO_HOST,
    };
    return send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) == (ssize_t)sizeof(hello) ? 0 : -1;
}

static int send_buffer(int socket_fd, const struct trierarch_gpu_probe_buffer *buffer, int fd) {
    char control[CMSG_SPACE(sizeof(fd))] = {0};
    struct iovec iov = { .iov_base = (void *)buffer, .iov_len = sizeof(*buffer) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(socket_fd, &message, MSG_NOSIGNAL) == (ssize_t)sizeof(*buffer) ? 0 : -1;
}

static uint32_t select_memory_type(VkPhysicalDevice device, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(device, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; index++)
        if (bits & (1u << index)) return index;
    return UINT32_MAX;
}

static void destroy_image(struct vulkan_image *image) {
    if (image->memory) image->free_memory(image->device, image->memory, NULL);
    if (image->image) image->destroy_image(image->device, image->image, NULL);
    if (image->device) image->destroy_device(image->device, NULL);
    if (image->instance) image->destroy_instance(image->instance, NULL);
    memset(image, 0, sizeof(*image));
}

static int create_exportable_image(struct vulkan_image *image, int *export_fd,
        uint32_t *stride, uint64_t *modifier_out) {
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
    image->destroy_device = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(
            image->device, "vkDestroyDevice");
    image->destroy_image = (PFN_vkDestroyImage)vkGetDeviceProcAddr(
            image->device, "vkDestroyImage");
    image->free_memory = (PFN_vkFreeMemory)vkGetDeviceProcAddr(
            image->device, "vkFreeMemory");
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
    destroy_image(image);
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s GPU_PROBE_SOCKET\n", argv[0]); return 64; }
    struct vulkan_image image = {0};
    int buffer_fd = -1;
    uint32_t stride = 0;
    uint64_t modifier = 0;
    if (create_exportable_image(&image, &buffer_fd, &stride, &modifier) < 0) {
        fputs("guest: unable to create exportable Turnip dma-buf\n", stderr); return 1;
    }
    int socket_fd = connect_socket(argv[1]);
    struct trierarch_gpu_probe_buffer buffer = { .magic = TRIERARCH_GPU_PROBE_MAGIC,
        .version = TRIERARCH_GPU_PROBE_VERSION, .type = TRIERARCH_GPU_PROBE_GUEST_BUFFER,
        .width = 64, .height = 64, .drm_format = DRM_FORMAT_ABGR8888, .stride = stride,
        .modifier = modifier };
    int status = socket_fd >= 0 && send_hello(socket_fd) == 0 &&
            send_buffer(socket_fd, &buffer, buffer_fd) == 0 ? 0 : 2;
    struct trierarch_gpu_probe_result_message result = {0};
    if (status == 0 && recv(socket_fd, &result, sizeof(result), 0) == (ssize_t)sizeof(result) &&
            result.magic == TRIERARCH_GPU_PROBE_MAGIC && result.result == TRIERARCH_GPU_PROBE_OK)
        printf("guest: host accepted dma-buf stride=%u modifier=0x%llx\n", stride,
                (unsigned long long)modifier);
    else if (status == 0) { fputs("guest: host rejected dma-buf\n", stderr); status = 3; }
    if (socket_fd >= 0) close(socket_fd);
    close(buffer_fd);
    destroy_image(&image);
    return status;
}
