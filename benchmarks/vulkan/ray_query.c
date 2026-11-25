/* 2 ジオメトリの BLAS と 2 インスタンスの TLAS を作り、Ray Query の結果を検算する。 */
#include "vulkan_util.h"
#include <string.h>

static const uint32_t shader_code[] =
#include "ray_query.spv.h"
;

static const char *const required_extensions[] = {
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
};

enum { RAY_MISS = 0, RAY_TRIANGLE = 1, INSTANCE_INDEX = 7, TRANSLATED_INDEX = 13,
       GEOMETRY_COUNT = 2, INSTANCE_COUNT = 2, RAY_COUNT = 5, EXIT_SKIP = 77 };
typedef struct {
    uint32_t kind;
    float distance;
    uint32_t instance_index, primitive_index, geometry_index;
} RayResult;
_Static_assert(sizeof(RayResult) == 20, "RayResult std430 array stride");
_Static_assert(sizeof(VkAccelerationStructureInstanceKHR) == 64, "Acceleration structure instance ABI");

#define DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkGetBufferDeviceAddress) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkGetAccelerationStructureBuildSizesKHR) X(vkCreateAccelerationStructureKHR) \
    X(vkDestroyAccelerationStructureKHR) X(vkGetAccelerationStructureDeviceAddressKHR) \
    X(vkCmdBuildAccelerationStructuresKHR) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdDispatch) X(vkCreateFence) X(vkDestroyFence) \
    X(vkQueueSubmit) X(vkWaitForFences)

#define DECLARE(name) static PFN_##name name;
DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
static PFN_vkDestroyInstance vkDestroyInstance;
static PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;

static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory_properties;

typedef struct {
    VkBuffer handle;
    VkDeviceMemory memory;
    void *mapped;
} Buffer;

static Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    Buffer buffer = {0};
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
    };
    CHECK(vkCreateBuffer(device, &info, NULL, &buffer.handle));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
    VkMemoryAllocateFlagsInfo address = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &address : NULL,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(&memory_properties, requirements.memoryTypeBits, properties),
    };
    CHECK(vkAllocateMemory(device, &allocation, NULL, &buffer.memory));
    CHECK(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0));
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        CHECK(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
    return buffer;
}

static void destroy_buffer(Buffer buffer) {
    if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
    vkDestroyBuffer(device, buffer.handle, NULL);
    vkFreeMemory(device, buffer.memory, NULL);
}

static VkDeviceAddress buffer_address(Buffer buffer) {
    VkBufferDeviceAddressInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.handle,
    };
    VkDeviceAddress address = vkGetBufferDeviceAddress(device, &info);
    if (!address) die("Buffer has no device address");
    return address;
}

static VkDeviceSize alignment_offset(VkDeviceAddress address, VkDeviceSize alignment) {
    return (alignment - address % alignment) % alignment;
}

static const char *missing_extension(VkPhysicalDevice physical) {
    uint32_t count = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    if (!count) return required_extensions[0];
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) die("Could not allocate extension list");
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    const char *missing = NULL;
    for (size_t i = 0; i < sizeof(required_extensions) / sizeof(required_extensions[0]); ++i) {
        uint32_t j = 0;
        while (j < count && strcmp(extensions[j].extensionName, required_extensions[i]) != 0) ++j;
        if (j == count) {
            missing = required_extensions[i];
            break;
        }
    }
    free(extensions);
    return missing;
}

static int skip(VkInstance instance, const char *reason) {
    if (instance) vkDestroyInstance(instance, NULL);
    printf("{\"status\":\"skipped\",\"validated\":false,\"reason\":\"%s\"}\n", reason);
    return EXIT_SKIP;
}

int main(int argc, char **argv) {
    int probe_cleanup = 0;
#ifdef __ANDROID__
    if (argc == 2 && strcmp(argv[1], "--probe-device-cleanup") == 0) probe_cleanup = 1;
    if (argc == 2 && strcmp(argv[1], "--probe-process-cleanup") == 0) probe_cleanup = 2;
#endif
    char *end = NULL;
    const int select_device = argc == 2 && !probe_cleanup;
    unsigned long index = select_device ? strtoul(argv[1], &end, 10) : 0;
    if (argc > 2 || (select_device && (end == argv[1] || *end))) die("usage: vk-ray_query [device-index]");

    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkEnumerateInstanceVersion enumerate_version =
        (PFN_vkEnumerateInstanceVersion)lookup(instance, "vkEnumerateInstanceVersion");
    uint32_t api_version = VK_API_VERSION_1_0;
    if (enumerate_version) CHECK(enumerate_version(&api_version));
    if (api_version < VK_API_VERSION_1_2) return skip(instance, "Vulkan 1.2 loader required");
    LOAD(vkCreateInstance);
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Velox Ray Query validation", .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application,
    };
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    vkDestroyInstance = (PFN_vkDestroyInstance)lookup(instance, "vkDestroyInstance");
    vkEnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)
        lookup(instance, "vkEnumerateDeviceExtensionProperties");
    if (!vkDestroyInstance || !vkEnumerateDeviceExtensionProperties) die("Missing instance entry point");
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceProperties2);
    LOAD(vkGetPhysicalDeviceFormatProperties);
    LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);
    LOAD(vkGetDeviceProcAddr);

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) return skip(instance, "No Vulkan physical device");
    if (index >= count) die("Physical device index does not exist");
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) die("Could not allocate physical device list");
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    if (index >= count) die("Physical device list changed");
    VkPhysicalDevice physical = devices[index];
    free(devices);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2 || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        return skip(instance, "Vulkan 1.2 hardware device required");
    const char *missing = missing_extension(physical);
    if (missing) return skip(instance, missing);

    VkPhysicalDeviceRayQueryFeaturesKHR ray_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &ray_query,
    };
    VkPhysicalDeviceBufferDeviceAddressFeatures address = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .pNext = &acceleration,
    };
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &address};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!address.bufferDeviceAddress || !acceleration.accelerationStructure || !ray_query.rayQuery)
        return skip(instance, "bufferDeviceAddress, accelerationStructure and rayQuery required");
    address.bufferDeviceAddressCaptureReplay = VK_FALSE;
    address.bufferDeviceAddressMultiDevice = VK_FALSE;
    acceleration.accelerationStructureCaptureReplay = VK_FALSE;
    acceleration.accelerationStructureIndirectBuild = VK_FALSE;
    acceleration.accelerationStructureHostCommands = VK_FALSE;
    acceleration.descriptorBindingAccelerationStructureUpdateAfterBind = VK_FALSE;

    VkFormat vertex_format = VK_FORMAT_R32G32B32_SFLOAT;
    VkFormatProperties format_properties;
    vkGetPhysicalDeviceFormatProperties(physical, vertex_format, &format_properties);
    if (!(format_properties.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))
        return skip(instance, "R32G32B32_SFLOAT acceleration structure vertices required");
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
    };
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &acceleration_properties,
    };
    vkGetPhysicalDeviceProperties2(physical, &properties2);

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (!count) return skip(instance, "No Vulkan queue families");
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) die("Could not allocate queue family list");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    free(families);
    if (family == count) return skip(instance, "Compute queue required");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &address,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = (uint32_t)(sizeof(required_extensions) / sizeof(required_extensions[0])),
        .ppEnabledExtensionNames = required_extensions,
    };
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
#define LOAD_DEVICE(name) \
    name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
    if (!name) die("Missing device entry point " #name);
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    const float triangles[GEOMETRY_COUNT][3][3] = {
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}},
    };
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkBufferUsageFlags build_input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    Buffer vertices = create_buffer(sizeof(triangles) + 3, build_input, coherent);
    Buffer instances = create_buffer(INSTANCE_COUNT * sizeof(VkAccelerationStructureInstanceKHR) + 15,
                                     build_input, coherent);
    VkDeviceAddress vertex_base = buffer_address(vertices);
    VkDeviceAddress instance_base = buffer_address(instances);
    VkDeviceSize vertex_offset = alignment_offset(vertex_base, 4);
    VkDeviceSize instance_offset = alignment_offset(instance_base, 16);
    memcpy((char *)vertices.mapped + vertex_offset, triangles, sizeof(triangles));

    VkAccelerationStructureGeometryKHR bottom_geometry[GEOMETRY_COUNT];
    for (size_t i = 0; i < GEOMETRY_COUNT; ++i) {
        bottom_geometry[i] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
            .geometry = {.triangles = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat = vertex_format,
                .vertexData = {.deviceAddress = vertex_base + vertex_offset + i * sizeof(triangles[0])},
                .vertexStride = sizeof(triangles[0][0]), .maxVertex = 2, .indexType = VK_INDEX_TYPE_NONE_KHR,
            }},
        };
    }
    VkAccelerationStructureGeometryKHR top_geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = {.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .data = {.deviceAddress = instance_base + instance_offset},
        }},
    };
    const VkAccelerationStructureTypeKHR types[] = {
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    };
    VkAccelerationStructureBuildGeometryInfoKHR builds[2];
    Buffer storage[2];
    VkDeviceSize scratch_size = 0;
    const uint32_t bottom_primitives[GEOMETRY_COUNT] = {1, 1};
    const uint32_t top_primitives = INSTANCE_COUNT;
    for (size_t i = 0; i < 2; ++i) {
        VkAccelerationStructureBuildGeometryInfoKHR build = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = types[i], .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = i == 0 ? GEOMETRY_COUNT : 1,
            .pGeometries = i == 0 ? bottom_geometry : &top_geometry,
        };
        VkAccelerationStructureBuildSizesInfoKHR size = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
        };
        vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &build, i == 0 ? bottom_primitives : &top_primitives, &size);
        if (!size.accelerationStructureSize) die("Acceleration structure storage size is zero");
        if (size.buildScratchSize > scratch_size) scratch_size = size.buildScratchSize;
        storage[i] = create_buffer(size.accelerationStructureSize,
                                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkAccelerationStructureCreateInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = storage[i].handle, .size = size.accelerationStructureSize, .type = types[i],
        };
        CHECK(vkCreateAccelerationStructureKHR(device, &info, NULL, &build.dstAccelerationStructure));
        builds[i] = build;
    }
    VkAccelerationStructureDeviceAddressInfoKHR blas_address_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = builds[0].dstAccelerationStructure,
    };
    VkDeviceAddress blas_address = vkGetAccelerationStructureDeviceAddressKHR(device, &blas_address_info);
    if (!blas_address) die("BLAS has no device address");
    VkAccelerationStructureInstanceKHR geometry_instances[INSTANCE_COUNT];
    geometry_instances[0] = (VkAccelerationStructureInstanceKHR){
        .transform = {.matrix = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}}},
        .instanceCustomIndex = INSTANCE_INDEX, .mask = 0x1,
        .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
        .accelerationStructureReference = blas_address,
    };
    geometry_instances[1] = geometry_instances[0];
    geometry_instances[1].transform.matrix[0][3] = 3.0f;
    geometry_instances[1].transform.matrix[2][3] = -1.0f;
    geometry_instances[1].instanceCustomIndex = TRANSLATED_INDEX;
    geometry_instances[1].mask = 0x2;
    memcpy((char *)instances.mapped + instance_offset, geometry_instances, sizeof(geometry_instances));

    VkDeviceSize scratch_alignment = acceleration_properties.minAccelerationStructureScratchOffsetAlignment;
    if (!scratch_alignment || scratch_size > UINT64_MAX - scratch_alignment) die("Invalid scratch size or alignment");
    Buffer scratch = create_buffer(scratch_size + scratch_alignment,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceAddress scratch_base = buffer_address(scratch);
    VkDeviceAddress scratch_address = scratch_base + alignment_offset(scratch_base, scratch_alignment);
    builds[0].scratchData.deviceAddress = scratch_address;
    builds[1].scratchData.deviceAddress = scratch_address;

    Buffer output = create_buffer(RAY_COUNT * sizeof(RayResult), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, coherent);
    memset(output.mapped, 0xff, RAY_COUNT * sizeof(RayResult));
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
        .poolSizeCount = 2, .pPoolSizes = pool_sizes,
    };
    VkDescriptorPool descriptor_pool;
    CHECK(vkCreateDescriptorPool(device, &descriptor_pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo set_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1, .pSetLayouts = &set_layout,
    };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &set_alloc, &set));
    VkWriteDescriptorSetAccelerationStructureKHR scene_info = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1, .pAccelerationStructures = &builds[1].dstAccelerationStructure,
    };
    VkDescriptorBufferInfo output_binding = {.buffer = output.handle, .range = RAY_COUNT * sizeof(RayResult)};
    VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &scene_info,
         .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &output_binding},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(shader_code), .pCode = shader_code,
    };
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader));
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout,
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
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(command, &begin));
    const VkAccelerationStructureBuildRangeInfoKHR bottom_ranges[] = {{.primitiveCount = 1}, {.primitiveCount = 1}};
    const VkAccelerationStructureBuildRangeInfoKHR top_range = {.primitiveCount = INSTANCE_COUNT};
    const VkAccelerationStructureBuildRangeInfoKHR *bottom_range_ptr = bottom_ranges, *top_range_ptr = &top_range;
    vkCmdBuildAccelerationStructuresKHR(command, 1, &builds[0], &bottom_range_ptr);
    VkMemoryBarrier build_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &build_barrier, 0, NULL, 0, NULL);
    vkCmdBuildAccelerationStructuresKHR(command, 1, &builds[1], &top_range_ptr);
    build_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &build_barrier, 0, NULL, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(command, RAY_COUNT, 1, 1);
    VkMemoryBarrier readback = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &readback, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));

    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    double start = clock_ns();
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
    double elapsed = clock_ns() - start;

    const RayResult *rays = output.mapped;
    const RayResult expected[RAY_COUNT] = {
        {RAY_TRIANGLE, 1.0f, INSTANCE_INDEX, 0, 0},
        {RAY_TRIANGLE, 2.0f, TRANSLATED_INDEX, 0, 1},
        {RAY_MISS, 0.0f, 0, 0, 0},
        {RAY_MISS, 0.0f, 0, 0, 0},
        {RAY_TRIANGLE, 1.0f, INSTANCE_INDEX, 0, 1},
    };
    int validated = 1;
    for (size_t i = 0; i < RAY_COUNT; ++i) {
        float error = rays[i].distance - expected[i].distance;
        if (rays[i].kind != expected[i].kind || !(error >= -0.00001f && error <= 0.00001f) ||
            rays[i].instance_index != expected[i].instance_index || rays[i].primitive_index != expected[i].primitive_index ||
            rays[i].geometry_index != expected[i].geometry_index) {
            validated = 0;
        }
    }
    if (validated) {
        printf("{\"status\":\"pass\",\"validated\":true,\"vendor_id\":%u,\"device_id\":%u,\"api_version\":%u,"
               "\"hit_distance\":%.9g,\"instance_custom_index\":%u,\"primitive_index\":%u,"
               "\"geometry_count\":%u,\"instance_count\":%u,\"rays_validated\":%u,\"submit_wait_ns\":%.0f}\n",
               properties.vendorID, properties.deviceID, properties.apiVersion,
               rays[0].distance, rays[0].instance_index, rays[0].primitive_index,
               (unsigned)GEOMETRY_COUNT, (unsigned)INSTANCE_COUNT, (unsigned)RAY_COUNT, elapsed);
    } else {
        for (size_t i = 0; i < RAY_COUNT; ++i) {
            fprintf(stderr, "Ray %zu: kind %u, distance %.9g, instance %u, primitive %u, geometry %u\n",
                    i, rays[i].kind, rays[i].distance, rays[i].instance_index, rays[i].primitive_index,
                    rays[i].geometry_index);
        }
    }

    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    // These Android-only probes exercise gfxstream's recovery of leaked AS and backing buffers.
    if (!probe_cleanup) {
        vkDestroyAccelerationStructureKHR(device, builds[1].dstAccelerationStructure, NULL);
        vkDestroyAccelerationStructureKHR(device, builds[0].dstAccelerationStructure, NULL);
        destroy_buffer(storage[1]);
        destroy_buffer(storage[0]);
    }
    destroy_buffer(scratch);
    destroy_buffer(output);
    destroy_buffer(instances);
    destroy_buffer(vertices);
    if (probe_cleanup == 2) return validated ? 0 : 2;
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return validated ? 0 : 2;
}
