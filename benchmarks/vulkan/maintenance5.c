/* VK_KHR_maintenance5 の列挙、明示的な有効化、KHR／コアのコマンド経路を確認する。 */
#include "vulkan_util.h"
#include <stdbool.h>
#include <string.h>

int main(int argc, char **argv) {
    const bool core14 = argc == 2 && !strcmp(argv[1], "--core14");
    if (argc > 2 || (argc == 2 && !core14)) die("usage: vk-maintenance5 [--core14]");
    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(vkEnumerateInstanceVersion);
    uint32_t loader_version;
    CHECK(vkEnumerateInstanceVersion(&loader_version));
    const uint32_t requested = core14 ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3;
    if (loader_version < requested) {
        printf("{\"validated\":false,\"loader_api\":%u,\"requested_api\":%u}\n", loader_version, requested);
        return 77;
    }
    LOAD(vkCreateInstance);
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Velox maintenance5 test", .apiVersion = requested};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application};
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    LOAD(vkDestroyInstance); LOAD(vkEnumeratePhysicalDevices); LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkEnumerateDeviceExtensionProperties); LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties); LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkCreateDevice); LOAD(vkGetDeviceProcAddr);

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) die("No Vulkan physical devices");
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) die("Device list allocation failed");
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    const VkPhysicalDevice physical = devices[0];
    free(devices);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);

    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) die("Extension list allocation failed");
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    bool advertised = false;
    for (uint32_t i = 0; i < count; ++i)
        advertised |= !strcmp(extensions[i].extensionName, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    free(extensions);
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceVulkan14Features core = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    void *feature_chain = core14 ? (void *)&core : (void *)&maintenance;
    if (properties.apiVersion < requested || (!core14 && !advertised)) {
        printf("{\"validated\":false,\"device_api\":%u,\"maintenance5_advertised\":%u}\n",
               properties.apiVersion, (unsigned)advertised);
        vkDestroyInstance(instance, NULL);
        return 77;
    }
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = feature_chain};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    const VkBool32 supported = core14 ? core.maintenance5 : maintenance.maintenance5;
    if (!supported) {
        puts("{\"validated\":false,\"maintenance5_feature\":0}");
        vkDestroyInstance(instance, NULL);
        return 77;
    }
    core = (VkPhysicalDeviceVulkan14Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .maintenance5 = VK_TRUE,
    };

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (!count) die("No queue families");
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) die("Queue family list allocation failed");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    free(families);
    if (family >= count) die("Graphics queue unavailable");
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *name = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    const VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = feature_chain,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = core14 ? 0 : 1, .ppEnabledExtensionNames = core14 ? NULL : &name};
    VkDevice device;
    const VkResult created = vkCreateDevice(physical, &info, NULL, &device);
    if (created != VK_SUCCESS) {
        printf("{\"validated\":false,\"device_api\":%u,\"maintenance5_advertised\":%u,"
               "\"maintenance5_feature\":%u,\"create_device_result\":%d}\n", properties.apiVersion,
               (unsigned)advertised, supported, created);
        vkDestroyInstance(instance, NULL);
        return 1;
    }

#define DEVICE(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name); if (!name) die("Missing " #name)
    DEVICE(vkDestroyDevice); DEVICE(vkCreateBuffer); DEVICE(vkDestroyBuffer);
    DEVICE(vkAllocateMemory); DEVICE(vkFreeMemory); DEVICE(vkBindBufferMemory); DEVICE(vkGetBufferMemoryRequirements);
    DEVICE(vkCreateCommandPool); DEVICE(vkDestroyCommandPool); DEVICE(vkAllocateCommandBuffers);
    DEVICE(vkBeginCommandBuffer); DEVICE(vkEndCommandBuffer); DEVICE(vkGetDeviceQueue);
    DEVICE(vkQueueSubmit); DEVICE(vkQueueWaitIdle); DEVICE(vkCreateImage); DEVICE(vkDestroyImage);
#undef DEVICE
#define ALIAS(name) PFN_##name##KHR name = (PFN_##name##KHR)vkGetDeviceProcAddr(device, core14 ? #name : #name "KHR"); \
                    if (!name) die("Missing " #name)
    ALIAS(vkCmdBindIndexBuffer2); ALIAS(vkGetRenderingAreaGranularity);
    ALIAS(vkGetDeviceImageSubresourceLayout); ALIAS(vkGetImageSubresourceLayout2);
#undef ALIAS

    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkRenderingAreaInfoKHR area = {.sType = VK_STRUCTURE_TYPE_RENDERING_AREA_INFO_KHR,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    VkExtent2D granularity = {0};
    vkGetRenderingAreaGranularity(device, &area, &granularity);
    if (!granularity.width || !granularity.height) die("Invalid rendering area granularity");
    const VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {64, 32, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    const VkImageSubresource2KHR plane = {.sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT}};
    const VkDeviceImageSubresourceInfoKHR image_subresource = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO_KHR,
        .pCreateInfo = &image_info, .pSubresource = &plane};
    VkSubresourceLayout2KHR expected = {.sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR};
    VkSubresourceLayout2KHR observed = {.sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR};
    VkImage image;
    CHECK(vkCreateImage(device, &image_info, NULL, &image));
    vkGetDeviceImageSubresourceLayout(device, &image_subresource, &expected);
    vkGetImageSubresourceLayout2(device, image, &plane, &observed);
    if (expected.subresourceLayout.rowPitch < 256 || expected.subresourceLayout.size < 8192 ||
        expected.subresourceLayout.offset != observed.subresourceLayout.offset ||
        expected.subresourceLayout.rowPitch != observed.subresourceLayout.rowPitch ||
        expected.subresourceLayout.size != observed.subresourceLayout.size) {
        die("Image layout query mismatch");
    }

    const VkBufferUsageFlags2CreateInfoKHR usage = {.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
        .usage = VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR};
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &usage, .size = 256};
    VkBuffer buffer;
    VkDeviceMemory memory;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties memory_info;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_info);
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(&memory_info, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(device, &allocate, NULL, &memory));
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdBindIndexBuffer2(command, buffer, 0, 128, VK_INDEX_TYPE_UINT16);
    CHECK(vkEndCommandBuffer(command));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    const double start = clock_ns();
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    printf("{\"validated\":true,\"device_api\":%u,\"entry_points\":\"%s\",\"row_pitch\":%" PRIu64
           ",\"granularity\":[%u,%u],\"submit_wait_ns\":%.0f}\n", properties.apiVersion, core14 ? "core14" : "KHR",
           expected.subresourceLayout.rowPitch, granularity.width, granularity.height, clock_ns() - start);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyImage(device, image, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
