/* Identical Vulkan 1.3 host-visible memory and synchronization workload on Windows/Android.
 *
 * For every HOST_VISIBLE memory type that a transfer buffer may use, the CPU writes a pattern
 * into the mapping (memcpy, like an app's upload path) and reads part of it back. The GPU then
 * copies the buffer into a separate readback buffer, and the CPU checks every word, so a result
 * is only reported when the written data reached the GPU intact.
 *
 * It also measures the cost of a submit + wait with an empty command buffer, a host round trip
 * (vkGetSemaphoreCounterValue), and recording and executing many small commands.
 */
#include <string.h>
#include "vulkan_util.h"

enum {
    BUFFER_BYTES = 64 * 1024 * 1024,
    READ_BYTES = 16 * 1024 * 1024,
    TRIALS = 5,
    SYNC_ITERATIONS = 200,
    QUERY_CALLS = 2000,
    SMALL_COMMANDS = 20000,
    MAX_TYPES = VK_MAX_MEMORY_TYPES,
};

static void fill_pattern(uint32_t *words, size_t count, uint32_t seed) {
    for (size_t i = 0; i < count; ++i) words[i] = seed ^ (uint32_t)(i * UINT32_C(2654435761));
}

static void print_array(const char *name, const double *values, size_t count, int last) {
    printf("\"%s\":[", name);
    for (size_t i = 0; i < count; ++i) printf("%s%.3f", i ? "," : "", values[i]);
    printf("]%s", last ? "" : ",");
}

int main(int argc, char **argv) {
    char *end = NULL;
    unsigned long index = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc > 2 || (argc == 2 && (end == argv[1] || *end))) die("usage: vk-upload [device-index]");
    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Velox Vulkan upload", .apiVersion = VK_API_VERSION_1_3,
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
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &timeline,
    };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!timeline.timelineSemaphore) die("timelineSemaphore is required");

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) die("Could not allocate queue family list");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count) die("No graphics queue");
    free(families);

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
    LOAD(vkFlushMappedMemoryRanges);
    LOAD(vkInvalidateMappedMemoryRanges);
    LOAD(vkCreateCommandPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkResetCommandPool);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCreateSemaphore);
    LOAD(vkDestroySemaphore);
    LOAD(vkGetSemaphoreCounterValue);
    LOAD(vkWaitSemaphores);
    LOAD(vkCmdFillBuffer);
    LOAD(vkCmdCopyBuffer);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkQueueSubmit);

    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

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
    uint64_t signal_value = 0;

#define SUBMIT_AND_WAIT(with_cb) do { \
        uint64_t value_ = ++signal_value; \
        VkTimelineSemaphoreSubmitInfo timeline_ = { \
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, \
            .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value_, \
        }; \
        VkSubmitInfo submit_ = { \
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline_, \
            .commandBufferCount = (with_cb) ? 1 : 0, .pCommandBuffers = &cb, \
            .signalSemaphoreCount = 1, .pSignalSemaphores = &semaphore, \
        }; \
        VkSemaphoreWaitInfo wait_ = { \
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, \
            .pSemaphores = &semaphore, .pValues = &value_, \
        }; \
        CHECK(vkQueueSubmit(queue, 1, &submit_, VK_NULL_HANDLE)); \
        CHECK(vkWaitSemaphores(device, &wait_, UINT64_C(10000000000))); \
    } while (0)

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkMemoryBarrier to_host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };

    /* Readback buffer: prefer cached host memory so CPU verification is not the bottleneck. */
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BUFFER_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer readback;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &readback));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, readback, &requirements);
    uint32_t allowed = requirements.memoryTypeBits;
    uint32_t readback_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount && readback_type == UINT32_MAX; ++i) {
        VkMemoryPropertyFlags f = memory_properties.memoryTypes[i].propertyFlags;
        if ((allowed & (1u << i)) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) readback_type = i;
    }
    if (readback_type == UINT32_MAX)
        readback_type = memory_type(&memory_properties, allowed, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = readback_type,
    };
    VkDeviceMemory readback_memory;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &readback_memory));
    CHECK(vkBindBufferMemory(device, readback, readback_memory, 0));
    void *readback_map;
    CHECK(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &readback_map));
    VkMappedMemoryRange readback_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = readback_memory, .size = VK_WHOLE_SIZE,
    };

    uint32_t *source = malloc(BUFFER_BYTES);
    if (!source) die("Could not allocate source data");
    const size_t words = BUFFER_BYTES / sizeof(uint32_t);

    uint32_t tested[MAX_TYPES];
    double write_gb_s[MAX_TYPES][TRIALS], read_gb_s[MAX_TYPES][TRIALS];
    uint32_t tested_count = 0;
    volatile uint64_t sink = 0;
    for (uint32_t type = 0; type < memory_properties.memoryTypeCount; ++type) {
        VkMemoryPropertyFlags flags = memory_properties.memoryTypes[type].propertyFlags;
        if (!(allowed & (1u << type)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        VkBuffer upload;
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &upload));
        VkMemoryRequirements upload_requirements;
        vkGetBufferMemoryRequirements(device, upload, &upload_requirements);
        VkMemoryAllocateInfo upload_allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = upload_requirements.size, .memoryTypeIndex = type,
        };
        VkDeviceMemory upload_memory;
        if (vkAllocateMemory(device, &upload_allocation, NULL, &upload_memory) != VK_SUCCESS) {
            /* e.g. a small ReBAR heap; record nothing for this type. */
            vkDestroyBuffer(device, upload, NULL);
            continue;
        }
        CHECK(vkBindBufferMemory(device, upload, upload_memory, 0));
        void *mapped;
        CHECK(vkMapMemory(device, upload_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
        VkMappedMemoryRange upload_range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = upload_memory, .size = VK_WHOLE_SIZE,
        };
        for (unsigned trial = 0; trial <= TRIALS; ++trial) {
            uint32_t seed = UINT32_C(0x9e3779b9) ^ (type << 24) ^ trial;
            fill_pattern(source, words, seed);
            double start = clock_ns();
            memcpy(mapped, source, BUFFER_BYTES);
            if (!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                CHECK(vkFlushMappedMemoryRanges(device, 1, &upload_range));
            double write_ns = clock_ns() - start;

            start = clock_ns();
            const uint64_t *qwords = mapped;
            uint64_t sum = 0;
            for (size_t i = 0; i < READ_BYTES / sizeof(uint64_t); ++i) sum += qwords[i];
            sink += sum;
            double read_ns = clock_ns() - start;

            /* GPU copy into the readback buffer, then verify every word. */
            CHECK(vkResetCommandPool(device, pool, 0));
            CHECK(vkBeginCommandBuffer(cb, &begin));
            VkBufferCopy region = {.size = BUFFER_BYTES};
            vkCmdCopyBuffer(cb, upload, readback, 1, &region);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 1, &to_host, 0, NULL, 0, NULL);
            CHECK(vkEndCommandBuffer(cb));
            SUBMIT_AND_WAIT(1);
            CHECK(vkInvalidateMappedMemoryRanges(device, 1, &readback_range));
            if (memcmp(readback_map, source, BUFFER_BYTES) != 0) {
                fprintf(stderr, "GPU readback of memory type %u differs from the CPU upload\n", type);
                return 2;
            }
            if (trial) {
                write_gb_s[tested_count][trial - 1] = BUFFER_BYTES / write_ns;
                read_gb_s[tested_count][trial - 1] = READ_BYTES / read_ns;
            }
        }
        tested[tested_count++] = type;
        vkUnmapMemory(device, upload_memory);
        vkDestroyBuffer(device, upload, NULL);
        vkFreeMemory(device, upload_memory, NULL);
    }
    if (!tested_count) die("No host visible memory type could be tested");

    /* Submit + wait with an empty command buffer: queue and fence round trip. */
    double empty_submit_ns[TRIALS], query_ns[TRIALS], record_ns[TRIALS], small_submit_ns[TRIALS];
    CHECK(vkResetCommandPool(device, pool, 0));
    CHECK(vkBeginCommandBuffer(cb, &begin));
    CHECK(vkEndCommandBuffer(cb));
    for (unsigned trial = 0; trial <= TRIALS; ++trial) {
        double start = clock_ns();
        for (unsigned i = 0; i < SYNC_ITERATIONS; ++i) SUBMIT_AND_WAIT(1);
        double per = (clock_ns() - start) / SYNC_ITERATIONS;
        uint64_t value = 0;
        start = clock_ns();
        for (unsigned i = 0; i < QUERY_CALLS; ++i) CHECK(vkGetSemaphoreCounterValue(device, semaphore, &value));
        double per_query = (clock_ns() - start) / QUERY_CALLS;
        if (value != signal_value) die("Semaphore counter mismatch");
        if (trial) {
            empty_submit_ns[trial - 1] = per;
            query_ns[trial - 1] = per_query;
        }
    }

    /* Many small commands: each fills its own 4-byte word, then the result is verified. */
    for (unsigned trial = 0; trial <= TRIALS; ++trial) {
        CHECK(vkResetCommandPool(device, pool, 0));
        double start = clock_ns();
        CHECK(vkBeginCommandBuffer(cb, &begin));
        for (uint32_t i = 0; i < SMALL_COMMANDS; ++i)
            vkCmdFillBuffer(cb, readback, (VkDeviceSize)i * 4, 4, i ^ (trial * UINT32_C(0x01000193)));
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             1, &to_host, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(cb));
        double recorded = clock_ns();
        SUBMIT_AND_WAIT(1);
        double done = clock_ns();
        CHECK(vkInvalidateMappedMemoryRanges(device, 1, &readback_range));
        const uint32_t *result = readback_map;
        for (uint32_t i = 0; i < SMALL_COMMANDS; ++i) {
            if (result[i] != (i ^ (trial * UINT32_C(0x01000193)))) {
                fprintf(stderr, "Small command %u produced a wrong value\n", i);
                return 2;
            }
        }
        if (trial) {
            record_ns[trial - 1] = recorded - start;
            small_submit_ns[trial - 1] = done - recorded;
        }
    }

    printf("{\"validated\":true,\"vendor_id\":%u,\"device_id\":%u,\"api_version\":%u,"
           "\"buffer_bytes\":%u,\"read_bytes\":%u,\"readback_type\":%u,\"readback_flags\":%u,"
           "\"sync_iterations\":%u,\"query_calls\":%u,\"small_commands\":%u,\"memory_types\":[",
           properties.vendorID, properties.deviceID, properties.apiVersion, BUFFER_BYTES, READ_BYTES,
           readback_type, memory_properties.memoryTypes[readback_type].propertyFlags,
           SYNC_ITERATIONS, QUERY_CALLS, SMALL_COMMANDS);
    for (uint32_t t = 0; t < tested_count; ++t) {
        uint32_t type = tested[t];
        printf("%s{\"index\":%u,\"flags\":%u,\"heap\":%u,", t ? "," : "", type,
               memory_properties.memoryTypes[type].propertyFlags, memory_properties.memoryTypes[type].heapIndex);
        print_array("write_gb_s", write_gb_s[t], TRIALS, 0);
        print_array("read_gb_s", read_gb_s[t], TRIALS, 1);
        printf("}");
    }
    printf("],");
    print_array("empty_submit_wait_ns", empty_submit_ns, TRIALS, 0);
    print_array("semaphore_query_ns", query_ns, TRIALS, 0);
    print_array("small_record_ns", record_ns, TRIALS, 0);
    print_array("small_submit_wait_ns", small_submit_ns, TRIALS, 1);
    printf(",\"sink\":%" PRIu64 "}\n", (uint64_t)(sink & 1));

    free(source);
    vkUnmapMemory(device, readback_memory);
    vkDestroyBuffer(device, readback, NULL);
    vkFreeMemory(device, readback_memory, NULL);
    vkDestroySemaphore(device, semaphore, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
