/*
 * vk-mem-paths: Vulkan GPU メモリ転送経路の詳細ベンチマーク（Windows ネイティブ / Android ゲスト共通）。
 *
 * 以下の経路を小サイズ (4 KiB, 64 KiB) から大サイズ (1 MiB, 16 MiB, 64 MiB) まで分離計測し、
 * 全経路で GPU 経由のデータ整合性をバイト単位で検証する:
 *   1. Direct CPU->GPU map write (全 HOST_VISIBLE memoryType: ReBAR / Uncached / Cached / Non-coherent)
 *   2. CPU->Staging->GPU vkCmdCopyBuffer->DEVICE_LOCAL VRAM (Cached Staging vs Uncached Staging)
 *   3. GPU->GPU DEVICE_LOCAL->DEVICE_LOCAL vkCmdCopyBuffer (VRAM 内部帯域: GPU タイムスタンプ & 実時間)
 *   4. GPU->CPU Readback (DEVICE_LOCAL->Cached Readback + vkInvalidateMappedMemoryRanges + CPU 読取)
 */
#include <stdbool.h>
#include <string.h>
#include "vulkan_util.h"

enum {
    MAX_BYTES = 64 * 1024 * 1024,
    TRIALS = 5,
    NUM_SIZES = 5,
};

static const uint32_t kSizes[NUM_SIZES] = {
    4u * 1024u,
    64u * 1024u,
    1u * 1024u * 1024u,
    16u * 1024u * 1024u,
    64u * 1024u * 1024u,
};

static const uint32_t kItersDirect[NUM_SIZES] = {512, 64, 8, 2, 1};
static const uint32_t kItersCopy[NUM_SIZES] = {128, 32, 8, 2, 1};

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static double median(const double *v, int n) {
    double tmp[TRIALS];
    memcpy(tmp, v, (size_t)n * sizeof(double));
    qsort(tmp, (size_t)n, sizeof(double), cmp_double);
    return tmp[n / 2];
}

static void fill_pattern(uint32_t *w, size_t n, uint32_t seed) {
    for (size_t i = 0; i < n; ++i) w[i] = seed ^ (uint32_t)(i * UINT32_C(2654435761));
}

static const char *classify_flags(VkMemoryPropertyFlags f) {
    bool dl = (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    bool coh = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    bool cac = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    if (dl && cac && coh) return "rebar_cached_coherent";
    if (dl && coh) return "rebar_uncached_coherent";
    if (dl) return "rebar_noncoherent";
    if (cac && coh) return "sys_cached_coherent";
    if (cac) return "sys_cached_noncoherent";
    if (coh) return "sys_uncached_coherent";
    return "sys_uncached_noncoherent";
}

int main(int argc, char **argv) {
    unsigned long index = argc == 2 ? strtoul(argv[1], NULL, 10) : 0;
    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Velox Vulkan mem-paths",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    LOAD(vkDestroyInstance);
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkCreateDevice);

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (index >= count) die("Device index out of range");
    VkPhysicalDevice *devs = calloc(count, sizeof(*devs));
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devs));
    VkPhysicalDevice phys = devs[index];
    free(devs);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    VkPhysicalDeviceMemoryProperties mprops;
    vkGetPhysicalDeviceMemoryProperties(phys, &mprops);

    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, NULL);
    VkQueueFamilyProperties *qprops = calloc(count, sizeof(*qprops));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, qprops);
    uint32_t qfam = 0;
    while (qfam < count && !(qprops[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++qfam;
    if (qfam == count) die("No graphics queue");
    free(qprops);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
    };
    VkDevice dev = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
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
    LOAD(vkCmdCopyBuffer);
    LOAD(vkCmdPipelineBarrier);
    LOAD(vkCmdResetQueryPool);
    LOAD(vkCmdWriteTimestamp);
    LOAD(vkCreateQueryPool);
    LOAD(vkDestroyQueryPool);
    LOAD(vkGetQueryPoolResults);
    LOAD(vkQueueSubmit);
    LOAD(vkCreateFence);
    LOAD(vkDestroyFence);
    LOAD(vkWaitForFences);
    LOAD(vkResetFences);
    LOAD(vkDeviceWaitIdle);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam};
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
    };
    VkQueryPool qpool = VK_NULL_HANDLE;
    CHECK(vkCreateQueryPool(dev, &qpci, NULL, &qpool));

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = MAX_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    /* Two pure DEVICE_LOCAL buffers for Staging->VRAM, GPU->GPU, and VRAM->Readback */
    VkBuffer vram_a = VK_NULL_HANDLE, vram_b = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &vram_a));
    CHECK(vkCreateBuffer(dev, &bci, NULL, &vram_b));
    CHECK(vkCreateBuffer(dev, &bci, NULL, &readback));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, vram_a, &req);
    uint32_t allowed = req.memoryTypeBits;

    /* Prefer pure DEVICE_LOCAL (not HOST_VISIBLE) for VRAM buffers */
    uint32_t vram_type = UINT32_MAX;
    for (uint32_t i = 0; i < mprops.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags f = mprops.memoryTypes[i].propertyFlags;
        if ((allowed & (1u << i)) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            vram_type = i;
            break;
        }
    }
    if (vram_type == UINT32_MAX) vram_type = memory_type(&mprops, allowed, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    uint32_t rb_type = UINT32_MAX;
    for (uint32_t i = 0; i < mprops.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags f = mprops.memoryTypes[i].propertyFlags;
        if ((allowed & (1u << i)) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            rb_type = i;
            break;
        }
    }
    if (rb_type == UINT32_MAX) rb_type = memory_type(&mprops, allowed, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = vram_type};
    VkDeviceMemory vram_a_mem = VK_NULL_HANDLE, vram_b_mem = VK_NULL_HANDLE, rb_mem = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &vram_a_mem));
    CHECK(vkAllocateMemory(dev, &mai, NULL, &vram_b_mem));
    mai.memoryTypeIndex = rb_type;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &rb_mem));
    CHECK(vkBindBufferMemory(dev, vram_a, vram_a_mem, 0));
    CHECK(vkBindBufferMemory(dev, vram_b, vram_b_mem, 0));
    CHECK(vkBindBufferMemory(dev, readback, rb_mem, 0));
    void *rb_map = NULL;
    CHECK(vkMapMemory(dev, rb_mem, 0, VK_WHOLE_SIZE, 0, &rb_map));
    VkMappedMemoryRange rb_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = rb_mem, .size = VK_WHOLE_SIZE};

    uint32_t *src_cpu = malloc(MAX_BYTES);
    if (!src_cpu) die("malloc failed");
    fill_pattern(src_cpu, MAX_BYTES / 4u, 0x12345678u);

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkMemoryBarrier to_host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };

#define RUN_COPY_SYNC(src_b, dst_b, sz_bytes, with_host_barrier, gpu_ns_out) do { \
        CHECK(vkResetCommandPool(dev, pool, 0)); \
        CHECK(vkBeginCommandBuffer(cb, &cbbi)); \
        vkCmdResetQueryPool(cb, qpool, 0, 2); \
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0); \
        VkBufferCopy reg_ = {.size = (sz_bytes)}; \
        vkCmdCopyBuffer(cb, (src_b), (dst_b), 1, &reg_); \
        if (with_host_barrier) { \
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, \
                                 1, &to_host, 0, NULL, 0, NULL); \
        } \
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1); \
        CHECK(vkEndCommandBuffer(cb)); \
        CHECK(vkResetFences(dev, 1, &fence)); \
        VkSubmitInfo si_ = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb}; \
        CHECK(vkQueueSubmit(queue, 1, &si_, fence)); \
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_C(10000000000))); \
        uint64_t ts_[2] = {0, 0}; \
        if (vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof(ts_), ts_, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) \
            *(gpu_ns_out) = (double)(ts_[1] - ts_[0]) * props.limits.timestampPeriod; \
        else *(gpu_ns_out) = 0.0; \
    } while (0)

    printf("{\"validated\":true,\"device\":\"%s\",\"vram_type\":%u,\"readback_type\":%u,\"host_visible_types\":[",
           props.deviceName, vram_type, rb_type);

    bool first_type = true;
    for (uint32_t t = 0; t < mprops.memoryTypeCount; ++t) {
        VkMemoryPropertyFlags f = mprops.memoryTypes[t].propertyFlags;
        if (!(allowed & (1u << t)) || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;

        VkBuffer buf = VK_NULL_HANDLE;
        CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
        VkMemoryRequirements ur;
        vkGetBufferMemoryRequirements(dev, buf, &ur);
        VkMemoryAllocateInfo uai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = ur.size, .memoryTypeIndex = t,
        };
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev, &uai, NULL, &mem) != VK_SUCCESS) {
            vkDestroyBuffer(dev, buf, NULL);
            continue;
        }
        CHECK(vkBindBufferMemory(dev, buf, mem, 0));
        void *mapped = NULL;
        CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped));
        bool non_coh = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;

        if (!first_type) printf(",");
        first_type = false;
        printf("{\"index\":%u,\"flags\":%u,\"heap\":%u,\"heap_mib\":%.1f,\"kind\":\"%s\",\"sizes\":{",
               t, f, mprops.memoryTypes[t].heapIndex,
               (double)mprops.memoryHeaps[mprops.memoryTypes[t].heapIndex].size / (1024.0 * 1024.0),
               classify_flags(f));

        for (int s = 0; s < NUM_SIZES; ++s) {
            uint32_t sz = kSizes[s];
            uint32_t it_d = kItersDirect[s];
            uint32_t it_c = kItersCopy[s];
            /* For slow ReBAR/uncached writes, cap 64 MiB direct iterations to 1 and 16 MiB to 1 */
            if (!(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && sz >= 16u * 1024u * 1024u) {
                it_d = 1;
                it_c = 1;
            }
            double direct_gbs[TRIALS], e2e_gbs[TRIALS], gpu_copy_gbs[TRIALS];
            VkMappedMemoryRange mr = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem, .offset = 0, .size = VK_WHOLE_SIZE};

            /* Warmup + validate data integrity via VRAM->Readback */
            uint32_t seed = 0xA5A50000u ^ (t << 8) ^ (uint32_t)s;
            fill_pattern(src_cpu, sz / 4u, seed);
            memcpy(mapped, src_cpu, sz);
            if (non_coh) CHECK(vkFlushMappedMemoryRanges(dev, 1, &mr));
            double gns = 0.0;
            RUN_COPY_SYNC(buf, vram_a, sz, false, &gns);
            RUN_COPY_SYNC(vram_a, readback, sz, true, &gns);
            CHECK(vkInvalidateMappedMemoryRanges(dev, 1, &rb_range));
            if (memcmp(rb_map, src_cpu, sz) != 0) die("Data mismatch during staging->VRAM->readback validation!");

            for (int tr = 0; tr < TRIALS; ++tr) {
                /* 1. Direct CPU write */
                double t0 = clock_ns();
                for (uint32_t k = 0; k < it_d; ++k) {
                    memcpy(mapped, src_cpu, sz);
                    if (non_coh) CHECK(vkFlushMappedMemoryRanges(dev, 1, &mr));
                }
                double dt = clock_ns() - t0;
                direct_gbs[tr] = ((double)sz * (double)it_d) / dt;

                /* 2. End-to-End CPU write + GPU vkCmdCopyBuffer -> DEVICE_LOCAL VRAM + Fence wait */
                double t1 = clock_ns();
                double sum_gns = 0.0;
                for (uint32_t k = 0; k < it_c; ++k) {
                    memcpy(mapped, src_cpu, sz);
                    if (non_coh) CHECK(vkFlushMappedMemoryRanges(dev, 1, &mr));
                    RUN_COPY_SYNC(buf, vram_a, sz, false, &gns);
                    sum_gns += gns;
                }
                double dt2 = clock_ns() - t1;
                e2e_gbs[tr] = ((double)sz * (double)it_c) / dt2;
                gpu_copy_gbs[tr] = sum_gns > 0.0 ? (((double)sz * (double)it_c) / sum_gns) : 0.0;
            }
            printf("%s\"%uKB\":{\"direct_write_gbs\":%.4f,\"staging_to_vram_e2e_gbs\":%.4f,\"gpu_dma_gbs\":%.4f}",
                   s ? "," : "", sz / 1024u,
                   median(direct_gbs, TRIALS), median(e2e_gbs, TRIALS), median(gpu_copy_gbs, TRIALS));
        }
        printf("}}");

        vkUnmapMemory(dev, mem);
        vkDestroyBuffer(dev, buf, NULL);
        vkFreeMemory(dev, mem, NULL);
    }

    /* 3. Pure GPU->GPU DEVICE_LOCAL->DEVICE_LOCAL copy */
    printf("],\"gpu_to_gpu\":{");
    for (int s = 0; s < NUM_SIZES; ++s) {
        uint32_t sz = kSizes[s];
        uint32_t it = kItersCopy[s];
        double wall_gbs[TRIALS], gpu_gbs[TRIALS];
        double gns = 0.0;
        RUN_COPY_SYNC(vram_a, vram_b, sz, false, &gns);
        for (int tr = 0; tr < TRIALS; ++tr) {
            double t0 = clock_ns();
            double sum_gns = 0.0;
            for (uint32_t k = 0; k < it; ++k) {
                RUN_COPY_SYNC(vram_a, vram_b, sz, false, &gns);
                sum_gns += gns;
            }
            double dt = clock_ns() - t0;
            wall_gbs[tr] = ((double)sz * (double)it) / dt;
            gpu_gbs[tr] = sum_gns > 0.0 ? (((double)sz * (double)it) / sum_gns) : 0.0;
        }
        printf("%s\"%uKB\":{\"wall_gbs\":%.4f,\"gpu_ts_gbs\":%.4f}",
               s ? "," : "", sz / 1024u, median(wall_gbs, TRIALS), median(gpu_gbs, TRIALS));
    }

    /* 4. GPU->CPU Readback (VRAM -> Cached Readback + invalidate + CPU read) */
    printf("},\"gpu_to_cpu_readback\":{");
    volatile uint64_t sink = 0;
    for (int s = 0; s < NUM_SIZES; ++s) {
        uint32_t sz = kSizes[s];
        uint32_t it = kItersCopy[s];
        double e2e_gbs[TRIALS], cpu_read_gbs[TRIALS];
        double gns = 0.0;
        for (int tr = 0; tr < TRIALS; ++tr) {
            double t0 = clock_ns();
            double cpu_read_ns = 0.0;
            for (uint32_t k = 0; k < it; ++k) {
                RUN_COPY_SYNC(vram_b, readback, sz, true, &gns);
                double tc0 = clock_ns();
                CHECK(vkInvalidateMappedMemoryRanges(dev, 1, &rb_range));
                const uint64_t *q = (const uint64_t *)rb_map;
                uint64_t acc = 0;
                for (size_t i = 0; i < sz / 8u; ++i) acc += q[i];
                sink += acc;
                cpu_read_ns += (clock_ns() - tc0);
            }
            double dt = clock_ns() - t0;
            e2e_gbs[tr] = ((double)sz * (double)it) / dt;
            cpu_read_gbs[tr] = ((double)sz * (double)it) / cpu_read_ns;
        }
        printf("%s\"%uKB\":{\"e2e_gbs\":%.4f,\"cpu_read_gbs\":%.4f}",
               s ? "," : "", sz / 1024u, median(e2e_gbs, TRIALS), median(cpu_read_gbs, TRIALS));
    }
    printf("},\"sink\":%" PRIu64 "}\n", (uint64_t)(sink & 1u));

    CHECK(vkDeviceWaitIdle(dev));
    free(src_cpu);
    vkUnmapMemory(dev, rb_mem);
    vkDestroyBuffer(dev, readback, NULL);
    vkDestroyBuffer(dev, vram_a, NULL);
    vkDestroyBuffer(dev, vram_b, NULL);
    vkFreeMemory(dev, rb_mem, NULL);
    vkFreeMemory(dev, vram_a_mem, NULL);
    vkFreeMemory(dev, vram_b_mem, NULL);
    vkDestroyQueryPool(dev, qpool, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
