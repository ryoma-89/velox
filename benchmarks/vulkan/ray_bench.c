/* Ray Query の性能を Windows ネイティブと Android で同じ内容で測る。
 *
 * シーン: G×G マスの起伏のある格子（2G² 三角形、インデックス付き）の BLAS 1 個を、
 * K×K 個のインスタンスで並べた TLAS。1920×1080 の各画素から主レイ（最も近い交差）と、
 * 命中したときに影レイ（最初の交差で終了）を出す。結果はオフスクリーンのバッファに書く。
 *
 * --scene static : 毎フレーム Ray Query の Dispatch だけ
 * --scene dynamic: 毎フレーム CPU がインスタンスの位置を書き換え、TLAS を更新（UPDATE）してから Dispatch
 *
 * 計測後に決まった手順（dynamic は frame 1000 で BUILD、1001 で UPDATE）で 1 フレーム描き、
 * 出力全体の FNV-1a 64 を出す。ネイティブとゲストで一致すれば同じ交差結果。
 *
 * --dispatches N: 1 フレームの Dispatch 回数（既定 8。1 にすると同期の往復の影響が大きい）
 *
 * --mode query   : Compute Shader の Ray Query（既定）
 * --mode pipeline: Ray Tracing Pipeline（raygen・miss 2 種・closest-hit、SBT、vkCmdTraceRaysKHR）で同じ値を出す
 *
 * 使い方: vk-ray_bench [--mode query|pipeline] [--scene static|dynamic] [--seconds N] [--grid G] [--instances K]
 *                      [--dispatches N] [device-index]
 */
#include "vulkan_util.h"
#include <math.h>
#include <string.h>

static const uint32_t shader_code[] =
#include "ray_bench.spv.h"
;
static const uint32_t raygen_code[] =
#include "ray_pipeline_rgen.spv.h"
;
static const uint32_t miss_code[] =
#include "ray_pipeline_rmiss.spv.h"
;
static const uint32_t shadow_miss_code[] =
#include "ray_shadow_rmiss.spv.h"
;
static const uint32_t closest_hit_code[] =
#include "ray_pipeline_rchit.spv.h"
;

static const char *const required_extensions[] = {
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,  /* --mode pipeline のときだけ */
};
static size_t required_extension_count = 3;
enum { WIDTH = 1920, HEIGHT = 1080, EXIT_SKIP = 77, MAX_FRAMES = 200000 };

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
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdDispatch) X(vkCmdPushConstants) \
    X(vkCmdCopyBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkQueueSubmit) \
    X(vkWaitForFences) X(vkResetCommandBuffer) X(vkCreateQueryPool) X(vkDestroyQueryPool) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

#define DECLARE(name) static PFN_##name name;
DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
static PFN_vkDestroyInstance vkDestroyInstance;
static PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;

static PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR;
static PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR;
static PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR;

static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory_properties;

typedef struct {
    VkBuffer handle;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize size;
} Buffer;

static Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    Buffer buffer = {.size = size};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
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
    VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer.handle};
    VkDeviceAddress address = vkGetBufferDeviceAddress(device, &info);
    if (!address) die("Buffer has no device address");
    return address;
}

static int skip(VkInstance instance, const char *reason) {
    if (instance) vkDestroyInstance(instance, NULL);
    printf("{\"status\":\"skipped\",\"reason\":\"%s\"}\n", reason);
    return EXIT_SKIP;
}

static int has_extensions(VkPhysicalDevice physical) {
    uint32_t count = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) die("Could not allocate extension list");
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    int found = 1;
    for (size_t i = 0; i < required_extension_count; ++i) {
        uint32_t j = 0;
        while (j < count && strcmp(extensions[j].extensionName, required_extensions[i])) ++j;
        if (j == count) found = 0;
    }
    free(extensions);
    return found;
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double percentile(double *values, size_t count, double p) {
    if (!count) return 0.0;
    qsort(values, count, sizeof(*values), compare_double);
    size_t index = (size_t)(p * (double)(count - 1) + 0.5);
    return values[index];
}

/* インスタンス i の位置。dynamic のときは frame ごとに上下へ動かす。 */
static void fill_instances(VkAccelerationStructureInstanceKHR *out, uint32_t k, VkDeviceAddress blas,
                           int dynamic, uint32_t frame) {
    for (uint32_t y = 0; y < k; ++y) {
        for (uint32_t x = 0; x < k; ++x) {
            uint32_t i = y * k + x;
            float z = dynamic ? 0.15f * sinf(0.05f * (float)frame + 0.7f * (float)i) : 0.0f;
            out[i] = (VkAccelerationStructureInstanceKHR){
                .transform = {.matrix = {{1.0f, 0.0f, 0.0f, (float)x}, {0.0f, 1.0f, 0.0f, (float)y},
                                         {0.0f, 0.0f, 1.0f, z}}},
                .instanceCustomIndex = i, .mask = 0xff,
                .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
                .accelerationStructureReference = blas,
            };
        }
    }
}

int main(int argc, char **argv) {
    const char *scene = "static", *mode = "query";
    double seconds = 5.0;
    uint32_t grid = 512, k = 8, dispatches = 8;
    unsigned long index = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--grid") && i + 1 < argc) grid = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--instances") && i + 1 < argc) k = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--dispatches") && i + 1 < argc) dispatches = (uint32_t)strtoul(argv[++i], NULL, 10);
        else {
            char *end = NULL;
            index = strtoul(argv[i], &end, 10);
            if (end == argv[i] || *end)
                die("usage: vk-ray_bench [--mode query|pipeline] [--scene static|dynamic] [--seconds N] [--grid G] "
                    "[--instances K] [--dispatches N] [device-index]");
        }
    }
    const int dynamic = !strcmp(scene, "dynamic");
    if (!dynamic && strcmp(scene, "static")) die("scene must be static or dynamic");
    const int use_pipeline = !strcmp(mode, "pipeline");
    if (!use_pipeline && strcmp(mode, "query")) die("mode must be query or pipeline");
    if (use_pipeline) required_extension_count = 4;
    if (grid < 1 || grid > 2048 || k < 1 || k > 64 || dispatches < 1 || dispatches > 64 || seconds <= 0.0) die("invalid --grid, --instances or --seconds");

    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Velox Ray Query benchmark",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    vkDestroyInstance = (PFN_vkDestroyInstance)lookup(instance, "vkDestroyInstance");
    vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)lookup(instance, "vkEnumerateDeviceExtensionProperties");
    if (!vkDestroyInstance || !vkEnumerateDeviceExtensionProperties) die("Missing instance entry point");
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceProperties2);
    LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);
    LOAD(vkGetDeviceProcAddr);

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (index >= count) return skip(instance, "No such Vulkan physical device");
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) die("Could not allocate physical device list");
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = devices[index];
    free(devices);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2) return skip(instance, "Vulkan 1.2 device required");
    if (!has_extensions(physical))
        return skip(instance, use_pipeline ? "acceleration structure, ray query and ray tracing pipeline extensions required"
                                           : "acceleration structure and ray query extensions required");

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_pipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
                                                     .pNext = use_pipeline ? &rt_pipeline : NULL};
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
    if (use_pipeline && !rt_pipeline.rayTracingPipeline) return skip(instance, "rayTracingPipeline required");
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabled_rt_pipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .rayTracingPipeline = VK_TRUE,
    };
    ray_query.pNext = use_pipeline ? &enabled_rt_pipeline : NULL;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabled_acceleration = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &ray_query,
        .accelerationStructure = VK_TRUE,
    };
    VkPhysicalDeviceBufferDeviceAddressFeatures enabled_address = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .pNext = &enabled_acceleration,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR,
    };
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, .pNext = &rt_properties,
    };
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &acceleration_properties,
    };
    vkGetPhysicalDeviceProperties2(physical, &properties2);

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count ? count : 1, sizeof(*families));
    if (!families) die("Could not allocate queue family list");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == count) return skip(instance, "Compute queue required");
    const int timestamps = families[family].timestampValidBits > 0;
    free(families);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enabled_address,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = (uint32_t)required_extension_count,
        .ppEnabledExtensionNames = required_extensions,
    };
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
#define LOAD_DEVICE(name) \
    name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
    if (!name) die("Missing device entry point " #name);
    DEVICE_FUNCTIONS(LOAD_DEVICE)
    if (use_pipeline) {
        LOAD_DEVICE(vkCreateRayTracingPipelinesKHR)
        LOAD_DEVICE(vkGetRayTracingShaderGroupHandlesKHR)
        LOAD_DEVICE(vkCmdTraceRaysKHR)
    }
#undef LOAD_DEVICE
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    /* 格子のメッシュ（一辺 1.0、起伏 ±0.05） */
    const uint32_t vertex_count = (grid + 1) * (grid + 1), triangle_count = 2 * grid * grid;
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkBufferUsageFlags input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    Buffer vertices = create_buffer((VkDeviceSize)vertex_count * 3 * sizeof(float), input, coherent);
    Buffer indices = create_buffer((VkDeviceSize)triangle_count * 3 * sizeof(uint32_t), input, coherent);
    float *v = vertices.mapped;
    for (uint32_t y = 0; y <= grid; ++y) {
        for (uint32_t x = 0; x <= grid; ++x) {
            float fx = (float)x / (float)grid, fy = (float)y / (float)grid;
            float *p = v + 3 * (y * (grid + 1) + x);
            p[0] = fx;
            p[1] = fy;
            p[2] = 0.05f * sinf(fx * 37.0f) * cosf(fy * 23.0f);
        }
    }
    uint32_t *ix = indices.mapped;
    for (uint32_t y = 0; y < grid; ++y) {
        for (uint32_t x = 0; x < grid; ++x) {
            uint32_t a = y * (grid + 1) + x, b = a + 1, c = a + grid + 1, d = c + 1;
            uint32_t *t = ix + 6 * (y * grid + x);
            t[0] = a; t[1] = b; t[2] = c; t[3] = b; t[4] = d; t[5] = c;
        }
    }
    const uint32_t instance_count = k * k;
    Buffer instances = create_buffer((VkDeviceSize)instance_count * sizeof(VkAccelerationStructureInstanceKHR),
                                     input, coherent);

    VkAccelerationStructureGeometryKHR blas_geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry = {.triangles = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData = {.deviceAddress = buffer_address(vertices)},
            .vertexStride = 3 * sizeof(float), .maxVertex = vertex_count - 1, .indexType = VK_INDEX_TYPE_UINT32,
            .indexData = {.deviceAddress = buffer_address(indices)},
        }},
    };
    VkAccelerationStructureGeometryKHR tlas_geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry = {.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .data = {.deviceAddress = buffer_address(instances)},
        }},
    };
    VkAccelerationStructureBuildGeometryInfoKHR blas_build = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &blas_geometry,
    };
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                 (dynamic ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0),
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &tlas_geometry,
    };
    VkAccelerationStructureBuildSizesInfoKHR blas_size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    VkAccelerationStructureBuildSizesInfoKHR tlas_size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blas_build,
                                            &triangle_count, &blas_size);
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_build,
                                            &instance_count, &tlas_size);
    if (!blas_size.accelerationStructureSize || !tlas_size.accelerationStructureSize) die("Zero acceleration structure size");
    const VkBufferUsageFlags storage_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    Buffer blas_storage = create_buffer(blas_size.accelerationStructureSize, storage_usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Buffer tlas_storage = create_buffer(tlas_size.accelerationStructureSize, storage_usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkAccelerationStructureCreateInfoKHR create = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR, .buffer = blas_storage.handle,
        .size = blas_size.accelerationStructureSize, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    };
    VkAccelerationStructureKHR blas, tlas;
    CHECK(vkCreateAccelerationStructureKHR(device, &create, NULL, &blas));
    create.buffer = tlas_storage.handle;
    create.size = tlas_size.accelerationStructureSize;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    CHECK(vkCreateAccelerationStructureKHR(device, &create, NULL, &tlas));
    VkDeviceSize scratch_needed = blas_size.buildScratchSize;
    if (tlas_size.buildScratchSize > scratch_needed) scratch_needed = tlas_size.buildScratchSize;
    if (tlas_size.updateScratchSize > scratch_needed) scratch_needed = tlas_size.updateScratchSize;
    const VkDeviceSize scratch_alignment = acceleration_properties.minAccelerationStructureScratchOffsetAlignment
        ? acceleration_properties.minAccelerationStructureScratchOffsetAlignment : 256;
    Buffer scratch = create_buffer(scratch_needed + scratch_alignment,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceAddress scratch_address = buffer_address(scratch);
    scratch_address += (scratch_alignment - scratch_address % scratch_alignment) % scratch_alignment;
    blas_build.dstAccelerationStructure = blas;
    blas_build.scratchData.deviceAddress = scratch_address;
    tlas_build.dstAccelerationStructure = tlas;
    tlas_build.scratchData.deviceAddress = scratch_address;
    VkAccelerationStructureDeviceAddressInfoKHR blas_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = blas,
    };
    const VkDeviceAddress blas_address = vkGetAccelerationStructureDeviceAddressKHR(device, &blas_info);
    if (!blas_address) die("BLAS has no device address");

    const VkShaderStageFlags shader_stage = use_pipeline ? VK_SHADER_STAGE_RAYGEN_BIT_KHR : VK_SHADER_STAGE_COMPUTE_BIT;
    const VkPipelineBindPoint bind_point =
        use_pipeline ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR : VK_PIPELINE_BIND_POINT_COMPUTE;
    const VkPipelineStageFlags shader_pipeline_stage =
        use_pipeline ? VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkDeviceSize output_size = (VkDeviceSize)WIDTH * HEIGHT * sizeof(uint32_t);
    Buffer output = create_buffer(output_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Buffer readback = create_buffer(output_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, coherent);

    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, shader_stage, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, shader_stage, NULL},
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = pool_sizes,
    };
    VkDescriptorPool descriptor_pool;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo set_alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1, .pSetLayouts = &set_layout,
    };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &set_alloc, &set));
    VkWriteDescriptorSetAccelerationStructureKHR scene_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1, .pAccelerationStructures = &tlas,
    };
    VkDescriptorBufferInfo output_info = {.buffer = output.handle, .range = output_size};
    VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &scene_write, .dstSet = set, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &output_info},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(shader_code), .pCode = shader_code,
    };
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader));
    VkPushConstantRange push_range = {shader_stage, 0, 16};
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range,
    };
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader, .pName = "main"},
    };
    VkPipeline pipeline;
    VkShaderModule rt_modules[4] = {0};
    Buffer sbt = {0};
    VkStridedDeviceAddressRegionKHR raygen_region = {0}, miss_region = {0}, hit_region = {0}, callable_region = {0};
    if (!use_pipeline) {
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    } else {
        const uint32_t *codes[4] = {raygen_code, miss_code, shadow_miss_code, closest_hit_code};
        const size_t sizes[4] = {sizeof(raygen_code), sizeof(miss_code), sizeof(shadow_miss_code), sizeof(closest_hit_code)};
        const VkShaderStageFlagBits stages_bits[4] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                                      VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
        VkPipelineShaderStageCreateInfo stages[4];
        VkRayTracingShaderGroupCreateInfoKHR groups[4];
        for (uint32_t i = 0; i < 4; ++i) {
            VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizes[i],
                                             .pCode = codes[i]};
            CHECK(vkCreateShaderModule(device, &info, NULL, &rt_modules[i]));
            stages[i] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                          .stage = stages_bits[i], .module = rt_modules[i], .pName = "main"};
            groups[i] = (VkRayTracingShaderGroupCreateInfoKHR){
                .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                .type = i == 3 ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR
                               : VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                .generalShader = i == 3 ? VK_SHADER_UNUSED_KHR : i,
                .closestHitShader = i == 3 ? 3 : VK_SHADER_UNUSED_KHR,
                .anyHitShader = VK_SHADER_UNUSED_KHR, .intersectionShader = VK_SHADER_UNUSED_KHR,
            };
        }
        VkRayTracingPipelineCreateInfoKHR rt_info = {
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR, .stageCount = 4, .pStages = stages,
            .groupCount = 4, .pGroups = groups, .maxPipelineRayRecursionDepth = 1, .layout = layout,
        };
        CHECK(vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rt_info, NULL, &pipeline));
        /* SBT: raygen 1、miss 2、hit 1。各領域の先頭は shaderGroupBaseAlignment、要素は shaderGroupHandleAlignment */
        const uint32_t handle_size = rt_properties.shaderGroupHandleSize;
        const uint32_t handle_align = rt_properties.shaderGroupHandleAlignment ? rt_properties.shaderGroupHandleAlignment : 1;
        const uint32_t base_align = rt_properties.shaderGroupBaseAlignment ? rt_properties.shaderGroupBaseAlignment : 1;
        if (!handle_size || handle_size > 256) die("Invalid shaderGroupHandleSize");
        const VkDeviceSize stride = (handle_size + handle_align - 1) / handle_align * handle_align;
        const VkDeviceSize region = (stride * 2 + base_align - 1) / base_align * base_align;
        uint8_t handles[4 * 256];
        CHECK(vkGetRayTracingShaderGroupHandlesKHR(device, pipeline, 0, 4, 4 * handle_size, handles));
        sbt = create_buffer(region * 3 + base_align,
                            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            coherent);
        VkDeviceAddress sbt_base = buffer_address(sbt);
        const VkDeviceSize sbt_offset = (base_align - sbt_base % base_align) % base_align;
        uint8_t *table = (uint8_t *)sbt.mapped + sbt_offset;
        memcpy(table, handles, handle_size);                                     /* raygen */
        memcpy(table + region, handles + handle_size, handle_size);              /* miss 0 */
        memcpy(table + region + stride, handles + 2 * handle_size, handle_size); /* miss 1（影） */
        memcpy(table + 2 * region, handles + 3 * handle_size, handle_size);      /* hit */
        sbt_base += sbt_offset;
        raygen_region = (VkStridedDeviceAddressRegionKHR){sbt_base, stride, stride};
        miss_region = (VkStridedDeviceAddressRegionKHR){sbt_base + region, stride, stride * 2};
        hit_region = (VkStridedDeviceAddressRegionKHR){sbt_base + 2 * region, stride, stride};
    }

    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    VkCommandPool command_pool;
    CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    VkQueryPoolCreateInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4,
    };
    VkQueryPool queries;
    CHECK(vkCreateQueryPool(device, &query_info, NULL, &queries));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    const VkBuildAccelerationStructureFlagsKHR tlas_flags = tlas_build.flags;
    const VkAccelerationStructureBuildRangeInfoKHR blas_range = {.primitiveCount = triangle_count};
    const VkAccelerationStructureBuildRangeInfoKHR tlas_range = {.primitiveCount = instance_count};
    const VkAccelerationStructureBuildRangeInfoKHR *blas_range_ptr = &blas_range, *tlas_range_ptr = &tlas_range;
    VkMemoryBarrier as_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    };
    const float timestamp_ns = properties.limits.timestampPeriod;
    const VkMemoryBarrier compute_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT};

    /* command: [TLAS の BUILD/UPDATE] → Dispatch → [readback へコピー]。タイムスタンプ 0-1 が TLAS、2-3 が Dispatch。 */
#define RECORD_FRAME(tlas_mode, copy_out) do { \
        CHECK(vkResetCommandBuffer(command, 0)); \
        VkCommandBufferBeginInfo frame_begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, \
                                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; \
        CHECK(vkBeginCommandBuffer(command, &frame_begin)); \
        if (timestamps) vkCmdResetQueryPool(command, queries, 0, 4); \
        if ((tlas_mode) >= 0) { \
            tlas_build.mode = (VkBuildAccelerationStructureModeKHR)(tlas_mode); \
            tlas_build.srcAccelerationStructure = \
                (tlas_mode) == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? tlas : VK_NULL_HANDLE; \
            if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0); \
            vkCmdBuildAccelerationStructuresKHR(command, 1, &tlas_build, &tlas_range_ptr); \
            if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, queries, 1); \
            as_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR; \
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, \
                                 shader_pipeline_stage, 0, 1, &as_barrier, 0, NULL, 0, NULL); \
        } \
        vkCmdBindPipeline(command, bind_point, pipeline); \
        vkCmdBindDescriptorSets(command, bind_point, layout, 0, 1, &set, 0, NULL); \
        vkCmdPushConstants(command, layout, shader_stage, 0, 16, push); \
        if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 2); \
        for (uint32_t d = 0; d < dispatches; ++d) { \
            if (d) vkCmdPipelineBarrier(command, shader_pipeline_stage, \
                                        shader_pipeline_stage, 0, 1, &compute_barrier, 0, NULL, 0, NULL); \
            if (use_pipeline) \
                vkCmdTraceRaysKHR(command, &raygen_region, &miss_region, &hit_region, &callable_region, \
                                  WIDTH, HEIGHT, 1); \
            else \
                vkCmdDispatch(command, (WIDTH + 7) / 8, (HEIGHT + 7) / 8, 1); \
        } \
        if (timestamps) vkCmdWriteTimestamp(command, shader_pipeline_stage, queries, 3); \
        if (copy_out) { \
            VkMemoryBarrier shader_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, \
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT}; \
            vkCmdPipelineBarrier(command, shader_pipeline_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, \
                                 0, 1, &shader_barrier, 0, NULL, 0, NULL); \
            VkBufferCopy region = {.size = output_size}; \
            vkCmdCopyBuffer(command, output.handle, readback.handle, 1, &region); \
            VkMemoryBarrier host_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, \
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT}; \
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, \
                                 0, 1, &host_barrier, 0, NULL, 0, NULL); \
        } \
        CHECK(vkEndCommandBuffer(command)); \
    } while (0)
#define SUBMIT_WAIT() do { \
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command}; \
        CHECK(vkQueueSubmit(queue, 1, &submit, fence)); \
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(20000000000))); \
        CHECK(vkResetFences(device, 1, &fence)); \
    } while (0)

    uint32_t push[4] = {WIDTH, HEIGHT, 0, 0};
    float extent = (float)k;
    memcpy(&push[2], &extent, sizeof(extent));

    /* BLAS と最初の TLAS を作る（BLAS のビルド時間をタイムスタンプで測る） */
    fill_instances(instances.mapped, k, blas_address, dynamic, 0);
    CHECK(vkResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(command, &begin));
    if (timestamps) {
        vkCmdResetQueryPool(command, queries, 0, 4);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
    }
    vkCmdBuildAccelerationStructuresKHR(command, 1, &blas_build, &blas_range_ptr);
    if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, queries, 1);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &as_barrier, 0, NULL, 0, NULL);
    vkCmdBuildAccelerationStructuresKHR(command, 1, &tlas_build, &tlas_range_ptr);
    CHECK(vkEndCommandBuffer(command));
    SUBMIT_WAIT();
    double blas_build_ms = 0.0;
    uint64_t stamps[4] = {0};
    if (timestamps) {
        CHECK(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(uint64_t) * 2, stamps, sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        blas_build_ms = (double)(stamps[1] - stamps[0]) * timestamp_ns / 1e6;
    }

    /* 計測（1 フレームずつ投入して完了を待つ。最初の 20 フレームはウォームアップ） */
    double *frame_ms = calloc(MAX_FRAMES, sizeof(double));
    double *dispatch_ms = calloc(MAX_FRAMES, sizeof(double));
    double *update_ms = calloc(MAX_FRAMES, sizeof(double));
    if (!frame_ms || !dispatch_ms || !update_ms) die("Could not allocate timing arrays");
    size_t frames = 0;
    const double begin_ns = clock_ns();
    double last = begin_ns;
    for (uint32_t frame = 1; frame < MAX_FRAMES + 20; ++frame) {
        if (dynamic) fill_instances(instances.mapped, k, blas_address, 1, frame);
        RECORD_FRAME(dynamic ? (int)VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : -1, 0);
        SUBMIT_WAIT();
        const double now = clock_ns();
        if (frame > 20) {
            frame_ms[frames] = (now - last) / 1e6;
            if (timestamps) {
                /* static では TLAS の 0-1 番を書かないので、待つと終わらない */
                const uint32_t first = dynamic ? 0 : 2;
                CHECK(vkGetQueryPoolResults(device, queries, first, 4 - first, sizeof(uint64_t) * (4 - first),
                                            stamps + first, sizeof(uint64_t),
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
                dispatch_ms[frames] = (double)(stamps[3] - stamps[2]) * timestamp_ns / 1e6;
                update_ms[frames] = dynamic ? (double)(stamps[1] - stamps[0]) * timestamp_ns / 1e6 : 0.0;
            }
            ++frames;
            if ((now - begin_ns) / 1e9 >= seconds || frames >= MAX_FRAMES) break;
        }
        last = now;
    }

    /* 検算用のフレーム（決まった手順で作る） */
    if (dynamic) {
        tlas_build.flags = tlas_flags;
        fill_instances(instances.mapped, k, blas_address, 1, 1000);
        RECORD_FRAME((int)VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, 0);
        SUBMIT_WAIT();
        fill_instances(instances.mapped, k, blas_address, 1, 1001);
        RECORD_FRAME((int)VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR, 1);
    } else {
        RECORD_FRAME(-1, 1);
    }
    SUBMIT_WAIT();
    const uint32_t *pixels = readback.mapped;
    uint64_t hash = UINT64_C(14695981039346656037);
    uint64_t hits = 0, shadowed = 0;
    for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; ++i) {
        uint32_t p = pixels[i];
        if (p) ++hits;
        if (p & 0x80000000u) ++shadowed;
        for (int b = 0; b < 4; ++b) {
            hash ^= (p >> (8 * b)) & 0xffu;
            hash *= UINT64_C(1099511628211);
        }
    }
    const double total_s = (last - begin_ns) / 1e9;
    double rays_per_frame = ((double)WIDTH * HEIGHT + (double)hits) * dispatches;
    double fps = frames / ((clock_ns() - begin_ns) / 1e9);
    double f50 = percentile(frame_ms, frames, 0.5), f95 = percentile(frame_ms, frames, 0.95);
    double d50 = percentile(dispatch_ms, frames, 0.5);
    double u50 = percentile(update_ms, frames, 0.5);
    (void)total_s;
    printf("{\"status\":\"pass\",\"mode\":\"%s\",\"scene\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"api_version\":%u,"
           "\"grid\":%u,\"dispatches\":%u,\"triangles\":%u,\"instances\":%u,\"width\":%d,\"height\":%d,\"frames\":%zu,"
           "\"fps\":%.2f,\"frame_ms\":{\"p50\":%.4f,\"p95\":%.4f},\"gpu_dispatch_ms_p50\":%.4f,"
           "\"gpu_tlas_update_ms_p50\":%.4f,\"gpu_blas_build_ms\":%.4f,\"blas_bytes\":%llu,"
           "\"grays_per_s\":%.3f,\"hit_pixels\":%llu,\"shadowed_pixels\":%llu,\"checksum\":\"%016llx\"}\n",
           mode, scene, properties.vendorID, properties.deviceID, properties.apiVersion, grid, dispatches, triangle_count,
           instance_count, WIDTH, HEIGHT, frames, fps, f50, f95, d50, u50, blas_build_ms,
           (unsigned long long)blas_size.accelerationStructureSize,
           d50 > 0 ? rays_per_frame / (d50 * 1e6) : 0.0, (unsigned long long)hits,
           (unsigned long long)shadowed, (unsigned long long)hash);

    free(frame_ms);
    free(dispatch_ms);
    free(update_ms);
    vkDestroyFence(device, fence, NULL);
    vkDestroyQueryPool(device, queries, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    for (uint32_t i = 0; i < 4; ++i) {
        if (rt_modules[i]) vkDestroyShaderModule(device, rt_modules[i], NULL);
    }
    if (sbt.handle) destroy_buffer(sbt);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyAccelerationStructureKHR(device, tlas, NULL);
    vkDestroyAccelerationStructureKHR(device, blas, NULL);
    destroy_buffer(readback);
    destroy_buffer(output);
    destroy_buffer(scratch);
    destroy_buffer(tlas_storage);
    destroy_buffer(blas_storage);
    destroy_buffer(instances);
    destroy_buffer(indices);
    destroy_buffer(vertices);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
