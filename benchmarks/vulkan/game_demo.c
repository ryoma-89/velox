/*
 * vk-game-demo: 実ゲームに近い複合 3D Vulkan ワークロード（Windows ネイティブ / Android ゲスト共通）。
 *
 * 1 フレームあたり以下を実行する:
 *   - テクスチャストリーミング: 512x512 RGBA8 タイル (1 MiB) を毎フレーム vkCmdCopyBufferToImage で更新
 *   - GPU Compute: 16,384 インスタンスの 3D 変換・色を DEVICE_LOCAL SSBO で計算し描画へバリア連携
 *   - 頻繁な Descriptor Set 更新: 毎フレーム vkResetDescriptorPool + 48 Set 割当・更新 (UBO+SSBO+Texture)
 *   - 頻繁なリソース作成・破棄: 毎フレーム 2 個の一時 VkBuffer 作成・破棄（10 フレーム毎に小メモリ割当・解放）
 *   - 3,200 Draw Call: 3 種類のパイプライン（不透明 3D PBR 立方体 / 半透明 FX / フルスクリーン後処理）を切替
 *   - 任意で実行時パイプライン再作成（シェーダーコンパイル時間計測）と背景 CPU スレッド負荷を追加可能
 *
 * 検算: 計測後に固定フレーム (seed=0) を描画・読み戻し、1920x1080 RGBA8 の FNV-1a 64 チェックサムを出力する。
 *
 * 使い方:
 *   vk-game-demo [--seconds N] [--pace-fps F] [--draws N] [--cpu-threads N]
 *                [--recompile-interval N] [--no-stream] [device-index]
 */
#include <math.h>
#include <stdbool.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include "vulkan_util.h"

static const uint32_t comp_code[] =
#include "game_demo.spv.h"
;
static const uint32_t vert_code[] =
#include "game_demo_vert.spv.h"
;
static const uint32_t frag_code[] =
#include "game_demo_frag.spv.h"
;

enum {
    WIDTH = 1920,
    HEIGHT = 1080,
    TEX_SIZE = 1024,
    TILE_SIZE = 512,
    IN_FLIGHT = 2,
    INSTANCE_COUNT = 16384,
    FRAME_SETS = 48,
    MAX_SAMPLES = 200000,
    MAX_CPU_THREADS = 16,
};

typedef struct {
    uint32_t count;
    uint32_t seed;
    float time_sec;
    uint32_t pad;
} CompPush;

typedef struct {
    uint32_t base_instance;
    uint32_t mode;
    uint32_t seed;
    uint32_t iterations;
} DrawPush;

typedef struct {
    float light_dir[4];
    float light_color[4];
    float camera_pos[4];
    float params[4];
} FrameUBO;

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(const double *src, size_t n, double p) {
    if (!n) return 0.0;
    double *tmp = malloc(n * sizeof(double));
    if (!tmp) die("out of memory");
    memcpy(tmp, src, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double);
    double idx = p * (double)(n - 1);
    size_t lo = (size_t)idx;
    size_t hi = lo + 1 < n ? lo + 1 : lo;
    double frac = idx - (double)lo;
    double val = tmp[lo] * (1.0 - frac) + tmp[hi] * frac;
    free(tmp);
    return val;
}

static void pace_until_ns(double target_ns) {
    for (;;) {
        double rem = target_ns - clock_ns();
        if (rem <= 0.0) break;
        if (rem > 2000000.0) {
#ifdef _WIN32
            Sleep((DWORD)((rem - 1500000.0) / 1e6));
#else
            double s_ns = rem - 500000.0;
            struct timespec ts = { .tv_sec = (time_t)(s_ns / 1e9), .tv_nsec = (long)fmod(s_ns, 1e9) };
            nanosleep(&ts, NULL);
#endif
        } else {
#ifdef _WIN32
            YieldProcessor();
#else
            __asm__ volatile("pause");
#endif
        }
    }
}

static double process_rss_mib(void) {
#ifdef _WIN32
    typedef struct {
        DWORD cb;
        DWORD PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
    } PMC;
    typedef BOOL(WINAPI *PFN_K32GetProcessMemoryInfo)(HANDLE, PMC *, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        PFN_K32GetProcessMemoryInfo fn = (PFN_K32GetProcessMemoryInfo)(void *)GetProcAddress(k32, "K32GetProcessMemoryInfo");
        PMC pmc;
        memset(&pmc, 0, sizeof(pmc));
        pmc.cb = sizeof(pmc);
        if (fn && fn(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
        }
    }
    return 0.0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0.0;
    unsigned long size = 0, resident = 0;
    int ok = fscanf(f, "%lu %lu", &size, &resident);
    fclose(f);
    if (ok == 2) {
        long page = sysconf(_SC_PAGESIZE);
        return (double)resident * (double)page / (1024.0 * 1024.0);
    }
    return 0.0;
#endif
}

/* Optional background CPU workers simulating game physics/AI parallel to rendering */
static volatile int g_cpu_stop = 0;
static volatile uint64_t g_cpu_sink = 0;

#ifdef _WIN32
static DWORD WINAPI cpu_worker_fn(LPVOID arg) {
    uint64_t x = (uint64_t)(uintptr_t)arg * UINT64_C(0x9e3779b97f4a7c15) + 1;
    while (!g_cpu_stop) {
        for (int i = 0; i < 4096; ++i) {
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            x *= UINT64_C(2685821657736338717);
        }
    }
    g_cpu_sink ^= x;
    return 0;
}
#else
static void *cpu_worker_fn(void *arg) {
    uint64_t x = (uint64_t)(uintptr_t)arg * UINT64_C(0x9e3779b97f4a7c15) + 1;
    while (!__atomic_load_n(&g_cpu_stop, __ATOMIC_RELAXED)) {
        for (int i = 0; i < 4096; ++i) {
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            x *= UINT64_C(2685821657736338717);
        }
    }
    __atomic_fetch_xor(&g_cpu_sink, x, __ATOMIC_RELAXED);
    return NULL;
}
#endif

#define FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdDispatch) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkDeviceWaitIdle)

#define DECLARE(name) static PFN_##name name;
FUNCS(DECLARE)

static VkDevice g_device;
static VkPhysicalDeviceMemoryProperties g_mem_props;

static uint32_t find_host_cached_type(uint32_t allowed) {
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < g_mem_props.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags f = g_mem_props.memoryTypes[i].propertyFlags;
        if ((allowed & (1u << i)) && (f & want) == want && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            return i;
    }
    return memory_type(&g_mem_props, allowed, want);
}

static VkDeviceMemory alloc_mem(VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
    VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type(&g_mem_props, req.memoryTypeBits, flags),
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(g_device, &info, NULL, &mem));
    return mem;
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_cached,
                            VkDeviceMemory *mem_out, void **map_out) {
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(g_device, &info, NULL, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_device, buf, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = host_cached
            ? find_host_cached_type(req.memoryTypeBits)
            : memory_type(&g_mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    CHECK(vkAllocateMemory(g_device, &alloc, NULL, mem_out));
    CHECK(vkBindBufferMemory(g_device, buf, *mem_out, 0));
    if (map_out) CHECK(vkMapMemory(g_device, *mem_out, 0, VK_WHOLE_SIZE, 0, map_out));
    return buf;
}

static VkImage make_image(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkDeviceMemory *mem_out) {
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img = VK_NULL_HANDLE;
    CHECK(vkCreateImage(g_device, &info, NULL, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_device, img, &req);
    *mem_out = alloc_mem(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkBindImageMemory(g_device, img, *mem_out, 0));
    return img;
}

static VkImageView make_view(VkImage img, VkFormat fmt, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    CHECK(vkCreateImageView(g_device, &info, NULL, &view));
    return view;
}

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                        VkImageLayout old_l, VkImageLayout new_l,
                        VkAccessFlags src_a, VkAccessFlags dst_a,
                        VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_a,
        .dstAccessMask = dst_a,
        .oldLayout = old_l,
        .newLayout = new_l,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cb, src_s, dst_s, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkPipeline build_gfx_pipeline(VkRenderPass rp, VkPipelineLayout layout,
                                     VkShaderModule vs, VkShaderModule fs,
                                     bool blend, bool depth_write, VkCompareOp depth_op) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = {0.0f, 0.0f, (float)WIDTH, (float)HEIGHT, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {WIDTH, HEIGHT}};
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = depth_op,
    };
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xFu,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &ba,
    };
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds,
        .pColorBlendState = &cb, .layout = layout, .renderPass = rp,
    };
    VkPipeline p = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pci, NULL, &p));
    return p;
}

static void fill_tile(uint8_t *dst, uint32_t w, uint32_t h, uint32_t seed) {
    uint32_t *px = (uint32_t *)dst;
    uint32_t s = seed * 2654435761u + 17u;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t r = ((x * 3u + s) ^ (y * 5u)) & 255u;
            uint32_t g = ((y * 7u + (s >> 3)) ^ (x * 2u)) & 255u;
            uint32_t b = (((x ^ y) * 11u + (s >> 7))) & 255u;
            px[y * w + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

int main(int argc, char **argv) {
    double t_proc_start = clock_ns();
    double seconds = 10.0;
    double pace_fps = 0.0;
    uint32_t draws_per_frame = 3200;
    uint32_t cpu_threads = 0;
    uint32_t recompile_interval = 0;
    bool enable_stream = true;
    unsigned long dev_index = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--pace-fps") && i + 1 < argc) pace_fps = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--draws") && i + 1 < argc) draws_per_frame = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--cpu-threads") && i + 1 < argc) cpu_threads = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--recompile-interval") && i + 1 < argc) recompile_interval = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--no-stream")) enable_stream = false;
        else dev_index = strtoul(argv[i], NULL, 10);
    }
    if (cpu_threads > MAX_CPU_THREADS) cpu_threads = MAX_CPU_THREADS;
    if (draws_per_frame < 64) draws_per_frame = 64;

    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(vkCreateInstance);
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Velox 3D Game Demo",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &instance));
#define LOAD_G(name) name = (PFN_##name)lookup(instance, #name); if (!name) die("Missing " #name);
    FUNCS(LOAD_G)

    uint32_t pcount = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &pcount, NULL));
    if (dev_index >= pcount) die("Device index out of range");
    VkPhysicalDevice *devs = calloc(pcount, sizeof(*devs));
    CHECK(vkEnumeratePhysicalDevices(instance, &pcount, devs));
    VkPhysicalDevice phys = devs[dev_index];
    free(devs);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &g_mem_props);

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, NULL);
    VkQueueFamilyProperties *qprops = calloc(qcount, sizeof(*qprops));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qprops);
    uint32_t qfam = 0;
    while (qfam < qcount && !(qprops[qfam].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) ++qfam;
    if (qfam == qcount) die("No graphics+compute queue");
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
    CHECK(vkCreateDevice(phys, &dci, NULL, &g_device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(g_device, qfam, 0, &queue);

    /* Render target + depth + streamable 1024x1024 texture atlas */
    VkDeviceMemory color_mem, depth_mem, tex_mem;
    VkImage color_img = make_image(VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &color_mem);
    VkImage depth_img = make_image(VK_FORMAT_D32_SFLOAT, WIDTH, HEIGHT,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &depth_mem);
    VkImage tex_img = make_image(VK_FORMAT_R8G8B8A8_UNORM, TEX_SIZE, TEX_SIZE,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &tex_mem);
    VkImageView color_view = make_view(color_img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView depth_view = make_view(depth_img, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView tex_view = make_view(tex_img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

    VkSampler samplers[2];
    for (int i = 0; i < 2; ++i) {
        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        };
        CHECK(vkCreateSampler(g_device, &sci, NULL, &samplers[i]));
    }

    /* RenderPass & Framebuffer */
    VkAttachmentDescription atts[2] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
    };
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &dref,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = atts, .subpassCount = 1, .pSubpasses = &subpass,
    };
    VkRenderPass rp = VK_NULL_HANDLE;
    CHECK(vkCreateRenderPass(g_device, &rpci, NULL, &rp));
    VkImageView fb_views[2] = {color_view, depth_view};
    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp, .attachmentCount = 2, .pAttachments = fb_views,
        .width = WIDTH, .height = HEIGHT, .layers = 1,
    };
    VkFramebuffer fb = VK_NULL_HANDLE;
    CHECK(vkCreateFramebuffer(g_device, &fbci, NULL, &fb));

    /* Compute & Graphics Descriptor Set Layouts + Pipeline Layouts */
    VkDescriptorSetLayoutBinding comp_b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo comp_dsl_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &comp_b,
    };
    VkDescriptorSetLayout comp_dsl = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(g_device, &comp_dsl_ci, NULL, &comp_dsl));

    VkDescriptorSetLayoutBinding gfx_b[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo gfx_dsl_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = gfx_b,
    };
    VkDescriptorSetLayout gfx_dsl = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(g_device, &gfx_dsl_ci, NULL, &gfx_dsl));

    VkPushConstantRange comp_pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CompPush)};
    VkPipelineLayoutCreateInfo comp_pl_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &comp_dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &comp_pcr,
    };
    VkPipelineLayout comp_pl = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(g_device, &comp_pl_ci, NULL, &comp_pl));

    VkPushConstantRange gfx_pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPush)};
    VkPipelineLayoutCreateInfo gfx_pl_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &gfx_dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &gfx_pcr,
    };
    VkPipelineLayout gfx_pl = VK_NULL_HANDLE;
    CHECK(vkCreatePipelineLayout(g_device, &gfx_pl_ci, NULL, &gfx_pl));

    /* Cold shader + pipeline compilation timing */
    double t_shader0 = clock_ns();
    VkShaderModuleCreateInfo cs_ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(comp_code), .pCode = comp_code};
    VkShaderModuleCreateInfo vs_ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vert_code), .pCode = vert_code};
    VkShaderModuleCreateInfo fs_ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(frag_code), .pCode = frag_code};
    VkShaderModule cs = VK_NULL_HANDLE, vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(g_device, &cs_ci, NULL, &cs));
    CHECK(vkCreateShaderModule(g_device, &vs_ci, NULL, &vs));
    CHECK(vkCreateShaderModule(g_device, &fs_ci, NULL, &fs));

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main"},
        .layout = comp_pl,
    };
    VkPipeline comp_pipe = VK_NULL_HANDLE;
    CHECK(vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &cpci, NULL, &comp_pipe));
    VkPipeline gfx_pipes[3];
    gfx_pipes[0] = build_gfx_pipeline(rp, gfx_pl, vs, fs, false, true, VK_COMPARE_OP_LESS);
    gfx_pipes[1] = build_gfx_pipeline(rp, gfx_pl, vs, fs, true, false, VK_COMPARE_OP_LESS_OR_EQUAL);
    gfx_pipes[2] = build_gfx_pipeline(rp, gfx_pl, vs, fs, true, false, VK_COMPARE_OP_ALWAYS);
    double shader_compile_ms = (clock_ns() - t_shader0) / 1e6;

    /* Buffers: DEVICE_LOCAL SSBO (16384 * 32B = 512 KiB), per-slot UBO + tile staging (1 MiB), readback (8 MiB) */
    const VkDeviceSize ssbo_bytes = (VkDeviceSize)INSTANCE_COUNT * 32u;
    const VkDeviceSize tile_bytes = (VkDeviceSize)TILE_SIZE * TILE_SIZE * 4u;
    const VkDeviceSize full_tex_bytes = (VkDeviceSize)TEX_SIZE * TEX_SIZE * 4u;
    const VkDeviceSize readback_bytes = (VkDeviceSize)WIDTH * HEIGHT * 4u;

    VkDeviceMemory ssbo_mem, readback_mem;
    VkBuffer ssbo_buf = make_buffer(ssbo_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    false, &ssbo_mem, NULL);
    void *readback_map = NULL;
    VkBuffer readback_buf = make_buffer(readback_bytes > full_tex_bytes ? readback_bytes : full_tex_bytes,
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        true, &readback_mem, &readback_map);

    VkBuffer ubo_buf[IN_FLIGHT], tile_buf[IN_FLIGHT];
    VkDeviceMemory ubo_mem[IN_FLIGHT], tile_mem[IN_FLIGHT];
    void *ubo_map[IN_FLIGHT], *tile_map[IN_FLIGHT];
    for (int i = 0; i < IN_FLIGHT; ++i) {
        ubo_buf[i] = make_buffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &ubo_mem[i], &ubo_map[i]);
        tile_buf[i] = make_buffer(tile_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &tile_mem[i], &tile_map[i]);
    }

    /* Descriptor Pools: static compute pool + per-frame dynamic graphics pools */
    VkDescriptorPoolSize comp_ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo comp_dp_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &comp_ps,
    };
    VkDescriptorPool comp_dp = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorPool(g_device, &comp_dp_ci, NULL, &comp_dp));
    VkDescriptorSetAllocateInfo comp_dsa = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp_dp, .descriptorSetCount = 1, .pSetLayouts = &comp_dsl,
    };
    VkDescriptorSet comp_set = VK_NULL_HANDLE;
    CHECK(vkAllocateDescriptorSets(g_device, &comp_dsa, &comp_set));
    VkDescriptorBufferInfo comp_bi = {ssbo_buf, 0, ssbo_bytes};
    VkWriteDescriptorSet comp_w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = comp_set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &comp_bi,
    };
    vkUpdateDescriptorSets(g_device, 1, &comp_w, 0, NULL);

    VkDescriptorPool frame_dp[IN_FLIGHT];
    VkDescriptorPoolSize gfx_ps[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAME_SETS},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FRAME_SETS},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAME_SETS},
    };
    VkDescriptorPoolCreateInfo gfx_dp_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = FRAME_SETS, .poolSizeCount = 3, .pPoolSizes = gfx_ps,
    };
    for (int i = 0; i < IN_FLIGHT; ++i) CHECK(vkCreateDescriptorPool(g_device, &gfx_dp_ci, NULL, &frame_dp[i]));

    /* Command pool, buffers, fences, queries */
    VkCommandPoolCreateInfo cp_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qfam,
    };
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(g_device, &cp_ci, NULL, &cmd_pool));
    VkCommandBuffer cbs[IN_FLIGHT];
    VkCommandBufferAllocateInfo cba = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = IN_FLIGHT,
    };
    CHECK(vkAllocateCommandBuffers(g_device, &cba, cbs));

    VkFence fences[IN_FLIGHT];
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < IN_FLIGHT; ++i) CHECK(vkCreateFence(g_device, &fci, NULL, &fences[i]));

    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * IN_FLIGHT,
    };
    VkQueryPool qpool = VK_NULL_HANDLE;
    CHECK(vkCreateQueryPool(g_device, &qpci, NULL, &qpool));

    /* Upload initial full 1024x1024 atlas */
    fill_tile((uint8_t *)readback_map, TEX_SIZE, TEX_SIZE, 0);
    VkCommandBufferBeginInfo begin_one = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(cbs[0], &begin_one));
    img_barrier(cbs[0], tex_img, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy full_copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {TEX_SIZE, TEX_SIZE, 1},
    };
    vkCmdCopyBufferToImage(cbs[0], readback_buf, tex_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full_copy);
    img_barrier(cbs[0], tex_img, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    CHECK(vkEndCommandBuffer(cbs[0]));
    VkSubmitInfo init_sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbs[0]};
    CHECK(vkQueueSubmit(queue, 1, &init_sub, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    double startup_ms = (clock_ns() - t_proc_start) / 1e6;

    /* Start optional background CPU load threads */
#ifdef _WIN32
    HANDLE cpu_handles[MAX_CPU_THREADS];
    for (uint32_t t = 0; t < cpu_threads; ++t)
        cpu_handles[t] = CreateThread(NULL, 0, cpu_worker_fn, (LPVOID)(uintptr_t)(t + 1), 0, NULL);
#else
    pthread_t cpu_handles[MAX_CPU_THREADS];
    for (uint32_t t = 0; t < cpu_threads; ++t)
        pthread_create(&cpu_handles[t], NULL, cpu_worker_fn, (void *)(uintptr_t)(t + 1));
#endif

    double *frame_ms = calloc(MAX_SAMPLES, sizeof(double));
    double *cpu_ms = calloc(MAX_SAMPLES, sizeof(double));
    double *gpu_ms = calloc(MAX_SAMPLES, sizeof(double));
    double *present_ms = calloc(MAX_SAMPLES, sizeof(double));
    if (!frame_ms || !cpu_ms || !gpu_ms || !present_ms) die("out of memory");

    VkDescriptorSetLayout set_layouts[FRAME_SETS];
    for (int i = 0; i < FRAME_SETS; ++i) set_layouts[i] = gfx_dsl;

    VkPipeline deferred_destroy_pipe = VK_NULL_HANDLE;
    double runtime_compile_total_ms = 0.0;
    uint32_t runtime_compile_count = 0;

    /* Shared small memory for per-frame transient VkBuffer create/bind/destroy churn */
    VkBuffer probe_buf = VK_NULL_HANDLE;
    VkBufferCreateInfo churn_bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    CHECK(vkCreateBuffer(g_device, &churn_bci, NULL, &probe_buf));
    VkMemoryRequirements churn_req;
    vkGetBufferMemoryRequirements(g_device, probe_buf, &churn_req);
    vkDestroyBuffer(g_device, probe_buf, NULL);
    VkDeviceMemory churn_pool_mem = alloc_mem(churn_req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    double submit_ts[IN_FLIGHT] = {0.0, 0.0};
    const double ts_period = props.limits.timestampPeriod;
    const unsigned long warmup = 24;
    const double pace_period_ns = pace_fps > 0.0 ? (1e9 / pace_fps) : 0.0;
    double next_pace_ns = 0.0;
    double start_ns = 0.0, last_ns = clock_ns();
    size_t samples = 0, gpu_samples = 0, present_samples = 0;
    unsigned long frames = 0;

    VkClearValue clears[2] = {{.color = {{0.05f, 0.07f, 0.11f, 1.0f}}}, {.depthStencil = {1.0f, 0}}};

    for (;;) {
        if (pace_period_ns > 0.0 && frames > warmup) {
            if (next_pace_ns == 0.0) next_pace_ns = clock_ns() + pace_period_ns;
            pace_until_ns(next_pace_ns);
            next_pace_ns += pace_period_ns;
            double cur = clock_ns();
            if (next_pace_ns < cur) next_pace_ns = cur;
        }
        uint32_t slot = (uint32_t)(frames % IN_FLIGHT);
        CHECK(vkWaitForFences(g_device, 1, &fences[slot], VK_TRUE, UINT64_C(10000000000)));
        double wait_done = clock_ns();

        if (frames > warmup + IN_FLIGHT) {
            if (present_samples < MAX_SAMPLES && submit_ts[slot] > 0.0)
                present_ms[present_samples++] = (wait_done - submit_ts[slot]) / 1e6;
            uint64_t stamps[2];
            if (vkGetQueryPoolResults(g_device, qpool, 2 * slot, 2, sizeof(stamps), stamps,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                gpu_samples < MAX_SAMPLES) {
                gpu_ms[gpu_samples++] = (double)(stamps[1] - stamps[0]) * ts_period / 1e6;
            }
        }

        double now = clock_ns();
        if (frames == warmup) start_ns = now;
        if (frames > warmup && samples < MAX_SAMPLES) frame_ms[samples] = (now - last_ns) / 1e6;
        last_ns = now;
        if (frames >= warmup && (now - start_ns) >= seconds * 1e9) break;

        double cpu0 = clock_ns();
        CHECK(vkResetFences(g_device, 1, &fences[slot]));

        /* Resource create/destroy churn (2 buffers/frame + 1 small alloc/free every 10 frames) */
        for (int c = 0; c < 2; ++c) {
            VkBuffer tb = VK_NULL_HANDLE;
            CHECK(vkCreateBuffer(g_device, &churn_bci, NULL, &tb));
            CHECK(vkBindBufferMemory(g_device, tb, churn_pool_mem, 0));
            vkDestroyBuffer(g_device, tb, NULL);
        }
        if ((frames % 10u) == 0u) {
            VkDeviceMemory tmp_m = alloc_mem(churn_req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkFreeMemory(g_device, tmp_m, NULL);
        }

        /* Optional periodic runtime pipeline recompile */
        if (recompile_interval > 0 && frames > warmup && ((frames - warmup) % recompile_interval) == 0) {
            if (deferred_destroy_pipe != VK_NULL_HANDLE) {
                vkDestroyPipeline(g_device, deferred_destroy_pipe, NULL);
                deferred_destroy_pipe = VK_NULL_HANDLE;
            }
            double tc0 = clock_ns();
            VkPipeline np = build_gfx_pipeline(rp, gfx_pl, vs, fs, true, false, VK_COMPARE_OP_LESS_OR_EQUAL);
            runtime_compile_total_ms += (clock_ns() - tc0) / 1e6;
            runtime_compile_count++;
            deferred_destroy_pipe = gfx_pipes[1];
            gfx_pipes[1] = np;
        }

        /* Update per-frame UBO */
        FrameUBO u;
        float t = (float)frames * 0.0166667f;
        u.light_dir[0] = 0.5f * cosf(t * 0.3f); u.light_dir[1] = 0.7f; u.light_dir[2] = 0.5f * sinf(t * 0.3f) + 0.5f; u.light_dir[3] = 0.0f;
        u.light_color[0] = 1.0f; u.light_color[1] = 0.96f; u.light_color[2] = 0.90f; u.light_color[3] = 1.0f;
        u.camera_pos[0] = 0.0f; u.camera_pos[1] = 0.0f; u.camera_pos[2] = -1.5f; u.camera_pos[3] = 1.0f;
        u.params[0] = 0.35f; u.params[1] = t; u.params[2] = 0.0f; u.params[3] = 0.0f;
        memcpy(ubo_map[slot], &u, sizeof(u));

        /* Dynamic texture tile streaming */
        if (enable_stream) {
            fill_tile((uint8_t *)tile_map[slot], TILE_SIZE, TILE_SIZE, (uint32_t)frames + 1u);
        }

        /* Per-frame descriptor pool reset + 48 set allocate & update */
        CHECK(vkResetDescriptorPool(g_device, frame_dp[slot], 0));
        VkDescriptorSet sets[FRAME_SETS];
        VkDescriptorSetAllocateInfo dsa = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = frame_dp[slot],
            .descriptorSetCount = FRAME_SETS,
            .pSetLayouts = set_layouts,
        };
        CHECK(vkAllocateDescriptorSets(g_device, &dsa, sets));
        VkDescriptorBufferInfo ubo_bi = {ubo_buf[slot], 0, sizeof(FrameUBO)};
        VkDescriptorBufferInfo ssbo_bi = {ssbo_buf, 0, ssbo_bytes};
        for (int i = 0; i < FRAME_SETS; ++i) {
            VkDescriptorImageInfo img_ii = {samplers[i & 1], tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet writes[3] = {
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i], .dstBinding = 0,
                 .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ubo_bi},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i], .dstBinding = 1,
                 .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &ssbo_bi},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i], .dstBinding = 2,
                 .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &img_ii},
            };
            vkUpdateDescriptorSets(g_device, 3, writes, 0, NULL);
        }

        /* Record command buffer */
        VkCommandBuffer cb = cbs[slot];
        CHECK(vkResetCommandBuffer(cb, 0));
        CHECK(vkBeginCommandBuffer(cb, &begin_one));
        vkCmdResetQueryPool(cb, qpool, 2 * slot, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 2 * slot);

        if (enable_stream) {
            img_barrier(cb, tex_img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            uint32_t quad = (uint32_t)(frames & 3u);
            VkBufferImageCopy tile_copy = {
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageOffset = {(int32_t)((quad & 1u) * TILE_SIZE), (int32_t)(((quad >> 1u) & 1u) * TILE_SIZE), 0},
                .imageExtent = {TILE_SIZE, TILE_SIZE, 1},
            };
            vkCmdCopyBufferToImage(cb, tile_buf[slot], tex_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tile_copy);
            img_barrier(cb, tex_img, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        /* Compute pass: update 16,384 3D instances */
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pl, 0, 1, &comp_set, 0, NULL);
        CompPush cp = {INSTANCE_COUNT, (uint32_t)frames + 1u, t, 0};
        vkCmdPushConstants(cb, comp_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp), &cp);
        vkCmdDispatch(cb, INSTANCE_COUNT / 64u, 1, 1);

        VkMemoryBarrier comp_to_vert = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 1, &comp_to_vert, 0, NULL, 0, NULL);

        /* Graphics pass: 3 pipelines, draws_per_frame draw calls */
        VkRenderPassBeginInfo rpb = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rp, .framebuffer = fb,
            .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
            .clearValueCount = 2, .pClearValues = clears,
        };
        vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        uint32_t opaque_draws = (draws_per_frame * 68u) / 100u;
        uint32_t fx_draws = draws_per_frame > opaque_draws + 2u ? (draws_per_frame - opaque_draws - 2u) : 1u;

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipes[0]);
        for (uint32_t d = 0; d < opaque_draws; ++d) {
            if ((d % 64u) == 0u) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pl, 0, 1,
                                        &sets[(d / 64u) % FRAME_SETS], 0, NULL);
            }
            DrawPush dp = {(d * 3u) & 16383u, 0u, (uint32_t)frames + 1u, 2u};
            vkCmdPushConstants(cb, gfx_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(dp), &dp);
            vkCmdDraw(cb, 36, 2, 0, 0);
        }

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipes[1]);
        for (uint32_t d = 0; d < fx_draws; ++d) {
            if ((d % 64u) == 0u) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pl, 0, 1,
                                        &sets[((opaque_draws + d) / 64u) % FRAME_SETS], 0, NULL);
            }
            DrawPush dp = {((opaque_draws + d) * 3u) & 16383u, 1u, (uint32_t)frames + 1u, 4u};
            vkCmdPushConstants(cb, gfx_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(dp), &dp);
            vkCmdDraw(cb, 36, 1, 0, 0);
        }

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipes[2]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pl, 0, 1, &sets[0], 0, NULL);
        for (uint32_t d = 0; d < 2u; ++d) {
            DrawPush dp = {0u, 2u, (uint32_t)frames + 1u, 2u};
            vkCmdPushConstants(cb, gfx_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(dp), &dp);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }

        vkCmdEndRenderPass(cb);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 2 * slot + 1);
        CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        submit_ts[slot] = clock_ns();
        CHECK(vkQueueSubmit(queue, 1, &sub, fences[slot]));

        if (frames > warmup && samples < MAX_SAMPLES) {
            cpu_ms[samples] = (clock_ns() - cpu0) / 1e6;
            samples++;
        }
        frames++;
    }

    const double elapsed = (last_ns - start_ns) / 1e9;
    const unsigned long measured = frames - warmup;
    CHECK(vkDeviceWaitIdle(g_device));

    /* Stop CPU worker threads */
#ifdef _WIN32
    g_cpu_stop = 1;
    for (uint32_t t = 0; t < cpu_threads; ++t) {
        WaitForSingleObject(cpu_handles[t], INFINITE);
        CloseHandle(cpu_handles[t]);
    }
#else
    __atomic_store_n(&g_cpu_stop, 1, __ATOMIC_RELAXED);
    for (uint32_t t = 0; t < cpu_threads; ++t) pthread_join(cpu_handles[t], NULL);
#endif

    /* Deterministic verification frame (seed=0) */
    fill_tile((uint8_t *)readback_map, TEX_SIZE, TEX_SIZE, 0);
    FrameUBO uv;
    uv.light_dir[0] = 0.5f; uv.light_dir[1] = 0.7f; uv.light_dir[2] = 0.5f; uv.light_dir[3] = 0.0f;
    uv.light_color[0] = 1.0f; uv.light_color[1] = 0.96f; uv.light_color[2] = 0.90f; uv.light_color[3] = 1.0f;
    uv.camera_pos[0] = 0.0f; uv.camera_pos[1] = 0.0f; uv.camera_pos[2] = -1.5f; uv.camera_pos[3] = 1.0f;
    uv.params[0] = 0.35f; uv.params[1] = 0.0f; uv.params[2] = 0.0f; uv.params[3] = 0.0f;
    memcpy(ubo_map[0], &uv, sizeof(uv));

    CHECK(vkResetDescriptorPool(g_device, frame_dp[0], 0));
    VkDescriptorSet vset = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo vdsa = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = frame_dp[0], .descriptorSetCount = 1, .pSetLayouts = &gfx_dsl,
    };
    CHECK(vkAllocateDescriptorSets(g_device, &vdsa, &vset));
    VkDescriptorBufferInfo vubo = {ubo_buf[0], 0, sizeof(FrameUBO)};
    VkDescriptorBufferInfo vssbo = {ssbo_buf, 0, ssbo_bytes};
    VkDescriptorImageInfo vimg = {samplers[0], tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet vwrites[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vset, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &vubo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vset, .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &vssbo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vset, .dstBinding = 2,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &vimg},
    };
    vkUpdateDescriptorSets(g_device, 3, vwrites, 0, NULL);

    CHECK(vkResetFences(g_device, 1, &fences[0]));
    CHECK(vkResetCommandBuffer(cbs[0], 0));
    CHECK(vkBeginCommandBuffer(cbs[0], &begin_one));
    img_barrier(cbs[0], tex_img, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(cbs[0], readback_buf, tex_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full_copy);
    img_barrier(cbs[0], tex_img, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    vkCmdBindPipeline(cbs[0], VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe);
    vkCmdBindDescriptorSets(cbs[0], VK_PIPELINE_BIND_POINT_COMPUTE, comp_pl, 0, 1, &comp_set, 0, NULL);
    CompPush vcp = {INSTANCE_COUNT, 0u, 0.0f, 0};
    vkCmdPushConstants(cbs[0], comp_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vcp), &vcp);
    vkCmdDispatch(cbs[0], INSTANCE_COUNT / 64u, 1, 1);
    VkMemoryBarrier vmb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 1, &vmb, 0, NULL, 0, NULL);

    VkRenderPassBeginInfo vrpb = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp, .framebuffer = fb,
        .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
        .clearValueCount = 2, .pClearValues = clears,
    };
    vkCmdBeginRenderPass(cbs[0], &vrpb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cbs[0], VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipes[0]);
    vkCmdBindDescriptorSets(cbs[0], VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pl, 0, 1, &vset, 0, NULL);
    for (uint32_t d = 0; d < 256u; ++d) {
        DrawPush dp = {(d * 3u) & 16383u, 0u, 0u, 2u};
        vkCmdPushConstants(cbs[0], gfx_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(dp), &dp);
        vkCmdDraw(cbs[0], 36, 2, 0, 0);
    }
    vkCmdEndRenderPass(cbs[0]);
    VkBufferImageCopy rb_copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {WIDTH, HEIGHT, 1},
    };
    vkCmdCopyImageToBuffer(cbs[0], color_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf, 1, &rb_copy);
    VkMemoryBarrier to_host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &to_host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cbs[0]));
    VkSubmitInfo vsub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbs[0]};
    CHECK(vkQueueSubmit(queue, 1, &vsub, fences[0]));
    CHECK(vkWaitForFences(g_device, 1, &fences[0], VK_TRUE, UINT64_C(10000000000)));
    VkMappedMemoryRange rb_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = readback_mem, .size = VK_WHOLE_SIZE};
    CHECK(vkInvalidateMappedMemoryRanges(g_device, 1, &rb_range));

    uint64_t hash = UINT64_C(1469598103934665603);
    const uint8_t *px = (const uint8_t *)readback_map;
    uint64_t non_bg = 0;
    for (VkDeviceSize i = 0; i < readback_bytes; ++i) {
        hash = (hash ^ px[i]) * UINT64_C(1099511628211);
        if ((i & 3u) == 0u && px[i] > 20u) non_bg++;
    }
    bool validated = (non_bg > 1000u);

    /* Compute frame-time statistics, stutter (>1.25x target), jank (>1.5x target) */
    double f_mean = 0.0, f_var = 0.0, f_max = 0.0;
    for (size_t i = 0; i < samples; ++i) {
        f_mean += frame_ms[i];
        if (frame_ms[i] > f_max) f_max = frame_ms[i];
    }
    if (samples) f_mean /= (double)samples;
    for (size_t i = 0; i < samples; ++i) {
        double d = frame_ms[i] - f_mean;
        f_var += d * d;
    }
    double f_cv = (samples > 1 && f_mean > 0.0) ? (sqrt(f_var / (double)(samples - 1)) / f_mean * 100.0) : 0.0;
    double f50 = percentile(frame_ms, samples, 0.50);
    double f95 = percentile(frame_ms, samples, 0.95);
    double f99 = percentile(frame_ms, samples, 0.99);
    double f999 = percentile(frame_ms, samples, 0.999);
    double target_ms = pace_fps > 0.0 ? (1000.0 / pace_fps) : f50;
    size_t stutters = 0, janks = 0;
    for (size_t i = 0; i < samples; ++i) {
        if (frame_ms[i] > target_ms * 1.25) stutters++;
        if (frame_ms[i] > target_ms * 1.50) janks++;
    }
    double fps = elapsed > 0.0 ? ((double)measured / elapsed) : 0.0;
    double fps_1pct_low = f99 > 0.0 ? (1000.0 / f99) : 0.0;

    printf("{\"validated\":%s,\"device\":\"%s\",\"width\":%d,\"height\":%d,"
           "\"draws\":%u,\"cpu_threads\":%u,\"stream\":%s,\"pace_fps\":%.1f,"
           "\"frames\":%lu,\"seconds\":%.3f,\"fps\":%.2f,\"fps_1pct_low\":%.2f,"
           "\"frame_ms\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"p99_9\":%.3f,\"max\":%.3f,\"mean\":%.3f,\"cv_pct\":%.2f},"
           "\"stutter_count\":%zu,\"stutter_pct\":%.3f,\"jank_count\":%zu,"
           "\"cpu_ms\":{\"p50\":%.3f,\"p95\":%.3f},\"gpu_ms\":{\"p50\":%.3f,\"p95\":%.3f},"
           "\"present_latency_ms\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f},"
           "\"startup_ms\":%.2f,\"shader_compile_ms\":%.2f,"
           "\"runtime_compile_count\":%u,\"runtime_compile_avg_ms\":%.3f,"
           "\"rss_mib\":%.2f,\"checksum\":\"%016" PRIx64 "\",\"non_bg_px\":%" PRIu64 "}\n",
           validated ? "true" : "false", props.deviceName, WIDTH, HEIGHT,
           draws_per_frame, cpu_threads, enable_stream ? "true" : "false", pace_fps,
           measured, elapsed, fps, fps_1pct_low,
           f50, f95, f99, f999, f_max, f_mean, f_cv,
           stutters, samples ? (100.0 * (double)stutters / (double)samples) : 0.0, janks,
           percentile(cpu_ms, samples, 0.50), percentile(cpu_ms, samples, 0.95),
           percentile(gpu_ms, gpu_samples, 0.50), percentile(gpu_ms, gpu_samples, 0.95),
           percentile(present_ms, present_samples, 0.50), percentile(present_ms, present_samples, 0.95),
           percentile(present_ms, present_samples, 0.99),
           startup_ms, shader_compile_ms,
           runtime_compile_count, runtime_compile_count ? (runtime_compile_total_ms / runtime_compile_count) : 0.0,
           process_rss_mib(), hash, non_bg);

    /* Cleanup */
    CHECK(vkDeviceWaitIdle(g_device));
    if (deferred_destroy_pipe != VK_NULL_HANDLE) vkDestroyPipeline(g_device, deferred_destroy_pipe, NULL);
    vkFreeMemory(g_device, churn_pool_mem, NULL);
    vkDestroyQueryPool(g_device, qpool, NULL);
    for (int i = 0; i < IN_FLIGHT; ++i) {
        vkDestroyFence(g_device, fences[i], NULL);
        vkDestroyDescriptorPool(g_device, frame_dp[i], NULL);
        vkUnmapMemory(g_device, ubo_mem[i]);
        vkDestroyBuffer(g_device, ubo_buf[i], NULL);
        vkFreeMemory(g_device, ubo_mem[i], NULL);
        vkUnmapMemory(g_device, tile_mem[i]);
        vkDestroyBuffer(g_device, tile_buf[i], NULL);
        vkFreeMemory(g_device, tile_mem[i], NULL);
    }
    vkDestroyCommandPool(g_device, cmd_pool, NULL);
    vkDestroyDescriptorPool(g_device, comp_dp, NULL);
    vkUnmapMemory(g_device, readback_mem);
    vkDestroyBuffer(g_device, readback_buf, NULL);
    vkFreeMemory(g_device, readback_mem, NULL);
    vkDestroyBuffer(g_device, ssbo_buf, NULL);
    vkFreeMemory(g_device, ssbo_mem, NULL);
    vkDestroyPipeline(g_device, comp_pipe, NULL);
    for (int i = 0; i < 3; ++i) vkDestroyPipeline(g_device, gfx_pipes[i], NULL);
    vkDestroyShaderModule(g_device, cs, NULL);
    vkDestroyShaderModule(g_device, vs, NULL);
    vkDestroyShaderModule(g_device, fs, NULL);
    vkDestroyPipelineLayout(g_device, comp_pl, NULL);
    vkDestroyPipelineLayout(g_device, gfx_pl, NULL);
    vkDestroyDescriptorSetLayout(g_device, comp_dsl, NULL);
    vkDestroyDescriptorSetLayout(g_device, gfx_dsl, NULL);
    vkDestroyFramebuffer(g_device, fb, NULL);
    vkDestroyRenderPass(g_device, rp, NULL);
    for (int i = 0; i < 2; ++i) vkDestroySampler(g_device, samplers[i], NULL);
    vkDestroyImageView(g_device, color_view, NULL);
    vkDestroyImageView(g_device, depth_view, NULL);
    vkDestroyImageView(g_device, tex_view, NULL);
    vkDestroyImage(g_device, color_img, NULL);
    vkDestroyImage(g_device, depth_img, NULL);
    vkDestroyImage(g_device, tex_img, NULL);
    vkFreeMemory(g_device, color_mem, NULL);
    vkFreeMemory(g_device, depth_mem, NULL);
    vkFreeMemory(g_device, tex_mem, NULL);
    vkDestroyDevice(g_device, NULL);
    vkDestroyInstance(instance, NULL);
    free(frame_ms);
    free(cpu_ms);
    free(gpu_ms);
    free(present_ms);
    return validated ? 0 : 2;
}
