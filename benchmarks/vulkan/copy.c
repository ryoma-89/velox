/* Identical Vulkan 1.3 copy, synchronization2 and timeline workload on Windows/Android. */
#include "vulkan_util.h"

enum { BUFFER_BYTES = 64 * 1024 * 1024, COPIES = 8, TRIALS = 5 };

static void barrier(PFN_vkCmdPipelineBarrier2 command, VkCommandBuffer cb,
                    VkPipelineStageFlags2 destination, VkAccessFlags2 access) {
    VkMemoryBarrier2 memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStageMask = destination,
        .dstAccessMask = access,
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &memory,
    };
    command(cb, &dependency);
}

int main(int argc, char **argv) {
    char *end = NULL;
    unsigned long index = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc > 2 || (argc == 2 && (end == argv[1] || *end))) die("usage: vk-copy [device-index]");
    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Velox Vulkan copy", .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application,
    };
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    LOAD(vkDestroyInstance);
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (index >= count) die("Physical device index does not exist");
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) die("Could not allocate physical device list");
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = devices[index];
    free(devices);

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3 || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        die("A Vulkan 1.3 hardware device is required");
    VkPhysicalDeviceSynchronization2Features sync = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
    };
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext = &sync,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &timeline,
    };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!sync.synchronization2 || !timeline.timelineSemaphore)
        die("synchronization2 and timelineSemaphore are required");

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) die("Could not allocate queue family list");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count && (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                              || !families[family].timestampValidBits)) ++family;
    if (family == count) die("No graphics queue with GPU timestamps");
    uint32_t timestamp_bits = families[family].timestampValidBits;
    free(families);
    uint64_t timestamp_mask = timestamp_bits == 64 ? UINT64_MAX : (UINT64_C(1) << timestamp_bits) - 1;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &timeline,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    VkDevice device;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    LOAD(vkDestroyDevice);
    LOAD(vkGetDeviceQueue);
    LOAD(vkCreateBuffer);
    LOAD(vkDestroyBuffer);
    LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkAllocateMemory);
    LOAD(vkFreeMemory);
    LOAD(vkBindBufferMemory);
    LOAD(vkMapMemory);
    LOAD(vkUnmapMemory);
    LOAD(vkInvalidateMappedMemoryRanges);
    LOAD(vkCreateCommandPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkResetCommandPool);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCreateSemaphore);
    LOAD(vkDestroySemaphore);
    LOAD(vkCreateQueryPool);
    LOAD(vkDestroyQueryPool);
    LOAD(vkGetQueryPoolResults);
    LOAD(vkCmdResetQueryPool);
    LOAD(vkCmdWriteTimestamp2);
    LOAD(vkCmdFillBuffer);
    LOAD(vkCmdCopyBuffer);
    LOAD(vkCmdPipelineBarrier2);
    LOAD(vkQueueSubmit2);
    LOAD(vkWaitSemaphores);

    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkBuffer buffers[3];
    VkDeviceMemory memories[3];
    uint32_t types[3];
    for (size_t i = 0; i < 3; ++i) {
        VkBufferCreateInfo buffer = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BUFFER_BYTES,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        CHECK(vkCreateBuffer(device, &buffer, NULL, &buffers[i]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        types[i] = memory_type(&memory_properties, requirements.memoryTypeBits,
                                i == 2 ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = types[i],
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[i]));
        CHECK(vkBindBufferMemory(device, buffers[i], memories[i], 0));
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
    };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo cb_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(device, &cb_info, &cb));
    VkSemaphoreTypeCreateInfo semaphore_type = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &semaphore_type,
    };
    VkSemaphore semaphore;
    CHECK(vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore));
    VkQueryPoolCreateInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
    };
    VkQueryPool query;
    CHECK(vkCreateQueryPool(device, &query_info, NULL, &query));

    void *mapped;
    CHECK(vkMapMemory(device, memories[2], 0, VK_WHOLE_SIZE, 0, &mapped));
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = memories[2], .size = VK_WHOLE_SIZE,
    };
    double gpu_ns[TRIALS], completion_ns[TRIALS];
    for (unsigned trial = 0; trial <= TRIALS; ++trial) {
        CHECK(vkResetCommandPool(device, pool, 0));
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        CHECK(vkBeginCommandBuffer(cb, &begin));
        vkCmdResetQueryPool(cb, query, 0, 2);
        uint32_t pattern = UINT32_C(0x893b32df) ^ (trial * UINT32_C(0x01010101));
        vkCmdFillBuffer(cb, buffers[0], 0, BUFFER_BYTES, pattern);
        vkCmdFillBuffer(cb, buffers[1], 0, BUFFER_BYTES, ~pattern);
        barrier(vkCmdPipelineBarrier2, cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);

        /* Copy the source into the alternate buffer, then back, and repeat. */
        VkBufferCopy region = {.size = BUFFER_BYTES};
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, query, 0);
        for (unsigned copy = 0; copy < COPIES; ++copy) {
            vkCmdCopyBuffer(cb, buffers[copy % 2], buffers[(copy + 1) % 2], 1, &region);
            barrier(vkCmdPipelineBarrier2, cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, query, 1);
        vkCmdCopyBuffer(cb, buffers[1], buffers[2], 1, &region);
        barrier(vkCmdPipelineBarrier2, cb, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(cb));
        uint64_t value = trial + 1;
        VkCommandBufferSubmitInfo cmd = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        VkSemaphoreSubmitInfo signal = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = semaphore,
            .value = value, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        VkSubmitInfo2 submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd,
            .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal,
        };
        VkSemaphoreWaitInfo wait = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
            .pSemaphores = &semaphore, .pValues = &value,
        };
        double start = clock_ns();
        CHECK(vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkWaitSemaphores(device, &wait, UINT64_C(10000000000)));
        double elapsed = clock_ns() - start;

        uint64_t ticks[2];
        CHECK(vkGetQueryPoolResults(device, query, 0, 2, sizeof(ticks), ticks,
                                    sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT));
        CHECK(vkInvalidateMappedMemoryRanges(device, 1, &range));
        const uint32_t *words = mapped;
        for (size_t i = 0; i < BUFFER_BYTES / sizeof(*words); ++i) {
            if (words[i] != pattern) {
                fprintf(stderr, "Readback failed at word %zu: %08" PRIx32 " != %08" PRIx32 "\n",
                        i, words[i], pattern);
                return 2;
            }
        }
        if (trial) {
            gpu_ns[trial - 1] = (double)((ticks[1] - ticks[0]) & timestamp_mask) * properties.limits.timestampPeriod;
            completion_ns[trial - 1] = elapsed;
            if (gpu_ns[trial - 1] <= 0 || elapsed <= 0) die("Invalid timestamps");
        }
    }

    printf("{\"validated\":true,\"vendor_id\":%u,\"device_id\":%u,\"api_version\":%u,"
           "\"memory_type_indices\":[%u,%u,%u],\"buffer_bytes\":%u,\"copies\":%u,\"trials\":[",
           properties.vendorID, properties.deviceID, properties.apiVersion,
           types[0], types[1], types[2], BUFFER_BYTES, COPIES);
    for (size_t i = 0; i < TRIALS; ++i)
        printf("%s{\"gpu_ns\":%.0f,\"submit_wait_ns\":%.0f}", i ? "," : "", gpu_ns[i], completion_ns[i]);
    puts("]}");
    vkUnmapMemory(device, memories[2]);
    vkDestroyQueryPool(device, query, NULL);
    vkDestroySemaphore(device, semaphore, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    for (size_t i = 0; i < 3; ++i) {
        vkDestroyBuffer(device, buffers[i], NULL);
        vkFreeMemory(device, memories[i], NULL);
    }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
