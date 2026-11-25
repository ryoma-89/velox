/* Verify real shader access via Vulkan buffer device addresses on Windows and Android. */
#include "vulkan_util.h"
#include <stddef.h>

enum { ELEMENTS = 1024 * 1024, WORKGROUP_SIZE = 64, STEPS = 32, TRIALS = 5 };

typedef struct {
    VkDeviceAddress src, dst;
    uint32_t seed;
} Params;

_Static_assert(offsetof(Params, seed) == 16, "Vulkan push constant offsets");

static const uint32_t shader_code[] =
#include "compute.spv.h"
;

static uint32_t reference(uint32_t input, uint32_t seed) {
    uint32_t state = input ^ seed;
    uint32_t value = state & 1023u;
    for (unsigned step = 0; step < STEPS; ++step) {
        state = state * 1664525u + 1013904223u;
        value = (value + (state >> 29u)) & 1023u;
    }
    return state ^ value;
}

int main(int argc, char **argv) {
    char *end = NULL;
    unsigned long index = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc > 2 || (argc == 2 && (end == argv[1] || *end))) die("usage: vk-compute [device-index]");
    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Velox Vulkan addressed compute", .apiVersion = VK_API_VERSION_1_3,
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
    VkPhysicalDeviceBufferDeviceAddressFeatures address = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .pNext = &sync,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &address,
    };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!address.bufferDeviceAddress || !sync.synchronization2)
        die("bufferDeviceAddress and synchronization2 are required");
    address.bufferDeviceAddressCaptureReplay = VK_FALSE;
    address.bufferDeviceAddressMultiDevice = VK_FALSE;

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) die("Could not allocate queue family list");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count && (!(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)
                             || !families[family].timestampValidBits)) ++family;
    if (family == count) die("No compute queue with GPU timestamps");
    uint32_t timestamp_bits = families[family].timestampValidBits;
    free(families);
    uint64_t timestamp_mask = timestamp_bits == 64 ? UINT64_MAX : (UINT64_C(1) << timestamp_bits) - 1;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &address,
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
    LOAD(vkGetBufferDeviceAddress);
    LOAD(vkMapMemory);
    LOAD(vkUnmapMemory);
    LOAD(vkCreateCommandPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkResetCommandPool);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCreateShaderModule);
    LOAD(vkDestroyShaderModule);
    LOAD(vkCreatePipelineLayout);
    LOAD(vkDestroyPipelineLayout);
    LOAD(vkCreateComputePipelines);
    LOAD(vkDestroyPipeline);
    LOAD(vkCmdBindPipeline);
    LOAD(vkCmdPushConstants);
    LOAD(vkCmdDispatch);
    LOAD(vkCreateQueryPool);
    LOAD(vkDestroyQueryPool);
    LOAD(vkCmdResetQueryPool);
    LOAD(vkCmdWriteTimestamp2);
    LOAD(vkGetQueryPoolResults);
    LOAD(vkCmdPipelineBarrier2);
    LOAD(vkCreateFence);
    LOAD(vkDestroyFence);
    LOAD(vkResetFences);
    LOAD(vkQueueSubmit2);
    LOAD(vkWaitForFences);

    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkBuffer buffers[2];
    VkDeviceMemory memories[2];
    VkDeviceAddress addresses[2];
    uint32_t memory_flags[2];
    void *mapped[2];
    for (size_t i = 0; i < 2; ++i) {
        VkBufferCreateInfo buffer = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ELEMENTS * sizeof(uint32_t),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        };
        CHECK(vkCreateBuffer(device, &buffer, NULL, &buffers[i]));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        uint32_t type = memory_type(&memory_properties, requirements.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        memory_flags[i] = memory_properties.memoryTypes[type].propertyFlags;
        VkMemoryAllocateFlagsInfo flags = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
            .allocationSize = requirements.size, .memoryTypeIndex = type,
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[i]));
        CHECK(vkBindBufferMemory(device, buffers[i], memories[i], 0));
        CHECK(vkMapMemory(device, memories[i], 0, VK_WHOLE_SIZE, 0, &mapped[i]));
        VkBufferDeviceAddressInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffers[i],
        };
        addresses[i] = vkGetBufferDeviceAddress(device, &info);
        if (!addresses[i]) die("Buffer has no device address");
    }
    if (addresses[0] == addresses[1]) die("Buffers unexpectedly alias");
    uint32_t *input = mapped[0];
    const uint32_t *output = mapped[1];
    for (uint32_t i = 0; i < ELEMENTS; ++i) {
        input[i] = i * UINT32_C(0x9e3779b9);
    }

    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(shader_code), .pCode = shader_code,
    };
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader));
    VkPushConstantRange constants = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(Params),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
        .pPushConstantRanges = &constants,
    };
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
    };
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
    };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo buffers_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(device, &buffers_info, &cb));
    VkQueryPoolCreateInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
    };
    VkQueryPool query;
    CHECK(vkCreateQueryPool(device, &query_info, NULL, &query));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));

    VkMemoryBarrier2 host_read = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo read_dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &host_read,
    };
    double gpu_ns[TRIALS], completion_ns[TRIALS];
    for (unsigned trial = 0; trial <= TRIALS; ++trial) {
        CHECK(vkResetFences(device, 1, &fence));
        CHECK(vkResetCommandPool(device, pool, 0));
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        CHECK(vkBeginCommandBuffer(cb, &begin));
        vkCmdResetQueryPool(cb, query, 0, 2);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        Params params = {addresses[0], addresses[1], UINT32_C(0xc0ffee00) + trial};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, query, 0);
        vkCmdDispatch(cb, ELEMENTS / WORKGROUP_SIZE, 1, 1);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, query, 1);
        vkCmdPipelineBarrier2(cb, &read_dependency);
        CHECK(vkEndCommandBuffer(cb));
        VkCommandBufferSubmitInfo command = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb,
        };
        VkSubmitInfo2 submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &command,
        };
        double start = clock_ns();
        CHECK(vkQueueSubmit2(queue, 1, &submit, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        double elapsed = clock_ns() - start;
        uint64_t ticks[2];
        CHECK(vkGetQueryPoolResults(device, query, 0, 2, sizeof(ticks), ticks,
                                    sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT));
        for (uint32_t i = 0; i < ELEMENTS; ++i) {
            uint32_t expected = reference(input[i], params.seed);
            if (output[i] != expected) {
                fprintf(stderr, "Compute readback at %" PRIu32 " got %08" PRIx32 " expected %08" PRIx32 "\n",
                        i, output[i], expected);
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
           "\"memory_flags\":[%u,%u],\"elements\":%u,\"steps\":%u,\"trials\":[",
           properties.vendorID, properties.deviceID, properties.apiVersion, memory_flags[0], memory_flags[1],
           ELEMENTS, STEPS);
    for (size_t i = 0; i < TRIALS; ++i)
        printf("%s{\"gpu_ns\":%.0f,\"submit_wait_ns\":%.0f}", i ? "," : "", gpu_ns[i], completion_ns[i]);
    puts("]}");
    vkDestroyFence(device, fence, NULL);
    vkDestroyQueryPool(device, query, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    for (size_t i = 0; i < 2; ++i) {
        vkUnmapMemory(device, memories[i]);
        vkDestroyBuffer(device, buffers[i], NULL);
        vkFreeMemory(device, memories[i], NULL);
    }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
