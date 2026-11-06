#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "vulkan_presenter.h"
#include "vulkan_shaders/cursor_vert.inc"
#include "vulkan_shaders/cursor_frag.inc"

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
    PFN_vkCmdBlitImage CmdBlitImage;
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
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkResetDescriptorPool ResetDescriptorPool;
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPushConstants CmdPushConstants;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdSetViewport CmdSetViewport;
    PFN_vkCmdSetScissor CmdSetScissor;
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
    DEVICE(CmdBlitImage)
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
    DEVICE(GetImageSubresourceLayout)
    DEVICE(MapMemory)
    DEVICE(UnmapMemory)
    DEVICE(CreateImageView)
    DEVICE(DestroyImageView)
    DEVICE(CreateRenderPass)
    DEVICE(DestroyRenderPass)
    DEVICE(CreateFramebuffer)
    DEVICE(DestroyFramebuffer)
    DEVICE(CreateShaderModule)
    DEVICE(DestroyShaderModule)
    DEVICE(CreatePipelineLayout)
    DEVICE(DestroyPipelineLayout)
    DEVICE(CreateGraphicsPipelines)
    DEVICE(DestroyPipeline)
    DEVICE(CreateDescriptorSetLayout)
    DEVICE(DestroyDescriptorSetLayout)
    DEVICE(CreateDescriptorPool)
    DEVICE(DestroyDescriptorPool)
    DEVICE(AllocateDescriptorSets)
    DEVICE(UpdateDescriptorSets)
    DEVICE(ResetDescriptorPool)
    DEVICE(CreateSampler)
    DEVICE(DestroySampler)
    DEVICE(CmdBeginRenderPass)
    DEVICE(CmdEndRenderPass)
    DEVICE(CmdBindPipeline)
    DEVICE(CmdBindDescriptorSets)
    DEVICE(CmdPushConstants)
    DEVICE(CmdDraw)
    DEVICE(CmdSetViewport)
    DEVICE(CmdSetScissor)
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

static int64_t presenter_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
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

/* ------------------------------------------------------------------------- */
/* Persistent production presenter                                             */

#define VULKAN_IMPORT_CACHE_SIZE 8

struct dmabuf_import {
    unsigned long long key;
    int width;
    int height;
    unsigned int format;
    unsigned long long modifier;
    VkImage image;
    VkDeviceMemory memory;
    VkImageLayout layout;
    unsigned long long last_used;
};

struct trierarch_vulkan_presenter {
    struct vk_api api;
    void *adrenotools;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    ANativeWindow *window;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    VkFormat format;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkSemaphore acquired;
    unsigned long long frames_presented;
    unsigned long long suboptimal_frames;
    int64_t last_present_log_ns;
    VkRenderPass cursor_pass;
    VkPipelineLayout cursor_layout;
    VkPipeline cursor_pipeline;
    VkDescriptorSetLayout cursor_descriptor_layout;
    VkDescriptorPool cursor_descriptor_pool;
    VkDescriptorSet cursor_descriptor;
    VkSampler cursor_sampler;
    VkImageView *swap_views;
    VkFramebuffer *swap_framebuffers;
    VkImage *swap_images;
    uint32_t swap_image_count;
    VkPhysicalDeviceMemoryProperties memory_properties;
    struct dmabuf_import imports[VULKAN_IMPORT_CACHE_SIZE];
    unsigned int import_count;
    unsigned long long import_clock;
    unsigned long long import_hits;
    unsigned long long import_misses;
    struct trierarch_vulkan_draw cursor_draw;
    bool cursor_set;
};

bool trierarch_vulkan_presenter_configured(void) {
    return trierarch_vulkan_dmabuf_probe_configured();
}

static void presenter_destroy_swapchain_resources(trierarch_vulkan_presenter *presenter);
static void presenter_destroy_imports(trierarch_vulkan_presenter *presenter);

static void destroy_presenter_objects(trierarch_vulkan_presenter *presenter) {
    if (!presenter) return;
    if (presenter->queue) presenter->api.QueueWaitIdle(presenter->queue);
    presenter_destroy_swapchain_resources(presenter);
    presenter_destroy_imports(presenter);
    if (presenter->device) {
        if (presenter->cursor_sampler) presenter->api.DestroySampler(presenter->device, presenter->cursor_sampler, NULL);
        if (presenter->cursor_descriptor_pool) presenter->api.DestroyDescriptorPool(presenter->device, presenter->cursor_descriptor_pool, NULL);
        if (presenter->cursor_pipeline) presenter->api.DestroyPipeline(presenter->device, presenter->cursor_pipeline, NULL);
        if (presenter->cursor_layout) presenter->api.DestroyPipelineLayout(presenter->device, presenter->cursor_layout, NULL);
        if (presenter->cursor_descriptor_layout) presenter->api.DestroyDescriptorSetLayout(presenter->device, presenter->cursor_descriptor_layout, NULL);
        if (presenter->cursor_pass) presenter->api.DestroyRenderPass(presenter->device, presenter->cursor_pass, NULL);
        if (presenter->command) presenter->api.DestroyCommandPool(presenter->device, presenter->pool, NULL);
        if (presenter->acquired) presenter->api.DestroySemaphore(presenter->device, presenter->acquired, NULL);
        if (presenter->swapchain) presenter->api.DestroySwapchainKHR(presenter->device, presenter->swapchain, NULL);
        presenter->api.DestroyDevice(presenter->device, NULL);
    }
    if (presenter->instance) {
        if (presenter->surface) presenter->api.DestroySurfaceKHR(presenter->instance, presenter->surface, NULL);
        presenter->api.DestroyInstance(presenter->instance, NULL);
    }
    if (presenter->adrenotools) dlclose(presenter->adrenotools);
}

static bool import_dmabuf_image(trierarch_vulkan_presenter *presenter, int fd,
        int width, int height, VkImage *out_image, VkDeviceMemory *out_memory) {
    VkFormat format = probe_format(0); /* overwritten below */
    (void)format;
    return false;
}


static void presenter_destroy_swapchain_resources(trierarch_vulkan_presenter *presenter) {
    if (!presenter || !presenter->device) return;
    for (uint32_t i = 0; i < presenter->swap_image_count; i++) {
        if (presenter->swap_framebuffers) {
            if (presenter->swap_framebuffers[i])
                presenter->api.DestroyFramebuffer(presenter->device,
                        presenter->swap_framebuffers[i], NULL);
        }
        if (presenter->swap_views && presenter->swap_views[i])
            presenter->api.DestroyImageView(presenter->device, presenter->swap_views[i], NULL);
    }
    free(presenter->swap_framebuffers);
    free(presenter->swap_views);
    free(presenter->swap_images);
    presenter->swap_framebuffers = NULL;
    presenter->swap_views = NULL;
    presenter->swap_images = NULL;
    presenter->swap_image_count = 0;
}

static bool presenter_create_cursor_pipeline(trierarch_vulkan_presenter *presenter) {
    if (presenter->cursor_pipeline) return true;
    VkAttachmentDescription attachment = {
        .format = presenter->format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference ref = {
        .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref,
    };
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass,
    };
    if (presenter->api.CreateRenderPass(presenter->device, &pass_info, NULL,
            &presenter->cursor_pass) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding,
    };
    if (presenter->api.CreateDescriptorSetLayout(presenter->device, &layout_info, NULL,
            &presenter->cursor_descriptor_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0, .size = sizeof(float) * 9,
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &presenter->cursor_descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    if (presenter->api.CreatePipelineLayout(presenter->device, &pipeline_layout_info, NULL,
            &presenter->cursor_layout) != VK_SUCCESS) return false;

    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo vert_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(cursor_vert_code), .pCode = cursor_vert_code,
    };
    VkShaderModuleCreateInfo frag_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(cursor_frag_code), .pCode = cursor_frag_code,
    };
    if (presenter->api.CreateShaderModule(presenter->device, &vert_info, NULL, &vert) != VK_SUCCESS ||
            presenter->api.CreateShaderModule(presenter->device, &frag_info, NULL, &frag) != VK_SUCCESS)
        return false;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"},
    };
    VkVertexInputBindingDescription no_bindings = {0};
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 0, .pVertexBindingDescriptions = &no_bindings,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkViewport viewport = {0, 0, (float)presenter->extent.width, (float)presenter->extent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, presenter->extent};
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment,
    };
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dynamic,
    };
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic_state,
        .layout = presenter->cursor_layout,
        .renderPass = presenter->cursor_pass,
    };
    VkResult result = presenter->api.CreateGraphicsPipelines(presenter->device,
            VK_NULL_HANDLE, 1, &pipeline_info, NULL, &presenter->cursor_pipeline);
    presenter->api.DestroyShaderModule(presenter->device, vert, NULL);
    presenter->api.DestroyShaderModule(presenter->device, frag, NULL);
    if (result != VK_SUCCESS) return false;

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 64,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 64, .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    if (presenter->api.CreateDescriptorPool(presenter->device, &pool_info, NULL,
            &presenter->cursor_descriptor_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = presenter->cursor_descriptor_pool,
        .descriptorSetCount = 1, .pSetLayouts = &presenter->cursor_descriptor_layout,
    };
    if (presenter->api.AllocateDescriptorSets(presenter->device, &set_info,
            &presenter->cursor_descriptor) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (presenter->api.CreateSampler(presenter->device, &sampler_info, NULL,
            &presenter->cursor_sampler) != VK_SUCCESS) return false;
    return true;
}

static bool presenter_create_swapchain_resources(trierarch_vulkan_presenter *presenter) {
    presenter_destroy_swapchain_resources(presenter);
    uint32_t count = 0;
    presenter->api.GetSwapchainImagesKHR(presenter->device, presenter->swapchain, &count, NULL);
    if (!count) return false;
    presenter->swap_images = calloc(count, sizeof(*presenter->swap_images));
    presenter->swap_views = calloc(count, sizeof(*presenter->swap_views));
    presenter->swap_framebuffers = calloc(count, sizeof(*presenter->swap_framebuffers));
    presenter->swap_image_count = count;
    if (!presenter->swap_images || !presenter->swap_views || !presenter->swap_framebuffers) {
        presenter_destroy_swapchain_resources(presenter);
        return false;
    }
    presenter->api.GetSwapchainImagesKHR(presenter->device, presenter->swapchain, &count,
            presenter->swap_images);
    if (!presenter_create_cursor_pipeline(presenter)) {
        presenter_destroy_swapchain_resources(presenter);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = presenter->swap_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = presenter->format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (presenter->api.CreateImageView(presenter->device, &view_info, NULL,
                &presenter->swap_views[i]) != VK_SUCCESS) {
            presenter_destroy_swapchain_resources(presenter);
            return false;
        }
        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = presenter->cursor_pass,
            .attachmentCount = 1, .pAttachments = &presenter->swap_views[i],
            .width = presenter->extent.width, .height = presenter->extent.height,
            .layers = 1,
        };
        if (presenter->api.CreateFramebuffer(presenter->device, &framebuffer_info, NULL,
                &presenter->swap_framebuffers[i]) != VK_SUCCESS) {
            presenter_destroy_swapchain_resources(presenter);
            return false;
        }
    }
    return true;
}

static bool presenter_create_swapchain(trierarch_vulkan_presenter *presenter) {
    if (!presenter || !presenter->device || !presenter->surface || !presenter->window) return false;
    presenter->api.QueueWaitIdle(presenter->queue);
    VkSurfaceCapabilitiesKHR caps;
    presenter->api.GetPhysicalDeviceSurfaceCapabilitiesKHR(presenter->physical,
            presenter->surface, &caps);
    uint32_t format_count = 0;
    presenter->api.GetPhysicalDeviceSurfaceFormatsKHR(presenter->physical,
            presenter->surface, &format_count, NULL);
    if (!format_count) return false;
    VkSurfaceFormatKHR *formats = calloc(format_count, sizeof(*formats));
    presenter->api.GetPhysicalDeviceSurfaceFormatsKHR(presenter->physical,
            presenter->surface, &format_count, formats);
    VkSurfaceFormatKHR chosen = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
                formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = formats[i];
            break;
        }
    }
    free(formats);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = (uint32_t)ANativeWindow_getWidth(presenter->window);
        extent.height = (uint32_t)ANativeWindow_getHeight(presenter->window);
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & composite)) composite = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR swap_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = presenter->surface,
        .minImageCount = image_count,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = composite,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = presenter->swapchain,
    };
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    if (presenter->api.CreateSwapchainKHR(presenter->device, &swap_info, NULL,
            &replacement) != VK_SUCCESS) {
        return false;
    }
    if (presenter->swapchain) {
        /* Views and framebuffers reference the old swapchain images; they must
         * go before the swapchain itself. */
        presenter_destroy_swapchain_resources(presenter);
        presenter->api.DestroySwapchainKHR(presenter->device, presenter->swapchain, NULL);
    }
    presenter->swapchain = replacement;
    presenter->extent = extent;
    presenter->format = chosen.format;
    if (!presenter_create_swapchain_resources(presenter)) return false;
    LOGI("Vulkan presenter swapchain %ux%u format=%d", extent.width, extent.height,
            chosen.format);
    return true;
}

trierarch_vulkan_presenter *trierarch_vulkan_presenter_create(ANativeWindow *window) {
    if (!window || !trierarch_vulkan_presenter_configured()) return NULL;
    trierarch_vulkan_presenter *presenter = calloc(1, sizeof(*presenter));
    if (!presenter) return NULL;
    presenter->adrenotools = dlopen(probe_adrenotools, RTLD_NOW | RTLD_LOCAL);
    if (!presenter->adrenotools) { free(presenter); return NULL; }
    typedef void *(*open_libvulkan_fn)(int, int, const char *, const char *, const char *,
            const char *, const char *, void **);
    open_libvulkan_fn open_libvulkan = (open_libvulkan_fn)dlsym(presenter->adrenotools,
            "adrenotools_open_libvulkan");
    if (!open_libvulkan) {
        dlclose(presenter->adrenotools);
        free(presenter);
        return NULL;
    }
    void *vulkan_handle = open_libvulkan(RTLD_NOW, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */,
            probe_tmp, probe_hook, probe_driver, "vulkan.ad07xx.so", NULL, NULL);
    if (!vulkan_handle) {
        dlclose(presenter->adrenotools);
        free(presenter);
        return NULL;
    }
    if (!load_instance(&presenter->api, vulkan_handle)) goto fail;

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "TrierarchVulkanPresenter",
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
    if (presenter->api.CreateInstance(&instance_info, NULL, &presenter->instance) != VK_SUCCESS) goto fail;
    if (!load_instance_functions(&presenter->api, presenter->instance)) goto fail;

    VkAndroidSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = window,
    };
    if (presenter->api.CreateAndroidSurfaceKHR(presenter->instance, &surface_info, NULL,
            &presenter->surface) != VK_SUCCESS) goto fail;

    uint32_t device_count = 0;
    presenter->api.EnumeratePhysicalDevices(presenter->instance, &device_count, NULL);
    if (!device_count) goto fail;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    presenter->api.EnumeratePhysicalDevices(presenter->instance, &device_count, &physical);

    uint32_t family_count = 0;
    presenter->api.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    presenter->api.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    presenter->family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++) {
        VkBool32 supported = VK_FALSE;
        presenter->api.GetPhysicalDeviceSurfaceSupportKHR(physical, i, presenter->surface, &supported);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && supported) {
            presenter->family = i;
            break;
        }
    }
    free(families);
    if (presenter->family == UINT32_MAX) goto fail;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = presenter->family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 3, .ppEnabledExtensionNames = device_extensions,
    };
    presenter->physical = physical;
    presenter->api.GetPhysicalDeviceMemoryProperties(physical,
            &presenter->memory_properties);
    presenter->window = window;
    if (presenter->api.CreateDevice(physical, &device_info, NULL, &presenter->device) != VK_SUCCESS)
        goto fail;
    load_device(&presenter->api, presenter->instance);
    presenter->api.GetDeviceQueue(presenter->device, presenter->family, 0, &presenter->queue);

    if (!presenter_create_swapchain(presenter)) goto fail;
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = presenter->family,
    };
    if (presenter->api.CreateCommandPool(presenter->device, &pool_info, NULL,
            &presenter->pool) != VK_SUCCESS) goto fail;
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = presenter->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (presenter->api.AllocateCommandBuffers(presenter->device, &command_info,
            &presenter->command) != VK_SUCCESS) goto fail;
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (presenter->api.CreateSemaphore(presenter->device, &semaphore_info, NULL,
            &presenter->acquired) != VK_SUCCESS) goto fail;
    LOGI("Vulkan presenter created %ux%u format=%d", presenter->extent.width,
            presenter->extent.height, presenter->format);
    return presenter;

fail:
    LOGI("Vulkan presenter creation failed; caller will fall back to EGL");
    destroy_presenter_objects(presenter);
    free(presenter);
    return NULL;
}

static bool presenter_import_cursor(trierarch_vulkan_presenter *presenter,
        const struct trierarch_vulkan_draw *draw, VkImage *out_image,
        VkDeviceMemory *out_memory, VkImageView *out_view) {
    VkFormat vk_format = probe_format(draw->format);
    if (vk_format == VK_FORMAT_UNDEFINED || draw->fd < 0) return false;
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format,
        .extent = {(uint32_t)draw->width, (uint32_t)draw->height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    if (presenter->api.CreateImage(presenter->device, &image_info, NULL, &image) != VK_SUCCESS)
        return false;
    VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (presenter->api.GetMemoryFdPropertiesKHR(presenter->device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, draw->fd, &fd_props) != VK_SUCCESS) {
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    VkMemoryRequirements requirements;
    presenter->api.GetImageMemoryRequirements(presenter->device, image, &requirements);
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image,
    };
    int imported_fd = dup(draw->fd);
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd, .pNext = &dedicated,
    };
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import, .allocationSize = requirements.size, .memoryTypeIndex = 0,
    };
    VkPhysicalDeviceMemoryProperties memory_properties;
    presenter->api.GetPhysicalDeviceMemoryProperties(presenter->physical, &memory_properties);
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
                (fd_props.memoryTypeBits & (1u << i))) {
            allocate.memoryTypeIndex = i;
            break;
        }
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (presenter->api.AllocateMemory(presenter->device, &allocate, NULL, &memory) != VK_SUCCESS) {
        close(imported_fd);
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    if (presenter->api.BindImageMemory(presenter->device, image, memory, 0) != VK_SUCCESS) {
        presenter->api.FreeMemory(presenter->device, memory, NULL);
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vk_format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    if (presenter->api.CreateImageView(presenter->device, &view_info, NULL, &view) != VK_SUCCESS) {
        presenter->api.FreeMemory(presenter->device, memory, NULL);
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    *out_image = image;
    *out_memory = memory;
    *out_view = view;
    return true;
}

static void presenter_destroy_import(trierarch_vulkan_presenter *presenter,
        struct dmabuf_import *import) {
    if (!presenter || !import) return;
    if (presenter->device) {
        if (import->image)
            presenter->api.DestroyImage(presenter->device, import->image, NULL);
        if (import->memory)
            presenter->api.FreeMemory(presenter->device, import->memory, NULL);
    }
    memset(import, 0, sizeof(*import));
}

static void presenter_destroy_imports(trierarch_vulkan_presenter *presenter) {
    if (!presenter) return;
    if (presenter->queue) presenter->api.QueueWaitIdle(presenter->queue);
    for (unsigned int i = 0; i < presenter->import_count; i++)
        presenter_destroy_import(presenter, &presenter->imports[i]);
    presenter->import_count = 0;
}

static struct dmabuf_import *presenter_find_import(
        trierarch_vulkan_presenter *presenter, unsigned long long key,
        int width, int height, unsigned int format, unsigned long long modifier) {
    if (!key) return NULL;
    for (unsigned int i = 0; i < presenter->import_count; i++) {
        struct dmabuf_import *import = &presenter->imports[i];
        if (import->key != key) continue;
        if (import->width == width && import->height == height &&
                import->format == format && import->modifier == modifier)
            return import;
        /* The buffer key was reused for a different description.  Drop the
         * stale image before importing the new one. */
        if (presenter->queue) presenter->api.QueueWaitIdle(presenter->queue);
        presenter_destroy_import(presenter, import);
        presenter->imports[i] = presenter->imports[--presenter->import_count];
        return NULL;
    }
    return NULL;
}

static bool presenter_create_import(trierarch_vulkan_presenter *presenter,
        unsigned long long key, int fd, int width, int height, int stride,
        unsigned int format, unsigned long long modifier,
        struct dmabuf_import *out) {
    (void)stride;
    memset(out, 0, sizeof(*out));
    VkFormat vk_format = probe_format(format);
    if (vk_format == VK_FORMAT_UNDEFINED) return false;
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
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    if (presenter->api.CreateImage(presenter->device, &image_info, NULL, &image) != VK_SUCCESS)
        return false;
    VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (presenter->api.GetMemoryFdPropertiesKHR(presenter->device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props) != VK_SUCCESS) {
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    VkMemoryRequirements requirements;
    presenter->api.GetImageMemoryRequirements(presenter->device, image, &requirements);
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image,
    };
    int imported_fd = dup(fd);
    if (imported_fd < 0) {
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
        .pNext = &dedicated,
    };
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import,
        .allocationSize = requirements.size,
        .memoryTypeIndex = 0,
    };
    for (uint32_t i = 0; i < presenter->memory_properties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
                (fd_props.memoryTypeBits & (1u << i)) &&
                (presenter->memory_properties.memoryTypes[i].propertyFlags &
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocate.memoryTypeIndex = i;
            break;
        }
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (presenter->api.AllocateMemory(presenter->device, &allocate, NULL, &memory) != VK_SUCCESS) {
        close(imported_fd);
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    if (presenter->api.BindImageMemory(presenter->device, image, memory, 0) != VK_SUCCESS) {
        presenter->api.FreeMemory(presenter->device, memory, NULL);
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    out->key = key;
    out->width = width;
    out->height = height;
    out->format = format;
    out->modifier = modifier;
    out->image = image;
    out->memory = memory;
    out->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

static struct dmabuf_import *presenter_get_import(trierarch_vulkan_presenter *presenter,
        unsigned long long key, int fd, int width, int height, int stride,
        unsigned int format, unsigned long long modifier) {
    struct dmabuf_import *import = presenter_find_import(presenter, key,
            width, height, format, modifier);
    if (import) {
        import->last_used = ++presenter->import_clock;
        presenter->import_hits++;
        return import;
    }
    if (presenter->import_count >= VULKAN_IMPORT_CACHE_SIZE) {
        unsigned int oldest = 0;
        for (unsigned int i = 1; i < presenter->import_count; i++) {
            if (presenter->imports[i].last_used < presenter->imports[oldest].last_used)
                oldest = i;
        }
        if (presenter->queue) presenter->api.QueueWaitIdle(presenter->queue);
        presenter_destroy_import(presenter, &presenter->imports[oldest]);
        presenter->imports[oldest] = presenter->imports[--presenter->import_count];
    }
    import = &presenter->imports[presenter->import_count];
    if (!presenter_create_import(presenter, key, fd, width, height, stride,
            format, modifier, import))
        return NULL;
    import->last_used = ++presenter->import_clock;
    presenter->import_count++;
    presenter->import_misses++;
    return import;
}

static int presenter_present_source(trierarch_vulkan_presenter *presenter,
        struct dmabuf_import *import, int width, int height) {
    uint32_t image_index = 0;
    VkResult vk = presenter->api.AcquireNextImageKHR(presenter->device, presenter->swapchain,
            UINT64_MAX, presenter->acquired, VK_NULL_HANDLE, &image_index);
    if (vk == VK_ERROR_OUT_OF_DATE_KHR) {
        LOGI("Vulkan presenter swapchain out of date; recreating");
        if (!presenter_create_swapchain(presenter) ||
                presenter->api.AcquireNextImageKHR(presenter->device, presenter->swapchain,
                        UINT64_MAX, presenter->acquired, VK_NULL_HANDLE, &image_index) != VK_SUCCESS) {
            return -1;
        }
    } else if (vk != VK_SUCCESS) {
        return -1;
    }
    if (image_index >= presenter->swap_image_count || !presenter->swap_images)
        return -1;
    VkImage source = import->image;
    VkImage target = presenter->swap_images[image_index];
    VkImage cursor_image = VK_NULL_HANDLE;
    VkDeviceMemory cursor_memory = VK_NULL_HANDLE;
    VkImageView cursor_view = VK_NULL_HANDLE;
    bool draw_cursor = presenter->cursor_set &&
            presenter_import_cursor(presenter, &presenter->cursor_draw,
                    &cursor_image, &cursor_memory, &cursor_view);
    if (draw_cursor) {
        VkDescriptorImageInfo image_info = {
            .sampler = presenter->cursor_sampler,
            .imageView = cursor_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = presenter->cursor_descriptor,
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        };
        presenter->api.UpdateDescriptorSets(presenter->device, 1, &write, 0, NULL);
    }

    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    presenter->api.BeginCommandBuffer(presenter->command, &begin);
    VkImageMemoryBarrier source_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = import->layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = source,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkImageMemoryBarrier target_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    presenter->api.CmdPipelineBarrier(presenter->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &source_barrier);
    presenter->api.CmdPipelineBarrier(presenter->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &target_barrier);
    if (draw_cursor) {
        VkImageMemoryBarrier cursor_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = cursor_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        presenter->api.CmdPipelineBarrier(presenter->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
                &cursor_barrier);
    }
    VkImageBlit region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {(int32_t)width, (int32_t)height, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {(int32_t)presenter->extent.width,
                (int32_t)presenter->extent.height, 1}},
    };
    presenter->api.CmdBlitImage(presenter->command, source,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    target_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    target_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    if (draw_cursor) {
        target_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        target_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        presenter->api.CmdPipelineBarrier(presenter->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
                &target_barrier);
        VkRenderPassBeginInfo pass_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = presenter->cursor_pass,
            .framebuffer = presenter->swap_framebuffers[image_index],
            .renderArea = {{0, 0}, presenter->extent},
        };
        presenter->api.CmdBeginRenderPass(presenter->command, &pass_begin,
                VK_SUBPASS_CONTENTS_INLINE);
        presenter->api.CmdBindPipeline(presenter->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                presenter->cursor_pipeline);
        presenter->api.CmdBindDescriptorSets(presenter->command,
                VK_PIPELINE_BIND_POINT_GRAPHICS, presenter->cursor_layout, 0, 1,
                &presenter->cursor_descriptor, 0, NULL);
        VkViewport viewport = {0, 0, (float)presenter->extent.width,
                (float)presenter->extent.height, 0, 1};
        VkRect2D scissor = {{0, 0}, presenter->extent};
        presenter->api.CmdSetViewport(presenter->command, 0, 1, &viewport);
        presenter->api.CmdSetScissor(presenter->command, 0, 1, &scissor);
        float w = (float)presenter->extent.width;
        float h = (float)presenter->extent.height;
        float push[8] = {
            presenter->cursor_draw.dst_x / w * 2.0f - 1.0f,
            presenter->cursor_draw.dst_y / h * 2.0f - 1.0f,
            (presenter->cursor_draw.dst_x + presenter->cursor_draw.dst_width) / w * 2.0f - 1.0f,
            (presenter->cursor_draw.dst_y + presenter->cursor_draw.dst_height) / h * 2.0f - 1.0f,
            0.0f, 0.0f, 1.0f, 1.0f,
        };
        presenter->api.CmdPushConstants(presenter->command, presenter->cursor_layout,
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
        presenter->api.CmdDraw(presenter->command, 4, 1, 0, 0);
        presenter->api.CmdEndRenderPass(presenter->command);
    } else {
        target_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presenter->api.CmdPipelineBarrier(presenter->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                &target_barrier);
    }
    presenter->api.EndCommandBuffer(presenter->command);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &presenter->acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &presenter->command,
    };
    vk = presenter->api.QueueSubmit(presenter->queue, 1, &submit, VK_NULL_HANDLE);
    if (vk == VK_SUCCESS) vk = presenter->api.QueueWaitIdle(presenter->queue);
    if (vk == VK_SUCCESS) import->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1, .pSwapchains = &presenter->swapchain,
        .pImageIndices = &image_index,
    };
    bool presented = false;
    if (vk == VK_SUCCESS) {
        vk = presenter->api.QueuePresentKHR(presenter->queue, &present);
        presented = vk == VK_SUCCESS || vk == VK_SUBOPTIMAL_KHR;
    }
    if (presented) {
        /* The source and cursor images are only read by the submitted blit,
         * which the wait above completed.  The swapchain image is owned by the
         * swapchain; the next AcquireNextImageKHR provides the pacing. */
        presenter->frames_presented++;
        int64_t now = presenter_now_ns();
        if (presenter->frames_presented == 1 ||
                now - presenter->last_present_log_ns >= 1000000000LL) {
            presenter->last_present_log_ns = now;
            LOGI("Vulkan frame presented #%llu image=%u size=%ux%u suboptimal=%d "
                    "cache=%llu/%llu",
                    presenter->frames_presented, image_index,
                    presenter->extent.width, presenter->extent.height,
                    vk == VK_SUBOPTIMAL_KHR ? 1 : 0,
                    presenter->import_hits, presenter->import_misses);
        }
        if (vk == VK_SUBOPTIMAL_KHR) presenter->suboptimal_frames++;
    } else if (vk == VK_ERROR_OUT_OF_DATE_KHR) {
        LOGI("Vulkan presenter present out of date; recreating");
        presenter_create_swapchain(presenter);
    } else {
        LOGI("Vulkan presenter frame failed result=%d image=%u", vk, image_index);
    }
    if (draw_cursor) {
        presenter->api.DestroyImageView(presenter->device, cursor_view, NULL);
        presenter->api.FreeMemory(presenter->device, cursor_memory, NULL);
        presenter->api.DestroyImage(presenter->device, cursor_image, NULL);
    }
    presenter->cursor_set = false;
    return vk == VK_SUCCESS ? 0 : -1;
}

int trierarch_vulkan_presenter_present_keyed(trierarch_vulkan_presenter *presenter,
        unsigned long long key, int fd, int width, int height, int stride,
        unsigned int format, unsigned long long modifier) {
    if (!presenter || fd < 0 || width <= 0 || height <= 0) return -1;
    struct dmabuf_import temporary;
    struct dmabuf_import *import = NULL;
    if (key) {
        import = presenter_get_import(presenter, key, fd, width, height, stride,
                format, modifier);
        if (!import) return -1;
    } else {
        if (!presenter_create_import(presenter, 0, fd, width, height, stride,
                format, modifier, &temporary))
            return -1;
        import = &temporary;
    }
    int result = presenter_present_source(presenter, import, width, height);
    if (!key) presenter_destroy_import(presenter, &temporary);
    return result;
}

int trierarch_vulkan_presenter_present(trierarch_vulkan_presenter *presenter,
        int fd, int width, int height, int stride, unsigned int format,
        unsigned long long modifier) {
    return trierarch_vulkan_presenter_present_keyed(presenter, 0, fd, width,
            height, stride, format, modifier);
}

static bool presenter_create_shm_image(trierarch_vulkan_presenter *presenter,
        const void *data, int width, int height, int stride, unsigned int format,
        VkImage *out_image, VkDeviceMemory *out_memory) {
    VkFormat vk_format = probe_format(format);
    if (vk_format == VK_FORMAT_UNDEFINED || !data || width <= 0 || height <= 0) return false;
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format,
        .extent = {(uint32_t)width, (uint32_t)height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };
    VkImage image = VK_NULL_HANDLE;
    if (presenter->api.CreateImage(presenter->device, &image_info, NULL, &image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements;
    presenter->api.GetImageMemoryRequirements(presenter->device, image, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    presenter->api.GetPhysicalDeviceMemoryProperties(presenter->physical, &memory_properties);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) &&
                (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = memory_type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (presenter->api.AllocateMemory(presenter->device, &allocate, NULL, &memory) != VK_SUCCESS) {
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    if (presenter->api.BindImageMemory(presenter->device, image, memory, 0) != VK_SUCCESS) {
        presenter->api.FreeMemory(presenter->device, memory, NULL);
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    presenter->api.GetImageSubresourceLayout(presenter->device, image, &subresource, &layout);
    void *mapped = NULL;
    if (presenter->api.MapMemory(presenter->device, memory, 0, requirements.size, 0,
            &mapped) != VK_SUCCESS) {
        presenter->api.FreeMemory(presenter->device, memory, NULL);
        presenter->api.DestroyImage(presenter->device, image, NULL);
        return false;
    }
    size_t row_bytes = (size_t)width * 4u;
    if (stride > 0 && (size_t)stride < row_bytes) row_bytes = (size_t)stride;
    for (int y = 0; y < height; y++) {
        memcpy((uint8_t *)mapped + layout.offset + (size_t)y * layout.rowPitch,
                (const uint8_t *)data + (size_t)y * stride, row_bytes);
    }
    presenter->api.UnmapMemory(presenter->device, memory);
    *out_image = image;
    *out_memory = memory;
    return true;
}

int trierarch_vulkan_presenter_present_shm(trierarch_vulkan_presenter *presenter,
        const void *data, int width, int height, int stride, unsigned int format) {
    if (!presenter || !data || width <= 0 || height <= 0) return -1;
    VkImage source = VK_NULL_HANDLE;
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
    if (!presenter_create_shm_image(presenter, data, width, height, stride, format,
            &source, &source_memory)) return -1;
    struct dmabuf_import temporary = {
        .width = width,
        .height = height,
        .image = source,
        .memory = source_memory,
        .layout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    int result = presenter_present_source(presenter, &temporary, width, height);
    presenter->api.FreeMemory(presenter->device, source_memory, NULL);
    presenter->api.DestroyImage(presenter->device, source, NULL);
    return result;
}

void trierarch_vulkan_presenter_set_cursor(trierarch_vulkan_presenter *presenter,
        const struct trierarch_vulkan_draw *cursor) {
    if (!presenter) return;
    if (cursor && cursor->fd >= 0 && cursor->width > 0 && cursor->height > 0) {
        presenter->cursor_draw = *cursor;
        presenter->cursor_set = true;
    } else {
        presenter->cursor_set = false;
    }
}

int trierarch_vulkan_presenter_present_scene(trierarch_vulkan_presenter *presenter,
        const struct trierarch_vulkan_draw *root,
        const struct trierarch_vulkan_draw *cursor) {
    if (!presenter || !root) return -1;
    trierarch_vulkan_presenter_set_cursor(presenter, cursor);
    return trierarch_vulkan_presenter_present(presenter, root->fd, root->width,
            root->height, root->stride, root->format, root->modifier);
}

void trierarch_vulkan_presenter_wait_idle(trierarch_vulkan_presenter *presenter) {
    if (presenter && presenter->queue) presenter->api.QueueWaitIdle(presenter->queue);
}

void trierarch_vulkan_presenter_destroy(trierarch_vulkan_presenter *presenter) {
    if (!presenter) return;
    destroy_presenter_objects(presenter);
    free(presenter);
}
