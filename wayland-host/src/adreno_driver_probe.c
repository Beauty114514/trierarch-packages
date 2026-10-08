#include "adreno_driver_probe.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

enum { ADRENOTOOLS_DRIVER_CUSTOM = 1 << 0 };
typedef void *(*open_libvulkan_fn)(int, int, const char *, const char *, const char *,
        const char *, const char *, void **);

struct result_buffer {
    char *text;
    size_t capacity;
    size_t length;
};

static void append(struct result_buffer *result, const char *format, ...) {
    if (result->length >= result->capacity) return;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(result->text + result->length, result->capacity - result->length,
            format, arguments);
    va_end(arguments);
    if (written > 0) {
        size_t amount = (size_t)written;
        result->length += amount < result->capacity - result->length
                ? amount : result->capacity - result->length;
    }
}

static bool has_extension(const VkExtensionProperties *extensions, uint32_t count,
        const char *name) {
    for (uint32_t index = 0; index < count; index++) {
        if (strcmp(extensions[index].extensionName, name) == 0) return true;
    }
    return false;
}

char *trierarch_adreno_probe_driver(const char *hook_library_dir,
        const char *driver_dir, const char *driver_name) {
    struct result_buffer result = {
        .capacity = 4096,
        .text = calloc(1, 4096),
    };
    if (!result.text) return NULL;

    void *adrenotools = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
    if (!adrenotools) {
        append(&result, "result=error stage=load-adrenotools detail=%s\n", dlerror());
        return result.text;
    }
    open_libvulkan_fn open_libvulkan = (open_libvulkan_fn)dlsym(adrenotools,
            "adrenotools_open_libvulkan");
    if (!open_libvulkan) {
        append(&result, "result=error stage=find-adrenotools-entry detail=%s\n", dlerror());
        dlclose(adrenotools);
        return result.text;
    }

    void *vulkan = open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
            NULL, hook_library_dir, driver_dir, driver_name, NULL, NULL);
    if (!vulkan) {
        append(&result, "result=error stage=open-custom-vulkan\n");
        dlclose(adrenotools);
        return result.text;
    }
    PFN_vkGetInstanceProcAddr get_instance_proc_addr =
            (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = get_instance_proc_addr
            ? (PFN_vkCreateInstance)get_instance_proc_addr(NULL, "vkCreateInstance") : NULL;
    if (!create_instance) {
        append(&result, "result=error stage=find-vulkan-entry\n");
        dlclose(vulkan);
        dlclose(adrenotools);
        return result.text;
    }

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Trierarch Adreno probe",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult created = create_instance(&info, NULL, &instance);
    if (created != VK_SUCCESS) {
        append(&result, "result=error stage=create-instance vk-result=%d\n", created);
        dlclose(vulkan);
        dlclose(adrenotools);
        return result.text;
    }

    PFN_vkEnumeratePhysicalDevices enumerate_devices =
            (PFN_vkEnumeratePhysicalDevices)get_instance_proc_addr(instance,
                    "vkEnumeratePhysicalDevices");
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions =
            (PFN_vkEnumerateDeviceExtensionProperties)get_instance_proc_addr(instance,
                    "vkEnumerateDeviceExtensionProperties");
    PFN_vkDestroyInstance destroy_instance = (PFN_vkDestroyInstance)get_instance_proc_addr(instance,
            "vkDestroyInstance");
    uint32_t device_count = 0;
    if (!enumerate_devices || !enumerate_extensions || !destroy_instance ||
            enumerate_devices(instance, &device_count, NULL) != VK_SUCCESS || device_count == 0) {
        append(&result, "result=error stage=enumerate-devices\n");
        destroy_instance(instance, NULL);
        dlclose(vulkan);
        dlclose(adrenotools);
        return result.text;
    }
    VkPhysicalDevice *devices = calloc(device_count, sizeof(*devices));
    uint32_t extension_count = 0;
    VkExtensionProperties *extensions = NULL;
    if (!devices || enumerate_devices(instance, &device_count, devices) != VK_SUCCESS ||
            enumerate_extensions(devices[0], NULL, &extension_count, NULL) != VK_SUCCESS ||
            !(extensions = calloc(extension_count, sizeof(*extensions))) ||
            enumerate_extensions(devices[0], NULL, &extension_count, extensions) != VK_SUCCESS) {
        append(&result, "result=error stage=enumerate-device-extensions\n");
    } else {
        const char *required[] = {
            "VK_EXT_external_memory_dma_buf",
            "VK_EXT_image_drm_format_modifier",
            "VK_KHR_external_memory_fd",
            "VK_ANDROID_external_memory_android_hardware_buffer",
            "VK_KHR_external_semaphore_fd",
            "VK_KHR_external_fence_fd",
        };
        append(&result, "result=ok devices=%u extensions=%u\n", device_count, extension_count);
        for (size_t index = 0; index < sizeof(required) / sizeof(required[0]); index++) {
            append(&result, "extension %s=%s\n", required[index],
                    has_extension(extensions, extension_count, required[index]) ? "yes" : "no");
        }
    }
    free(extensions);
    free(devices);
    destroy_instance(instance, NULL);
    dlclose(vulkan);
    dlclose(adrenotools);
    return result.text;
}
