#ifndef VELOX_VULKAN_UTIL_H
#define VELOX_VULKAN_UTIL_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <time.h>
#endif

_Noreturn static void die(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

#define CHECK(expression) do { \
    VkResult result = (expression); \
    if (result != VK_SUCCESS) { \
        fprintf(stderr, "%s failed: %d\n", #expression, result); \
        exit(1); \
    } \
} while (0)

#ifndef _WIN32
/* Android の Vulkan HAL（hardware/hwvulkan.h）を直接開くための最小の定義。
 * レイアウトは AOSP の hardware/hardware.h と hwvulkan.h（LP64）に一致させている。 */
struct velox_hw_module_t;
struct velox_hw_device_t {
    uint32_t tag, version;
    struct velox_hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct velox_hw_device_t *device);
};
struct velox_hw_module_methods_t {
    int (*open)(const struct velox_hw_module_t *module, const char *id, struct velox_hw_device_t **device);
};
struct velox_hw_module_t {
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    struct velox_hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};
struct velox_hwvulkan_device_t {
    struct velox_hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};
#endif

/* 既定はシステムの Vulkan ローダー。Android で環境変数 VELOX_VK_HAL にドライバー（例:
 * /data/local/tmp/vulkan.ranchu.so）を指定すると、ローダーを通さずにその HAL を直接使う。
 * 同じ起動中に標準ドライバーと試作ドライバーを交互に測るためのもの。この場合、同じプロセスで
 * libvulkan.so を読み込まない（2 つの gfxstream ドライバーが同時に動くのを避ける）。 */
static PFN_vkGetInstanceProcAddr load_vulkan(void) {
#ifdef _WIN32
    HMODULE library = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return library ? (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr") : NULL;
#else
    const char *hal = getenv("VELOX_VK_HAL");
    if (hal && *hal) {
        void *library = dlopen(hal, RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            fprintf(stderr, "dlopen %s: %s\n", hal, dlerror());
            return NULL;
        }
        const struct velox_hw_module_t *module = dlsym(library, "HMI");
        struct velox_hw_device_t *device = NULL;
        if (!module || !module->methods || module->methods->open(module, "vk0", &device) != 0 || !device) {
            fprintf(stderr, "%s is not a Vulkan HAL module\n", hal);
            return NULL;
        }
        return ((struct velox_hwvulkan_device_t *)device)->GetInstanceProcAddr;
    }
    void *library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return library ? (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr") : NULL;
#endif
}

static double clock_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart * 1e9 / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1e9 + now.tv_nsec;
#endif
}

/* The instance dispatcher also resolves core Vulkan 1.3 device entry points. */
#define LOAD(name) PFN_##name name = (PFN_##name)lookup(instance, #name); \
                   if (!name) die("Missing entry point " #name)

static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *properties,
                            uint32_t allowed, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < properties->memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) &&
            (properties->memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    die("No suitable memory type");
}

#endif
