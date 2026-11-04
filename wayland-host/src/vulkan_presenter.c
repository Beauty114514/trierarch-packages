#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "vulkan_presenter.h"

#define TAG "TrierarchVulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct vk_api {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateAndroidSurfaceKHR CreateAndroidSurfaceKHR;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdClearColorImage CmdClearColorImage;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
};

static bool load_instance(struct vk_api *api, void *handle) {
    api->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
    api->GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)dlsym(handle, "vkGetDeviceProcAddr");
    if (!api->GetInstanceProcAddr || !api->GetDeviceProcAddr) return false;
    api->CreateInstance = (PFN_vkCreateInstance)api->GetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!api->CreateInstance) { LOGE("missing vkCreateInstance"); return false; }
    return true;
}

static bool load_instance_functions(struct vk_api *api, VkInstance instance) {
#define INSTANCE(name) api->name = (PFN_vk##name)api->GetInstanceProcAddr(instance, "vk" #name); \
    if (!api->name) { LOGE("missing %s", "vk" #name); return false; }
    INSTANCE(EnumeratePhysicalDevices)
    INSTANCE(GetPhysicalDeviceProperties)
    INSTANCE(GetPhysicalDeviceQueueFamilyProperties)
    INSTANCE(GetPhysicalDeviceMemoryProperties)
    INSTANCE(CreateDevice)
    INSTANCE(GetPhysicalDeviceSurfaceSupportKHR)
    INSTANCE(GetPhysicalDeviceSurfaceCapabilitiesKHR)
    INSTANCE(GetPhysicalDeviceSurfaceFormatsKHR)
    INSTANCE(GetPhysicalDeviceSurfacePresentModesKHR)
    INSTANCE(DestroyInstance)
    INSTANCE(CreateAndroidSurfaceKHR)
    INSTANCE(DestroySurfaceKHR)
#undef INSTANCE
    return true;
}

static void load_device(struct vk_api *api, VkInstance instance) {
#define DEVICE(name) api->name = (PFN_vk##name)api->GetInstanceProcAddr(instance, "vk" #name);
    DEVICE(GetDeviceQueue)
    DEVICE(DestroyDevice)
    DEVICE(CreateSwapchainKHR)
    DEVICE(DestroySwapchainKHR)
    DEVICE(GetSwapchainImagesKHR)
    DEVICE(AcquireNextImageKHR)
    DEVICE(QueuePresentKHR)
    DEVICE(QueueWaitIdle)
    DEVICE(CreateCommandPool)
    DEVICE(DestroyCommandPool)
    DEVICE(AllocateCommandBuffers)
    DEVICE(BeginCommandBuffer)
    DEVICE(EndCommandBuffer)
    DEVICE(CmdPipelineBarrier)
    DEVICE(CmdClearColorImage)
    DEVICE(CreateSemaphore)
    DEVICE(DestroySemaphore)
    DEVICE(QueueSubmit)
    DEVICE(CreateImage)
    DEVICE(DestroyImage)
    DEVICE(GetImageMemoryRequirements)
    DEVICE(AllocateMemory)
    DEVICE(FreeMemory)
    DEVICE(BindImageMemory)
    DEVICE(GetMemoryFdPropertiesKHR)
#undef DEVICE
}

static VkSurfaceFormatKHR choose_format(const VkSurfaceFormatKHR *formats, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ||
                formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
            return formats[i];
    }
    return formats[0];
}

int trierarch_vulkan_presenter_self_test(ANativeWindow *window,
        const char *adrenotools_library, const char *driver_dir,
        const char *hook_dir, const char *tmp_dir) {
    int result = 1;
    typedef void *(*open_libvulkan_fn)(int, int, const char *, const char *, const char *,
            const char *, const char *, void **);
    void *adrenotools = dlopen(adrenotools_library, RTLD_NOW | RTLD_LOCAL);
    if (!adrenotools) {
        LOGE("adrenotools dlopen failed: %s", dlerror());
        return 1;
    }
    open_libvulkan_fn open_libvulkan = (open_libvulkan_fn)dlsym(adrenotools,
            "adrenotools_open_libvulkan");
    if (!open_libvulkan) {
        LOGE("adrenotools_open_libvulkan missing");
        dlclose(adrenotools);
        return 1;
    }
    void *handle = open_libvulkan(RTLD_NOW, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */,
            tmp_dir, hook_dir, driver_dir, "vulkan.ad07xx.so", NULL, NULL);
    LOGI("adrenotools handle=%p", handle);
    if (!handle) {
        dlclose(adrenotools);
        return 1;
    }

    struct vk_api api;
    memset(&api, 0, sizeof(api));
    if (!load_instance(&api, handle)) goto cleanup_handle;

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "TrierarchVulkanTest",
        .apiVersion = VK_API_VERSION_1_1,
    };
    const char *instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vk = api.CreateInstance(&instance_info, NULL, &instance);
    LOGI("vkCreateInstance=%d", vk);
    if (vk != VK_SUCCESS) goto cleanup_handle;
    if (!load_instance_functions(&api, instance)) goto cleanup_instance;

    VkAndroidSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = window,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vk = api.CreateAndroidSurfaceKHR(instance, &surface_info, NULL, &surface);
    LOGI("vkCreateAndroidSurfaceKHR=%d", vk);
    if (vk != VK_SUCCESS) goto cleanup_instance;

    uint32_t device_count = 0;
    api.EnumeratePhysicalDevices(instance, &device_count, NULL);
    if (device_count == 0) { LOGE("no Vulkan devices"); goto cleanup_surface; }
    VkPhysicalDevice *devices = calloc(device_count, sizeof(*devices));
    api.EnumeratePhysicalDevices(instance, &device_count, devices);
    VkPhysicalDevice physical = devices[0];
    VkPhysicalDeviceProperties props;
    api.GetPhysicalDeviceProperties(physical, &props);
    LOGI("device=%s api=%u.%u.%u", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
            VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t family_count = 0;
    api.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    api.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++) {
        VkBool32 supported = VK_FALSE;
        api.GetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && supported) {
            family = i;
            break;
        }
    }
    free(families);
    if (family == UINT32_MAX) { LOGE("no graphics+present queue"); goto cleanup_devices; }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features = {0};
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures = &features,
    };
    VkDevice device = VK_NULL_HANDLE;
    vk = api.CreateDevice(physical, &device_info, NULL, &device);
    LOGI("vkCreateDevice=%d", vk);
    if (vk != VK_SUCCESS) goto cleanup_devices;
    load_device(&api, instance);
    VkQueue queue = VK_NULL_HANDLE;
    api.GetDeviceQueue(device, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    api.GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    uint32_t format_count = 0;
    api.GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, NULL);
    VkSurfaceFormatKHR *formats = calloc(format_count, sizeof(*formats));
    api.GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats);
    VkSurfaceFormatKHR format = choose_format(formats, format_count);
    free(formats);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = (uint32_t)ANativeWindow_getWidth(window);
        extent.height = (uint32_t)ANativeWindow_getHeight(window);
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & composite)) composite = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR swap_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = composite,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    vk = api.CreateSwapchainKHR(device, &swap_info, NULL, &swapchain);
    LOGI("vkCreateSwapchainKHR=%d extent=%ux%u images=%u", vk, extent.width, extent.height, image_count);
    if (vk != VK_SUCCESS) goto cleanup_device;

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    api.CreateCommandPool(device, &pool_info, NULL, &pool);
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    api.AllocateCommandBuffers(device, &command_info, &command);
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired = VK_NULL_HANDLE;
    api.CreateSemaphore(device, &semaphore_info, NULL, &acquired);

    for (int frame = 0; frame < 30; frame++) {
        uint32_t image_index = 0;
        vk = api.AcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &image_index);
        if (vk != VK_SUCCESS) { LOGE("acquire failed %d", vk); break; }
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        api.BeginCommandBuffer(command, &begin);
        VkImageMemoryBarrier to_transfer = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = VK_NULL_HANDLE,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        /* The swapchain image is fetched after the command buffer begins so it
         * can be transitioned without keeping a separate image-view table. */
        uint32_t swap_count = 0;
        api.GetSwapchainImagesKHR(device, swapchain, &swap_count, NULL);
        VkImage *images = calloc(swap_count, sizeof(*images));
        api.GetSwapchainImagesKHR(device, swapchain, &swap_count, images);
        to_transfer.image = images[image_index];
        api.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_transfer);
        VkClearColorValue color = {
            .float32 = {(frame % 60) / 60.0f, 0.2f, 0.8f, 1.0f},
        };
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        api.CmdClearColorImage(command, to_transfer.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        VkImageMemoryBarrier to_present = to_transfer;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        api.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
        api.EndCommandBuffer(command);

        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = (VkPipelineStageFlags[]){VK_PIPELINE_STAGE_TRANSFER_BIT},
            .commandBufferCount = 1,
            .pCommandBuffers = &command,
        };
        vk = api.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if (vk != VK_SUCCESS) { LOGE("submit failed %d", vk); free(images); break; }
        api.QueueWaitIdle(queue);
        VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &image_index,
        };
        vk = api.QueuePresentKHR(queue, &present);
        free(images);
        if (vk != VK_SUCCESS) { LOGE("present failed %d", vk); break; }
        if (frame == 0) LOGI("first frame presented");
    }
    result = 0;
    api.QueueWaitIdle(queue);
    api.DestroySemaphore(device, acquired, NULL);
    api.DestroyCommandPool(device, pool, NULL);
    api.DestroySwapchainKHR(device, swapchain, NULL);

cleanup_device:
    api.DestroyDevice(device, NULL);
cleanup_devices:
    free(devices);
cleanup_surface:
    if (api.DestroySurfaceKHR) api.DestroySurfaceKHR(instance, surface, NULL);
cleanup_instance:
    if (api.DestroyInstance) api.DestroyInstance(instance, NULL);
cleanup_handle:
    dlclose(handle);
    dlclose(adrenotools);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Real dma-buf import check                                                   */

static char *probe_adrenotools;
static char *probe_driver;
static char *probe_hook;
static char *probe_tmp;

void trierarch_vulkan_configure_dmabuf_probe(const char *adrenotools_library,
        const char *driver_dir, const char *hook_dir, const char *tmp_dir) {
    free(probe_adrenotools); probe_adrenotools = NULL;
    free(probe_driver); probe_driver = NULL;
    free(probe_hook); probe_hook = NULL;
    free(probe_tmp); probe_tmp = NULL;
    if (adrenotools_library) probe_adrenotools = strdup(adrenotools_library);
    if (driver_dir) probe_driver = strdup(driver_dir);
    if (hook_dir) probe_hook = strdup(hook_dir);
    if (tmp_dir) probe_tmp = strdup(tmp_dir);
}

bool trierarch_vulkan_dmabuf_probe_configured(void) {
    return probe_adrenotools && probe_driver && probe_hook && probe_tmp;
}

static VkFormat probe_format(unsigned int format) {
    switch (format) {
    case 0x34325258u: return VK_FORMAT_B8G8R8A8_UNORM; /* XRGB8888 */
    case 0x34324258u: return VK_FORMAT_R8G8B8A8_UNORM; /* XBGR8888 */
    case 0x34325241u: return VK_FORMAT_B8G8R8A8_UNORM; /* ARGB8888 */
    case 0x34324241u: return VK_FORMAT_R8G8B8A8_UNORM; /* ABGR8888 */
    default: return VK_FORMAT_UNDEFINED;
    }
}

int trierarch_vulkan_probe_dmabuf(int fd, int width, int height, int stride,
        unsigned int format, unsigned long long modifier) {
    (void)stride; (void)modifier;
    if (!trierarch_vulkan_dmabuf_probe_configured() || fd < 0 || width <= 0 || height <= 0) return -1;
    VkFormat vk_format = probe_format(format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        LOGE("dmabuf probe: unsupported format 0x%x", format);
        return -1;
    }

    int result = -1;
    void *adrenotools = dlopen(probe_adrenotools, RTLD_NOW | RTLD_LOCAL);
    if (!adrenotools) { LOGE("dmabuf probe: adrenotools dlopen failed"); return -1; }
    typedef void *(*open_libvulkan_fn)(int, int, const char *, const char *, const char *,
            const char *, const char *, void **);
    open_libvulkan_fn open_libvulkan = (open_libvulkan_fn)dlsym(adrenotools,
            "adrenotools_open_libvulkan");
    if (!open_libvulkan) { dlclose(adrenotools); return -1; }
    void *handle = open_libvulkan(RTLD_NOW, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */,
            probe_tmp, probe_hook, probe_driver, "vulkan.ad07xx.so", NULL, NULL);
    if (!handle) { dlclose(adrenotools); return -1; }

    struct vk_api api;
    memset(&api, 0, sizeof(api));
    if (!load_instance(&api, handle)) goto cleanup_handle;
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "TrierarchDmabufProbe",
        .apiVersion = VK_API_VERSION_1_1,
    };
    const char *probe_instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = probe_instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (api.CreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) goto cleanup_handle;
    if (!load_instance_functions(&api, instance)) goto cleanup_instance;

    uint32_t device_count = 0;
    api.EnumeratePhysicalDevices(instance, &device_count, NULL);
    if (!device_count) goto cleanup_instance;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    api.EnumeratePhysicalDevices(instance, &device_count, &physical);

    uint32_t family_count = 0;
    api.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    api.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    }
    free(families);
    if (family == UINT32_MAX) goto cleanup_instance;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = device_extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    VkResult vk = api.CreateDevice(physical, &device_info, NULL, &device);
    if (vk != VK_SUCCESS) {
        LOGE("dmabuf probe: vkCreateDevice failed %d", vk);
        goto cleanup_instance;
    }
    load_device(&api, instance);
    VkQueue queue = VK_NULL_HANDLE;
    api.GetDeviceQueue(device, family, 0, &queue);

    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format,
        .extent = {(uint32_t)width, (uint32_t)height, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    vk = api.CreateImage(device, &image_info, NULL, &image);
    if (vk != VK_SUCCESS) {
        LOGE("dmabuf probe: vkCreateImage failed %d", vk);
        goto cleanup_device;
    }
    VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (!api.GetMemoryFdPropertiesKHR || api.GetMemoryFdPropertiesKHR(device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props) != VK_SUCCESS) {
        LOGE("dmabuf probe: vkGetMemoryFdPropertiesKHR failed");
        goto cleanup_image;
    }
    VkMemoryRequirements requirements;
    api.GetImageMemoryRequirements(device, image, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    api.GetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
                (fd_props.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        LOGE("dmabuf probe: no compatible memory type");
        goto cleanup_image;
    }
    int imported_fd = dup(fd);
    if (imported_fd < 0) goto cleanup_image;
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image,
        .pNext = &import,
    };
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    vk = api.AllocateMemory(device, &allocate, NULL, &memory);
    if (vk != VK_SUCCESS) {
        LOGE("dmabuf probe: vkAllocateMemory failed %d", vk);
        close(imported_fd);
        goto cleanup_image;
    }
    vk = api.BindImageMemory(device, image, memory, 0);
    if (vk != VK_SUCCESS) {
        LOGE("dmabuf probe: vkBindImageMemory failed %d", vk);
        goto cleanup_memory;
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    if (api.CreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS) goto cleanup_memory;
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (api.AllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS) {
        api.DestroyCommandPool(device, pool, NULL);
        goto cleanup_memory;
    }
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    api.BeginCommandBuffer(command, &begin);
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    api.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkClearColorValue color = { .float32 = {0.1f, 0.5f, 0.9f, 1.0f} };
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    api.CmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    api.EndCommandBuffer(command);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    vk = api.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (vk == VK_SUCCESS) vk = api.QueueWaitIdle(queue);
    LOGI("dmabuf probe result=%d fd=%d size=%dx%d format=0x%x modifier=0x%llx",
            vk, fd, width, height, format, modifier);
    if (vk == VK_SUCCESS) result = 0;
    api.DestroyCommandPool(device, pool, NULL);
cleanup_memory:
    if (api.FreeMemory) api.FreeMemory(device, memory, NULL);
cleanup_image:
    if (api.DestroyImage) api.DestroyImage(device, image, NULL);
cleanup_device:
    if (api.DestroyDevice) api.DestroyDevice(device, NULL);
cleanup_instance:
    if (api.DestroyInstance) api.DestroyInstance(instance, NULL);
cleanup_handle:
    dlclose(handle);
    dlclose(adrenotools);
    return result;
}
