#define VK_USE_PLATFORM_ANDROID_KHR 1

#include <android/hardware_buffer.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static const char *result_name(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    default:
        return "VK_OTHER";
    }
}

static const char *handle_name(VkExternalMemoryHandleTypeFlagBits type)
{
    switch (type) {
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
        return "dma-buf";
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
        return "opaque-fd";
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID:
        return "android-hardware-buffer";
    default:
        return "unknown";
    }
}

static int has_extension(const VkExtensionProperties *extensions,
                         uint32_t count,
                         const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (!strcmp(extensions[i].extensionName, name))
            return 1;
    }
    return 0;
}

static uint32_t choose_memory_type(VkPhysicalDevice physical,
                                   uint32_t bits,
                                   VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return UINT32_MAX;
}

static void query_handle(
    VkPhysicalDevice physical,
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_external_properties,
    VkExternalMemoryHandleTypeFlagBits handle)
{
    const VkPhysicalDeviceExternalBufferInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .handleType = handle,
    };
    VkExternalBufferProperties properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    get_external_properties(physical, &info, &properties);
    printf("probe handle=%s compatible=0x%x exportable=%u importable=%u\n",
           handle_name(handle),
           properties.externalMemoryProperties.compatibleHandleTypes,
           !!(properties.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
           !!(properties.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT));
}

static void probe_fd(VkDevice device,
                     VkPhysicalDevice physical,
                     PFN_vkGetMemoryFdKHR get_memory_fd,
                     VkExternalMemoryHandleTypeFlagBits handle,
                     VkMemoryPropertyFlags required)
{
    const VkExternalMemoryBufferCreateInfo external_buffer = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = handle,
    };
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_buffer,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = vkCreateBuffer(device, &buffer_info, NULL, &buffer);
    if (result != VK_SUCCESS) {
        printf("probe handle=%s create-buffer=%s(%d)\n",
               handle_name(handle), result_name(result), result);
        return;
    }

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const uint32_t memory_type =
        choose_memory_type(physical, requirements.memoryTypeBits, required);
    if (memory_type == UINT32_MAX) {
        printf("probe handle=%s memory-type=none required=0x%x\n",
               handle_name(handle), required);
        vkDestroyBuffer(device, buffer, NULL);
        return;
    }

    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = handle,
    };
    const VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(device, &allocation_info, NULL, &memory);
    if (result != VK_SUCCESS) {
        printf("probe handle=%s memory-type=%u allocate=%s(%d)\n",
               handle_name(handle), memory_type, result_name(result), result);
        vkDestroyBuffer(device, buffer, NULL);
        return;
    }

    int fd = -1;
    const VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = memory,
        .handleType = handle,
    };
    result = get_memory_fd(device, &fd_info, &fd);
    printf("probe handle=%s memory-type=%u get-fd=%s(%d) fd=%d\n",
           handle_name(handle), memory_type, result_name(result), result, fd);
    if (fd >= 0)
        close(fd);
    vkFreeMemory(device, memory, NULL);
    vkDestroyBuffer(device, buffer, NULL);
}

static void probe_ahb(VkDevice device,
                      VkPhysicalDevice physical,
                      PFN_vkGetMemoryAndroidHardwareBufferANDROID get_ahb)
{
    const VkExternalMemoryHandleTypeFlagBits handle =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    const VkExternalMemoryBufferCreateInfo external_buffer = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = handle,
    };
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &external_buffer,
        .size = 4096,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = vkCreateBuffer(device, &buffer_info, NULL, &buffer);
    if (result != VK_SUCCESS) {
        printf("probe handle=%s create-buffer=%s(%d)\n",
               handle_name(handle), result_name(result), result);
        return;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const uint32_t memory_type = choose_memory_type(physical, requirements.memoryTypeBits, 0);
    if (memory_type == UINT32_MAX) {
        printf("probe handle=%s memory-type=none\n", handle_name(handle));
        vkDestroyBuffer(device, buffer, NULL);
        return;
    }
    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = handle,
    };
    const VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(device, &allocation_info, NULL, &memory);
    if (result == VK_SUCCESS) {
        AHardwareBuffer *buffer_handle = NULL;
        const VkMemoryGetAndroidHardwareBufferInfoANDROID ahb_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .memory = memory,
        };
        result = get_ahb(device, &ahb_info, &buffer_handle);
        printf("probe handle=%s memory-type=%u get-ahb=%s(%d) ahb=%p\n",
               handle_name(handle), memory_type, result_name(result), result,
               (void *)buffer_handle);
        if (buffer_handle)
            AHardwareBuffer_release(buffer_handle);
    } else {
        printf("probe handle=%s memory-type=%u allocate=%s(%d)\n",
               handle_name(handle), memory_type, result_name(result), result);
    }
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(device, memory, NULL);
    vkDestroyBuffer(device, buffer, NULL);
}

int main(void)
{
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        printf("probe instance=%s(%d)\n", result_name(result), result);
        return 1;
    }

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, NULL);
    if (result != VK_SUCCESS || !physical_count) {
        printf("probe physical-devices=%s(%d) count=%u\n",
               result_name(result), result, physical_count);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice physical;
    uint32_t selected_count = 1;
    vkEnumeratePhysicalDevices(instance, &selected_count, &physical);

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("probe device=%s api=%u.%u\n", properties.deviceName,
           VK_API_VERSION_MAJOR(properties.apiVersion),
           VK_API_VERSION_MINOR(properties.apiVersion));

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL);
    VkExtensionProperties extensions[256];
    if (extension_count > 256)
        extension_count = 256;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions);
    const char *required_extension = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    const int has_fd = has_extension(extensions, extension_count, required_extension);
    const int has_ahb = has_extension(
        extensions, extension_count,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    printf("probe extension %s=%d\n", required_extension, has_fd);
    printf("probe extension %s=%d\n",
           VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, has_ahb);

    PFN_vkGetPhysicalDeviceExternalBufferProperties get_external_properties =
        (PFN_vkGetPhysicalDeviceExternalBufferProperties)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceExternalBufferProperties");
    printf("probe vkGetPhysicalDeviceExternalBufferProperties=%s\n",
           get_external_properties ? "present" : "missing");

    const VkExternalMemoryHandleTypeFlagBits handles[] = {
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    if (get_external_properties) {
        for (uint32_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++)
            query_handle(physical, get_external_properties, handles[i]);
    }

    if (!has_fd && !has_ahb) {
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    const char *device_extensions[2];
    uint32_t device_extension_count = 0;
    if (has_fd)
        device_extensions[device_extension_count++] = required_extension;
    if (has_ahb)
        device_extensions[device_extension_count++] =
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
    const VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = (const float[]){1.0f},
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = device_extension_count,
        .ppEnabledExtensionNames = device_extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(physical, &device_info, NULL, &device);
    if (result != VK_SUCCESS) {
        printf("probe device-create=%s(%d)\n", result_name(result), result);
        vkDestroyInstance(instance, NULL);
        return 1;
    }

    PFN_vkGetMemoryFdKHR get_memory_fd = has_fd
        ? (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR")
        : NULL;
    printf("probe vkGetMemoryFdKHR=%s\n", get_memory_fd ? "present" : "missing");
    if (get_memory_fd) {
        probe_fd(device, physical, get_memory_fd,
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        probe_fd(device, physical, get_memory_fd,
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    PFN_vkGetMemoryAndroidHardwareBufferANDROID get_ahb = has_ahb
        ? (PFN_vkGetMemoryAndroidHardwareBufferANDROID)vkGetDeviceProcAddr(
              device, "vkGetMemoryAndroidHardwareBufferANDROID")
        : NULL;
    printf("probe vkGetMemoryAndroidHardwareBufferANDROID=%s\n",
           get_ahb ? "present" : "missing");
    if (get_ahb)
        probe_ahb(device, physical, get_ahb);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
