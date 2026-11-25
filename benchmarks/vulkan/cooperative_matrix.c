/* VK_KHR_cooperative_matrix の列挙と FP16/INT8 行列積のネイティブ・ゲスト共通試験。 */
#include "vulkan_util.h"
#include <math.h>
#include <string.h>

#include "cooperative_matrix_fp16.spv.h"
#include "cooperative_matrix_fp16_accum.spv.h"
#include "cooperative_matrix_int8.spv.h"

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *data;
} MatrixBuffer;

static uint32_t positive_arg(const char *arg, unsigned long maximum) {
    char *end;
    unsigned long value = strtoul(arg, &end, 10);
    if (end == arg || *end || value == 0 || value > maximum) die("Invalid positive integer option");
    return (uint32_t)value;
}

static int input_value(uint32_t row, uint32_t column, uint32_t seed) {
    return (int)((row * (column + seed) + 3 * column + seed) % 5) - 2;
}

static void fill_input(void *storage, uint32_t rows, uint32_t columns, uint32_t seed, int integer) {
    static const uint16_t fp16[] = {0xc000, 0xbc00, 0, 0x3c00, 0x4000};
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t col = 0; col < columns; ++col) {
            const uint32_t index = row * columns + col;
            const int input = input_value(row, col, seed);
            if (integer) ((int8_t *)storage)[index] = (int8_t)input;
            else ((uint16_t *)storage)[index] = fp16[input + 2];
        }
    }
}

static double read_half(uint16_t bits) {
    int exponent = (bits >> 10) & 31;
    int fraction = bits & 1023;
    if (exponent == 31) return NAN;
    double magnitude = exponent ? ldexp(1024 + fraction, exponent - 25)
                               : ldexp(fraction, -24);
    return (bits & 0x8000) ? -magnitude : magnitude;
}

static int device_local;  /* --device-local */

static int verify_matrix(const void *storage, uint32_t rows, uint32_t columns, uint32_t depth,
                         uint32_t batch, VkComponentTypeKHR result_type) {
    // 各部分和の絶対値は最大 4*K。FP16 の試験では 2,048 以下に制限する。
    // INT32 と FP32 の試験は K <= 2,048 なのでオーバーフローや丸めはない。
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t col = 0; col < columns; ++col) {
            int expected = 0;
            for (uint32_t k = 0; k < depth; ++k)
                expected += input_value(row, k, 2 + batch) * input_value(k, col, 5 + 3 * batch);
            const uint32_t index = row * columns + col;
            double actual;
            if (result_type == VK_COMPONENT_TYPE_SINT32_KHR) actual = ((const int32_t *)storage)[index];
            else if (result_type == VK_COMPONENT_TYPE_FLOAT16_KHR) actual = read_half(((const uint16_t *)storage)[index]);
            else actual = ((const float *)storage)[index];
            if (actual != expected) {
                fprintf(stderr, "matrix[%u,%u,%u]: expected %d, got %.9g\n", batch, row, col, expected, actual);
                return 0;
            }
        }
    }
    return 1;
}

static int run_matrix(PFN_vkGetInstanceProcAddr lookup, VkInstance instance, VkDevice device,
                      const VkPhysicalDeviceMemoryProperties *memory, uint32_t family,
                      uint32_t timestamp_bits, double timestamp_period, uint32_t subgroup,
                      VkCooperativeMatrixPropertiesKHR tile, uint32_t tiles, uint32_t batches,
                      int split_dispatch, uint32_t repetitions) {
    LOAD(vkGetDeviceQueue);
    LOAD(vkCreateBuffer);
    LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkAllocateMemory);
    LOAD(vkBindBufferMemory);
    LOAD(vkMapMemory);
    LOAD(vkCreateDescriptorSetLayout);
    LOAD(vkCreatePipelineLayout);
    LOAD(vkCreateDescriptorPool);
    LOAD(vkAllocateDescriptorSets);
    LOAD(vkUpdateDescriptorSets);
    LOAD(vkCreateShaderModule);
    LOAD(vkCreateComputePipelines);
    LOAD(vkCreateCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCmdBindPipeline);
    LOAD(vkCmdBindDescriptorSets);
    LOAD(vkCmdPushConstants);
    LOAD(vkCmdDispatch);
    LOAD(vkCmdCopyBuffer);
    LOAD(vkResetCommandBuffer);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkCreateQueryPool);
    LOAD(vkCmdResetQueryPool);
    LOAD(vkCmdWriteTimestamp);
    LOAD(vkGetQueryPoolResults);
    LOAD(vkCreateFence);
    LOAD(vkResetFences);
    LOAD(vkQueueSubmit);
    LOAD(vkWaitForFences);
    LOAD(vkDestroyFence);
    LOAD(vkDestroyQueryPool);
    LOAD(vkDestroyCommandPool);
    LOAD(vkDestroyPipeline);
    LOAD(vkDestroyShaderModule);
    LOAD(vkDestroyDescriptorPool);
    LOAD(vkDestroyPipelineLayout);
    LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkUnmapMemory);
    LOAD(vkDestroyBuffer);
    LOAD(vkFreeMemory);

    const int integer = tile.AType == VK_COMPONENT_TYPE_SINT8_KHR;
    const int half = tile.ResultType == VK_COMPONENT_TYPE_FLOAT16_KHR;
    const char *format = integer ? "int8-int32" : half ? "fp16-fp16" : "fp16-fp32";
    const uint32_t rows = tile.MSize * tiles, columns = tile.NSize * tiles, depth = tile.KSize * tiles;
    const VkDeviceSize input_bytes = integer ? 1 : 2;
    const size_t strides[] = {rows * depth * input_bytes, depth * columns * input_bytes,
                              rows * columns * (half ? sizeof(uint16_t) : sizeof(uint32_t))};
    MatrixBuffer matrices[3] = {0};
    /* --device-local: the kernel reads and writes device-local copies; the host-visible buffers
     * become staging buffers that are copied once before and once after the timed submits. */
    MatrixBuffer local[3] = {0};
    VkDescriptorSetLayoutBinding bindings[3] = {0};
    VkDescriptorBufferInfo descriptors[3];
    for (uint32_t i = 0; i < 3; ++i) {
        const VkDeviceSize size = strides[i] * batches;
        VkBufferCreateInfo buffer = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        CHECK(vkCreateBuffer(device, &buffer, NULL, &matrices[i].buffer));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, matrices[i].buffer, &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type(memory, requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &matrices[i].memory));
        CHECK(vkBindBufferMemory(device, matrices[i].buffer, matrices[i].memory, 0));
        CHECK(vkMapMemory(device, matrices[i].memory, 0, VK_WHOLE_SIZE, 0, &matrices[i].data));
        bindings[i] = (VkDescriptorSetLayoutBinding){
            i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL,
        };
        descriptors[i] = (VkDescriptorBufferInfo){matrices[i].buffer, 0, size};
        if (device_local) {
            CHECK(vkCreateBuffer(device, &buffer, NULL, &local[i].buffer));
            VkMemoryRequirements local_requirements;
            vkGetBufferMemoryRequirements(device, local[i].buffer, &local_requirements);
            VkMemoryAllocateInfo local_allocation = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = local_requirements.size,
                .memoryTypeIndex = memory_type(memory, local_requirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            };
            CHECK(vkAllocateMemory(device, &local_allocation, NULL, &local[i].memory));
            CHECK(vkBindBufferMemory(device, local[i].buffer, local[i].memory, 0));
            descriptors[i] = (VkDescriptorBufferInfo){local[i].buffer, 0, size};
        }
    }
    const double input_started = clock_ns();
    for (uint32_t batch = 0; batch < batches; ++batch) {
        fill_input((char *)matrices[0].data + strides[0] * batch, rows, depth, 2 + batch, integer);
        fill_input((char *)matrices[1].data + strides[1] * batch, depth, columns, 5 + 3 * batch, integer);
    }
    const double input_write_ns = clock_ns() - input_started;
    memset(matrices[2].data, 0xff, strides[2] * batches);

    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    uint32_t shape[] = {rows, columns, depth, 0};
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shape)};
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
        .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
        .poolSizeCount = 1, .pPoolSizes = &pool_size,
    };
    VkDescriptorPool descriptor_pool;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo set_allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1, .pSetLayouts = &set_layout,
    };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &set_allocate, &set));
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &descriptors[i],
        };
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = integer ? sizeof(cooperative_matrix_int8) :
                   half ? sizeof(cooperative_matrix_fp16_accum) : sizeof(cooperative_matrix_fp16),
        .pCode = integer ? cooperative_matrix_int8 :
                 half ? cooperative_matrix_fp16_accum : cooperative_matrix_fp16,
    };
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader));
    const uint32_t values[] = {tile.MSize, tile.NSize, tile.KSize, subgroup};
    const VkSpecializationMapEntry entries[] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}, {3, 12, 4}};
    const VkSpecializationInfo specialization = {4, entries, sizeof(values), values};
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main",
                  .pSpecializationInfo = &specialization},
    };
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family,
    };
    VkCommandPool command_pool;
    CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &command_allocate, &command));
    VkQueryPool queries = VK_NULL_HANDLE;
    if (timestamp_bits) {
        VkQueryPoolCreateInfo query_info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 2,
        };
        CHECK(vkCreateQueryPool(device, &query_info, NULL, &queries));
    }
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    const double record_started = clock_ns();
    CHECK(vkBeginCommandBuffer(command, &begin));
    if (queries) {
        vkCmdResetQueryPool(command, queries, 0, 2);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    const uint32_t dispatches = split_dispatch ? batches : 1;
    for (uint32_t i = 0; i < dispatches; ++i) {
        shape[3] = i;
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shape), shape);
        vkCmdDispatch(command, tiles, tiles, split_dispatch ? 1 : batches);
    }
    if (queries) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
    VkMemoryBarrier readback = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &readback, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    printf("{\"type\":\"setup\",\"format\":\"%s\",\"m\":%u,\"n\":%u,\"k\":%u,"
           "\"batches\":%u,\"dispatches\":%u,\"input_write_ns\":%.0f,\"record_ns\":%.0f}\n",
           format, rows, columns, depth, batches, dispatches, input_write_ns, clock_ns() - record_started);
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkCommandBuffer transfer = VK_NULL_HANDLE;
    double upload_ns = 0.0;
    if (device_local) {
        CHECK(vkAllocateCommandBuffers(device, &command_allocate, &transfer));
        CHECK(vkBeginCommandBuffer(transfer, &begin));
        for (uint32_t i = 0; i < 3; ++i) {
            VkBufferCopy region = {.size = strides[i] * batches};
            vkCmdCopyBuffer(transfer, matrices[i].buffer, local[i].buffer, 1, &region);
        }
        VkMemoryBarrier uploaded = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        vkCmdPipelineBarrier(transfer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &uploaded, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(transfer));
        VkSubmitInfo upload = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                               .pCommandBuffers = &transfer};
        const double upload_started = clock_ns();
        CHECK(vkQueueSubmit(queue, 1, &upload, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        upload_ns = clock_ns() - upload_started;
        printf("{\"type\":\"upload\",\"format\":\"%s\",\"m\":%u,\"n\":%u,\"k\":%u,\"bytes\":%llu,"
               "\"upload_ns\":%.0f}\n", format, rows, columns, depth,
               (unsigned long long)((strides[0] + strides[1] + strides[2]) * batches), upload_ns);
    }
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    for (uint32_t rep = 0; rep <= repetitions; ++rep) {
        CHECK(vkResetFences(device, 1, &fence));
        double started = clock_ns();
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        const double submit_ns = clock_ns() - started;
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        double host_ns = clock_ns() - started;
        if (!rep) continue;
        printf("{\"type\":\"sample\",\"format\":\"%s\",\"m\":%u,\"n\":%u,\"k\":%u,"
               "\"tile\":[%u,%u,%u],\"batches\":%u,\"dispatches\":%u,\"rep\":%u,"
               "\"submit_ns\":%.0f,\"submit_wait_ns\":%.0f",
               format, rows, columns, depth, tile.MSize, tile.NSize, tile.KSize,
               batches, dispatches, rep, submit_ns, host_ns);
        if (queries) {
            uint64_t ticks[2];
            CHECK(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks, sizeof(ticks[0]),
                                        VK_QUERY_RESULT_64_BIT));
            const uint64_t mask = timestamp_bits == 64 ? UINT64_MAX
                                : (UINT64_C(1) << timestamp_bits) - 1;
            const double gpu_ns = (double)((ticks[1] - ticks[0]) & mask) * timestamp_period;
            printf(",\"gpu_ns\":%.0f,\"mean_gpu_ns_per_dispatch\":%.3f", gpu_ns, gpu_ns / dispatches);
            if (gpu_ns > 0)
                printf(",\"%s\":%.6f", integer ? "tops" : "tflops",
                       2.0 * batches * rows * columns * depth / gpu_ns / 1000.0);
        }
        puts("}");
    }
    if (device_local) {
        CHECK(vkResetCommandBuffer(transfer, 0));
        CHECK(vkBeginCommandBuffer(transfer, &begin));
        VkBufferCopy region = {.size = strides[2] * batches};
        vkCmdCopyBuffer(transfer, local[2].buffer, matrices[2].buffer, 1, &region);
        VkMemoryBarrier host_read = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(transfer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &host_read, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(transfer));
        VkSubmitInfo download = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                 .pCommandBuffers = &transfer};
        CHECK(vkResetFences(device, 1, &fence));
        CHECK(vkQueueSubmit(queue, 1, &download, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
    }
    int valid = 1;
    for (uint32_t batch = 0; batch < batches; ++batch) {
        valid &= verify_matrix((char *)matrices[2].data + strides[2] * batch,
                               rows, columns, depth, batch, tile.ResultType);
    }
    printf("{\"type\":\"check\",\"format\":\"%s\",\"m\":%u,\"n\":%u,\"k\":%u,"
           "\"batches\":%u,\"dispatches\":%u,\"validated\":%s}\n",
           format, rows, columns, depth, batches, dispatches, valid ? "true" : "false");
    vkDestroyFence(device, fence, NULL);
    if (queries) vkDestroyQueryPool(device, queries, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (uint32_t i = 0; i < 3; ++i) {
        if (local[i].buffer) {
            vkDestroyBuffer(device, local[i].buffer, NULL);
            vkFreeMemory(device, local[i].memory, NULL);
        }
        vkUnmapMemory(device, matrices[i].memory);
        vkDestroyBuffer(device, matrices[i].buffer, NULL);
        vkFreeMemory(device, matrices[i].memory, NULL);
    }
    return valid;
}

int main(int argc, char **argv) {
    uint32_t tiles = 1, reps = 3, batches = 1;
    int list_only = 0, split_dispatch = 0;
    for (int arg = 1; arg < argc; ++arg) {
        if (strcmp(argv[arg], "--list") == 0) list_only = 1;
        else if (strcmp(argv[arg], "--split-dispatch") == 0) split_dispatch = 1;
        else if (strcmp(argv[arg], "--tiles") == 0 && arg + 1 < argc) tiles = positive_arg(argv[++arg], 128);
        else if (strcmp(argv[arg], "--device-local") == 0) device_local = 1;
        else if (strcmp(argv[arg], "--reps") == 0 && arg + 1 < argc) reps = positive_arg(argv[++arg], 30);
        else if (strcmp(argv[arg], "--batches") == 0 && arg + 1 < argc) batches = positive_arg(argv[++arg], 16);
        else die("usage: vk-cooperative_matrix [--list] [--tiles 1..128] [--reps 1..30] [--batches 1..16] "
                 "[--split-dispatch] [--device-local]");
    }

    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Velox Cooperative Matrix", .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                          .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    LOAD(vkDestroyInstance);
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkEnumerateDeviceExtensionProperties);
    LOAD(vkGetPhysicalDeviceProperties2);
    LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkCreateDevice);
    LOAD(vkDestroyDevice);
    const char *extension = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;

    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkResult enumeration = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((enumeration != VK_SUCCESS && enumeration != VK_INCOMPLETE) || !count)
        die("No Vulkan physical device");
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions && count) die("Could not allocate extension list");
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    uint32_t revision = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (strcmp(extensions[i].extensionName, extension) == 0) revision = extensions[i].specVersion;
    free(extensions);
    if (!revision) {
        puts("{\"type\":\"device\",\"status\":\"not_supported\",\"extension\":\"VK_KHR_cooperative_matrix\"}");
        vkDestroyInstance(instance, NULL);
        return 77;
    }

    LOAD(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR);
    CHECK(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(physical, &count, NULL));
    const uint32_t capacity = count;
    VkCooperativeMatrixPropertiesKHR *properties = calloc(capacity, sizeof(*properties));
    if (!properties && capacity) die("Could not allocate matrix properties");
    for (uint32_t i = 0; i < capacity; ++i)
        properties[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
    if (capacity) {
        // Also exercise the protocol's VK_INCOMPLETE response for a smaller output array.
        if (capacity > 1) {
            uint32_t one = 1;
            if (vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(physical, &one, properties) !=
                    VK_INCOMPLETE || one != 1)
                die("Cooperative matrix property count contract violated");
        }
        CHECK(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(physical, &count, properties));
    }
    VkPhysicalDeviceSubgroupProperties subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceCooperativeMatrixPropertiesKHR matrix_limits = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR, .pNext = &subgroup,
    };
    VkPhysicalDeviceProperties2 limits = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                          .pNext = &matrix_limits};
    vkGetPhysicalDeviceProperties2(physical, &limits);
    VkPhysicalDeviceVulkan12Features core12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features core11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                               .pNext = &core12};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR matrix_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, .pNext = &core11,
    };
    VkPhysicalDeviceFeatures2 supported = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                            .pNext = &matrix_features};
    vkGetPhysicalDeviceFeatures2(physical, &supported);
    printf("{\"type\":\"device\",\"vendor_id\":%u,\"device_id\":%u,\"api_version\":%u,"
           "\"extension_revision\":%u,\"cooperative_matrix\":%u,\"robust_buffer_access\":%u,"
           "\"stages\":%u,\"subgroup_size\":%u,\"vulkan_memory_model\":%u,"
           "\"shader_float16\":%u,\"shader_int8\":%u,\"storage16\":%u,\"storage8\":%u,\"count\":%u}\n",
           limits.properties.vendorID, limits.properties.deviceID, limits.properties.apiVersion, revision,
           matrix_features.cooperativeMatrix, matrix_features.cooperativeMatrixRobustBufferAccess,
           matrix_limits.cooperativeMatrixSupportedStages, subgroup.subgroupSize,
           core12.vulkanMemoryModel, core12.shaderFloat16, core12.shaderInt8,
           core11.storageBuffer16BitAccess, core12.storageBuffer8BitAccess, count);
    for (uint32_t i = 0; i < count; ++i) {
        const VkCooperativeMatrixPropertiesKHR t = properties[i];
        printf("{\"type\":\"tuple\",\"m\":%u,\"n\":%u,\"k\":%u,\"a\":%u,\"b\":%u,"
               "\"c\":%u,\"result\":%u,\"scope\":%u,\"saturating\":%u}\n",
               t.MSize, t.NSize, t.KSize, (unsigned)t.AType, (unsigned)t.BType, (unsigned)t.CType,
               (unsigned)t.ResultType, (unsigned)t.scope, t.saturatingAccumulation);
    }

    if (list_only) {
        free(properties);
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    const int fp16 = core11.storageBuffer16BitAccess && core12.shaderFloat16;
    const int int8 = core12.storageBuffer8BitAccess && core12.shaderInt8;
    if (limits.properties.apiVersion < VK_API_VERSION_1_2 || !matrix_features.cooperativeMatrix ||
        !(matrix_limits.cooperativeMatrixSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
        !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || !subgroup.subgroupSize ||
        !core12.vulkanMemoryModel || (!fp16 && !int8)) {
        puts("{\"type\":\"check\",\"status\":\"not_supported\",\"reason\":\"compute_features\"}");
        free(properties);
        vkDestroyInstance(instance, NULL);
        return 77;
    }
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
    VkQueueFamilyProperties *families = calloc(queue_count, sizeof(*families));
    if (!families) die("No Vulkan queue families");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, families);
    uint32_t family = 0;
    while (family < queue_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == queue_count) die("No compute queue available");
    uint32_t timestamp_bits = families[family].timestampValidBits;
    free(families);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const VkPhysicalDeviceLimits *device_limits = &limits.properties.limits;
    if (tiles > device_limits->maxComputeWorkGroupCount[0] ||
        tiles > device_limits->maxComputeWorkGroupCount[1] ||
        (!split_dispatch && batches > device_limits->maxComputeWorkGroupCount[2]) ||
        subgroup.subgroupSize > device_limits->maxComputeWorkGroupSize[0] ||
        subgroup.subgroupSize > device_limits->maxComputeWorkGroupInvocations) {
        puts("{\"type\":\"check\",\"status\":\"not_supported\",\"reason\":\"dispatch_limits\"}");
        free(properties);
        vkDestroyInstance(instance, NULL);
        return 77;
    }
    core12 = (VkPhysicalDeviceVulkan12Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .vulkanMemoryModel = VK_TRUE,
        .shaderFloat16 = (VkBool32)fp16, .shaderInt8 = (VkBool32)int8,
        .storageBuffer8BitAccess = (VkBool32)int8,
    };
    core11 = (VkPhysicalDeviceVulkan11Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &core12,
        .storageBuffer16BitAccess = (VkBool32)fp16,
    };
    matrix_features.cooperativeMatrixRobustBufferAccess = VK_FALSE;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &matrix_features,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension,
    };
    VkDevice device;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    uint32_t checked = 0;
    int valid = 1;
    for (uint32_t i = 0; i < count; ++i) {
        const VkCooperativeMatrixPropertiesKHR t = properties[i];
        if (t.scope != VK_SCOPE_SUBGROUP_KHR || t.saturatingAccumulation ||
            t.AType != t.BType || t.CType != t.ResultType ||
            !t.MSize || !t.NSize || !t.KSize ||
            t.MSize > 2048 / tiles || t.NSize > 2048 / tiles || t.KSize > 2048 / tiles) continue;
        const int half = t.ResultType == VK_COMPONENT_TYPE_FLOAT16_KHR;
        const int integer = t.AType == VK_COMPONENT_TYPE_SINT8_KHR;
        const uint32_t limit = device_limits->maxStorageBufferRange / (batches * tiles * tiles);
        if (t.MSize * t.KSize * (integer ? 1u : 2u) > limit ||
            t.KSize * t.NSize * (integer ? 1u : 2u) > limit ||
            t.MSize * t.NSize * (half ? 2u : 4u) > limit) continue;
        if ((fp16 && t.AType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
             (t.CType == VK_COMPONENT_TYPE_FLOAT32_KHR || (half && t.KSize * tiles <= 512))) ||
            (int8 && integer && t.CType == VK_COMPONENT_TYPE_SINT32_KHR)) {
            valid &= run_matrix(lookup, instance, device, &memory, family, timestamp_bits,
                                device_limits->timestampPeriod, subgroup.subgroupSize,
                                t, tiles, batches, split_dispatch, reps);
            ++checked;
        }
    }
    vkDestroyDevice(device, NULL);
    free(properties);
    vkDestroyInstance(instance, NULL);
    if (!checked) {
        puts("{\"type\":\"check\",\"status\":\"not_supported\",\"reason\":\"no_benchmark_tuple\"}");
        return 77;
    }
    return valid ? 0 : 2;
}
