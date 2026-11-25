/*
 * vk-render: Windows ネイティブと Android ゲストで同じ描画負荷を流し、処理性能を比べるベンチマーク。
 *
 * オフスクリーン（1920x1080 RGBA8 ＋ D32）に描くので、表示の垂直同期や Android のフレームレート設定に依存しない。
 * 同時に 2 フレームまで GPU に投入し、Fence でフレームの再利用を待つ（キューを無制限には積まない）。
 *
 * シーン:
 *   draws   CPU 中心: 小さな四角形を 1 フレームに 20,000 回の Draw で描く（Push Constant を毎回変える）
 *   tris    頂点中心: 1 Draw で 800 万三角形のメッシュ
 *   fill    フラグメント中心: 全画面を 8 回重ね、1 画素あたり 16 回テクスチャを読む
 *   sync    同期中心: 1 フレームごとに完了を待つ（同時投入 1）。小さな Draw 200 回
 *   game    複合: 4,000 Draw、2 つのパイプラインの切り替え、Descriptor Pool のリセットと 40 回の割り当て・更新、
 *           中規模のメッシュとテクスチャ読み取り
 *
 * 計測: Vulkan API 呼び出しの経過時間、フレーム間隔、GPU タイムスタンプ。
 * --timings では API ごとの経過時間と計測区間全体の描画スレッド CPU 時間も取る。
 * 検算: 計測後に seed=0 の固定フレームを描き、画像全体の FNV-1a 64 を出す（同じ GPU なら一致するはず）。
 *
 * 使い方: vk-render [--scene draws|tris|fill|sync|game] [--seconds N] [--frames N] [--timings] [device-index]
 * 出力: 標準出力に JSON を 1 行。
 */
#include <math.h>
#include <stdbool.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#endif

#include "vulkan_util.h"

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
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield");
#else
            __asm__ volatile("pause");
#endif
        }
    }
}

static const uint32_t vert_code[] =
#include "render_vert.spv.h"
;
static const uint32_t frag_code[] =
#include "render_frag.spv.h"
;

enum { WIDTH = 1920, HEIGHT = 1080, TEX = 1024, IN_FLIGHT = 2, MAX_SAMPLES = 100000 };
enum { DRAWS = 20000, SYNC_DRAWS = 200 };

typedef struct {
    double wait[MAX_SAMPLES];
    double read_queries[MAX_SAMPLES];
    double prepare[MAX_SAMPLES];
    double record[MAX_SAMPLES];
    double submit[MAX_SAMPLES];
    double submit_to_wait_return[MAX_SAMPLES];
} ApiTimings;

static double thread_cpu_ns(void) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        die("GetThreadTimes failed");
    const ULARGE_INTEGER kernel_ticks = {.LowPart = kernel.dwLowDateTime, .HighPart = kernel.dwHighDateTime};
    const ULARGE_INTEGER user_ticks = {.LowPart = user.dwLowDateTime, .HighPart = user.dwHighDateTime};
    return (double)(kernel_ticks.QuadPart + user_ticks.QuadPart) * 100.0;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) die("CLOCK_THREAD_CPUTIME_ID failed");
    return now.tv_sec * 1e9 + now.tv_nsec;
#endif
}

typedef struct {
    float color[4];
    float xform[4];
    uint32_t mode, iterations, grid, seed;
} Push;

#define FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateImage) \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateFence) \
    X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkDeviceWaitIdle)

#define DECLARE(name) static PFN_##name name;
FUNCS(DECLARE)

static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory_properties;

static VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                 .memoryTypeIndex = memory_type(&memory_properties, req.memoryTypeBits, flags)};
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &info, NULL, &memory));
    return memory;
}

static VkImage make_image(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkDeviceMemory *memory) {
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = format, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image;
    CHECK(vkCreateImage(device, &info, NULL, &image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    *memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkBindImageMemory(device, image, *memory, 0));
    return image;
}

static VkImageView make_view(VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                  .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkImageView view;
    CHECK(vkCreateImageView(device, &info, NULL, &view));
    return view;
}

static VkBuffer make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **map) {
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &info, NULL, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    /* 読み戻しはキャッシュ付きを優先する（無ければ coherent だけ） */
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags f = memory_properties.memoryTypes[i].propertyFlags;
        if ((req.memoryTypeBits & (1u << i)) && (f & want) == want && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) type = memory_type(&memory_properties, req.memoryTypeBits, want);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                  .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(device, &alloc, NULL, memory));
    CHECK(vkBindBufferMemory(device, buffer, *memory, 0));
    CHECK(vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, map));
    return buffer;
}

static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src,
                                    .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile(double *values, size_t count, double p) {
    if (!count) return 0;
    qsort(values, count, sizeof(*values), compare_double);
    size_t index = (size_t)(p * (double)(count - 1) + 0.5);
    return values[index];
}

/* ---- 描画の記録 ---- */
static VkPipelineLayout pipeline_layout;
static VkPipeline pipelines[2];
static VkDescriptorSet texture_sets[2];

static void push(VkCommandBuffer cb, const Push *p) {
    vkCmdPushConstants(cb, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(*p), p);
}

static float rnd(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (float)(*state >> 8) / 16777216.0f;
}

static void record_scene(VkCommandBuffer cb, const char *scene, uint32_t seed, VkDescriptorSet *frame_sets,
                         uint32_t frame_set_count) {
    uint32_t state = seed * 2654435761u + 1u;
    Push p = {.color = {1, 1, 1, 1}, .xform = {0, 0, 1, 1}, .mode = 0, .iterations = 1, .grid = 1, .seed = seed};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &texture_sets[0], 0, NULL);
    if (!strcmp(scene, "draws") || !strcmp(scene, "sync")) {
        uint32_t draws = !strcmp(scene, "draws") ? DRAWS : SYNC_DRAWS;
        for (uint32_t i = 0; i < draws; ++i) {
            p.xform[0] = rnd(&state) * 0.95f;
            p.xform[1] = rnd(&state) * 0.95f;
            p.xform[2] = p.xform[3] = 0.02f;
            p.color[0] = rnd(&state);
            push(cb, &p);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }
    } else if (!strcmp(scene, "tris")) {
        p.mode = 1;
        p.grid = 2000; /* 2000 x 2000 セル x 2 = 800 万三角形 */
        push(cb, &p);
        vkCmdDraw(cb, p.grid * p.grid * 6, 1, 0, 0);
    } else if (!strcmp(scene, "fill")) {
        p.iterations = 16;
        for (uint32_t layer = 0; layer < 8; ++layer) {
            p.color[1] = 0.5f + 0.06f * (float)layer;
            push(cb, &p);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }
    } else { /* game */
        for (uint32_t i = 0; i < 4000; ++i) {
            if (i % 100 == 0) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[(i / 100) & 1]);
                VkDescriptorSet set = frame_sets[(i / 100) % frame_set_count];
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0, NULL);
            }
            if (i % 500 == 0) {
                p.mode = 1;
                p.grid = 200;
                p.iterations = 4;
                p.xform[0] = rnd(&state) * 0.6f;
                p.xform[1] = rnd(&state) * 0.6f;
                p.xform[2] = p.xform[3] = 0.4f;
                push(cb, &p);
                vkCmdDraw(cb, p.grid * p.grid * 6, 1, 0, 0);
                p.mode = 0;
                p.iterations = 2;
            }
            p.xform[0] = rnd(&state) * 0.9f;
            p.xform[1] = rnd(&state) * 0.9f;
            p.xform[2] = p.xform[3] = 0.05f;
            p.color[2] = rnd(&state);
            push(cb, &p);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }
    }
}

int main(int argc, char **argv) {
    const char *scene = "game";
    double seconds = 10;
    double pace_fps = 0;
    bool detailed_timings = false;
    unsigned long max_frames = 0, index = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--pace-fps") && i + 1 < argc) pace_fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--timings")) detailed_timings = true;
        else {
            char *end = NULL;
            index = strtoul(argv[i], &end, 10);
            if (end == argv[i] || *end)
                die("usage: vk-render [--scene draws|tris|fill|sync|game] [--seconds N] [--frames N] [--pace-fps N] [--timings] [device-index]");
        }
    }
    if (pace_fps < 0 || pace_fps > 1000) die("pace-fps must be in [0, 1000]");
    if (strcmp(scene, "draws") && strcmp(scene, "tris") && strcmp(scene, "fill") && strcmp(scene, "sync") &&
        strcmp(scene, "game"))
        die("unknown scene");
    if (seconds <= 0 || seconds > 600) die("seconds must be in (0, 600]");
    const uint32_t in_flight = !strcmp(scene, "sync") ? 1 : IN_FLIGHT;

    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)lookup(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Velox render",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
#define LOAD_GLOBAL(name) name = (PFN_##name)lookup(instance, #name); if (!name) die("Missing entry point " #name);
    FUNCS(LOAD_GLOBAL)

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    VkPhysicalDevice physicals[8];
    if (count > 8) count = 8;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, physicals));
    if (index >= count) die("device index out of range");
    VkPhysicalDevice physical = physicals[index];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    VkQueueFamilyProperties families[16];
    if (family_count > 16) family_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && families[i].timestampValidBits) {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX) die("no graphics queue with timestamps");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue_info};
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    /* 描画先 */
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM, depth_format = VK_FORMAT_D32_SFLOAT;
    VkDeviceMemory color_memory, depth_memory, tex_memory;
    VkImage color = make_image(color_format, WIDTH, HEIGHT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &color_memory);
    VkImage depth = make_image(depth_format, WIDTH, HEIGHT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &depth_memory);
    VkImage texture = make_image(color_format, TEX, TEX, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 &tex_memory);
    VkImageView color_view = make_view(color, color_format, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView depth_view = make_view(depth, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView tex_view = make_view(texture, color_format, VK_IMAGE_ASPECT_COLOR_BIT);

    VkAttachmentDescription attachments[2] = {
        {0, color_format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {0, depth_format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
    };
    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                    .pColorAttachments = &color_ref, .pDepthStencilAttachment = &depth_ref};
    VkSubpassDependency dependencies[2] = {{VK_SUBPASS_EXTERNAL, 0,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                      VK_ACCESS_TRANSFER_READ_BIT,
                                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
        /* 検算フレームの読み戻し（転送）が描画の書き込みを見るため */
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0}};
    VkRenderPassCreateInfo rp_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                      .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass,
                                      .dependencyCount = 2, .pDependencies = dependencies};
    VkRenderPass render_pass;
    CHECK(vkCreateRenderPass(device, &rp_info, NULL, &render_pass));
    VkImageView fb_views[2] = {color_view, depth_view};
    VkFramebufferCreateInfo fb_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = render_pass,
                                       .attachmentCount = 2, .pAttachments = fb_views, .width = WIDTH,
                                       .height = HEIGHT, .layers = 1};
    VkFramebuffer framebuffer;
    CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer));

    /* パイプライン（不透明と加算合成の 2 種類） */
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                            VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dsl_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &dsl_info, NULL, &set_layout));
    VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                          .pSetLayouts = &set_layout, .pushConstantRangeCount = 1,
                                          .pPushConstantRanges = &range};
    CHECK(vkCreatePipelineLayout(device, &pl_info, NULL, &pipeline_layout));
    VkShaderModuleCreateInfo vs_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = sizeof(vert_code), .pCode = vert_code};
    VkShaderModuleCreateInfo fs_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = sizeof(frag_code), .pCode = frag_code};
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(device, &vs_info, NULL, &vs));
    CHECK(vkCreateShaderModule(device, &fs_info, NULL, &fs));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {.sType =
                                                              VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, WIDTH, HEIGHT, 0, 1};
    VkRect2D scissor = {{0, 0}, {WIDTH, HEIGHT}};
    VkPipelineViewportStateCreateInfo viewport_state = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                        .viewportCount = 1, .pViewports = &viewport,
                                                        .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
    VkPipelineColorBlendAttachmentState blends[2] = {
        {.blendEnable = VK_FALSE, .colorWriteMask = 0xf},
        {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
         .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
         .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf},
    };
    for (int i = 0; i < 2; ++i) {
        VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                     .attachmentCount = 1, .pAttachments = &blends[i]};
        VkGraphicsPipelineCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
            .pRasterizationState = &raster, .pMultisampleState = &multisample, .pDepthStencilState = &depth_state,
            .pColorBlendState = &blend, .layout = pipeline_layout, .renderPass = render_pass};
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipelines[i]));
    }

    /* テクスチャ（2 枚分を同じ画像の別サンプラーで表す）と Descriptor */
    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = 0};
    VkSampler samplers[2];
    CHECK(vkCreateSampler(device, &sampler_info, NULL, &samplers[0]));
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
    CHECK(vkCreateSampler(device, &sampler_info, NULL, &samplers[1]));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2,
                                            .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool static_pool;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &static_pool));
    VkDescriptorSetLayout layouts[64];
    for (int i = 0; i < 64; ++i) layouts[i] = set_layout;
    VkDescriptorSetAllocateInfo set_alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = static_pool, .descriptorSetCount = 2,
                                             .pSetLayouts = layouts};
    CHECK(vkAllocateDescriptorSets(device, &set_alloc, texture_sets));
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo image_info = {samplers[i], tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = texture_sets[i],
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &image_info};
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    }
    /* game シーン用: フレームごとにリセットするプール */
    enum { FRAME_SETS = 40 };
    VkDescriptorPoolSize frame_pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAME_SETS};
    VkDescriptorPoolCreateInfo frame_pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                  .maxSets = FRAME_SETS, .poolSizeCount = 1,
                                                  .pPoolSizes = &frame_pool_size};
    VkDescriptorPool frame_pools[IN_FLIGHT];
    for (int i = 0; i < IN_FLIGHT; ++i) CHECK(vkCreateDescriptorPool(device, &frame_pool_info, NULL, &frame_pools[i]));

    /* コマンドと同期 */
    VkCommandPoolCreateInfo cp_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                       .queueFamilyIndex = family};
    VkCommandPool command_pool;
    CHECK(vkCreateCommandPool(device, &cp_info, NULL, &command_pool));
    VkCommandBuffer cbs[IN_FLIGHT];
    VkCommandBufferAllocateInfo cb_alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = IN_FLIGHT};
    CHECK(vkAllocateCommandBuffers(device, &cb_alloc, cbs));
    VkFence fences[IN_FLIGHT];
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                    .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < IN_FLIGHT; ++i) CHECK(vkCreateFence(device, &fence_info, NULL, &fences[i]));
    VkQueryPoolCreateInfo qp_info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * IN_FLIGHT};
    VkQueryPool queries;
    CHECK(vkCreateQueryPool(device, &qp_info, NULL, &queries));

    /* テクスチャの内容（決定的な模様）を転送する */
    VkDeviceMemory staging_memory;
    void *staging_map;
    const VkDeviceSize image_bytes = (VkDeviceSize)WIDTH * HEIGHT * 4;
    VkBuffer staging = make_host_buffer(image_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        &staging_memory, &staging_map);
    uint32_t *texels = staging_map;
    for (uint32_t y = 0; y < TEX; ++y)
        for (uint32_t x = 0; x < TEX; ++x)
            texels[y * TEX + x] = 0xff000000u | ((x ^ y) & 0xff) | (((x * 3) & 0xff) << 8) | (((y * 5) & 0xff) << 16);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cbs[0], &begin));
    image_barrier(cbs[0], texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy upload = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {TEX, TEX, 1}};
    vkCmdCopyBufferToImage(cbs[0], staging, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
    image_barrier(cbs[0], texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    CHECK(vkEndCommandBuffer(cbs[0]));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbs[0]};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    /* フレームの記録（seed でシーンの乱数を変える）。verify は固定フレーム＋画像の読み戻し */
    VkClearValue clears[2] = {{.color = {{0.1f, 0.1f, 0.1f, 1}}}, {.depthStencil = {1, 0}}};
#define RECORD_FRAME(slot, seed, verify) do { \
        VkCommandBuffer cb = cbs[slot]; \
        CHECK(vkResetCommandBuffer(cb, 0)); \
        CHECK(vkBeginCommandBuffer(cb, &begin)); \
        vkCmdResetQueryPool(cb, queries, 2 * (slot), 2); \
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 2 * (slot)); \
        VkRenderPassBeginInfo rp_begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, \
                                          .renderPass = render_pass, .framebuffer = framebuffer, \
                                          .renderArea = {{0, 0}, {WIDTH, HEIGHT}}, .clearValueCount = 2, \
                                          .pClearValues = clears}; \
        vkCmdBeginRenderPass(cb, &rp_begin, VK_SUBPASS_CONTENTS_INLINE); \
        record_scene(cb, scene, seed, frame_sets, FRAME_SETS); \
        vkCmdEndRenderPass(cb); \
        if (verify) { \
            VkBufferImageCopy readback = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, \
                                          .imageExtent = {WIDTH, HEIGHT, 1}}; \
            vkCmdCopyImageToBuffer(cb, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &readback); \
        } \
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 2 * (slot) + 1); \
        CHECK(vkEndCommandBuffer(cb)); \
    } while (0)

    double *cpu_ms = calloc(MAX_SAMPLES, sizeof(double)), *frame_ms = calloc(MAX_SAMPLES, sizeof(double)),
           *gpu_ms = calloc(MAX_SAMPLES, sizeof(double));
    ApiTimings *timings = detailed_timings ? calloc(1, sizeof(*timings)) : NULL;
    if (!cpu_ms || !frame_ms || !gpu_ms || (detailed_timings && !timings)) die("out of memory");
    size_t samples = 0, gpu_samples = 0;
    double submit_begin[IN_FLIGHT] = {0};
    size_t completion_samples = 0;
    const double period_ns = props.limits.timestampPeriod;
    unsigned long frames = 0;
    const unsigned long warmup = 20;
    double start = 0, last = clock_ns(), thread_start = 0;
    const double pace_period_ns = pace_fps > 0.0 ? (1e9 / pace_fps) : 0.0;
    double next_pace_ns = 0.0;
    for (;;) {
        if (pace_period_ns > 0.0 && frames > warmup) {
            if (next_pace_ns == 0.0) next_pace_ns = clock_ns() + pace_period_ns;
            pace_until_ns(next_pace_ns);
            next_pace_ns += pace_period_ns;
            double cur = clock_ns();
            if (next_pace_ns < cur) next_pace_ns = cur;
        }
        const uint32_t slot = frames % in_flight;
        const bool sample_timing = timings && frames > warmup && samples < MAX_SAMPLES;
        const double wait_start = sample_timing ? clock_ns() : 0;
        CHECK(vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_C(10000000000)));
        const double wait_end = sample_timing ? clock_ns() : 0;
        if (frames > warmup + in_flight) {
            if (sample_timing) {
                timings->submit_to_wait_return[completion_samples++] = (wait_end - submit_begin[slot]) / 1e6;
            }
            uint64_t stamps[2];
            if (vkGetQueryPoolResults(device, queries, 2 * slot, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && gpu_samples < MAX_SAMPLES)
                gpu_ms[gpu_samples++] = (double)(stamps[1] - stamps[0]) * period_ns / 1e6;
        }
        const double now = clock_ns();
        if (frames == warmup) {
            start = now;
            if (timings) thread_start = thread_cpu_ns();
        }
        if (frames > warmup && samples < MAX_SAMPLES) frame_ms[samples] = (now - last) / 1e6;
        last = now;
        if (frames >= warmup && ((max_frames && frames - warmup >= max_frames) ||
                                 (!max_frames && now - start >= seconds * 1e9)))
            break;
        const double cpu_start = clock_ns();
        CHECK(vkResetFences(device, 1, &fences[slot]));
        VkDescriptorSet frame_sets[FRAME_SETS];
        if (!strcmp(scene, "game")) {
            /* 一般的なエンジンと同じく、フレームのプールをリセットして割り当て直し、更新する */
            CHECK(vkResetDescriptorPool(device, frame_pools[slot], 0));
            VkDescriptorSetAllocateInfo frame_alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                       .descriptorPool = frame_pools[slot],
                                                       .descriptorSetCount = FRAME_SETS, .pSetLayouts = layouts};
            CHECK(vkAllocateDescriptorSets(device, &frame_alloc, frame_sets));
            for (uint32_t i = 0; i < FRAME_SETS; ++i) {
                VkDescriptorImageInfo image_info = {samplers[i & 1], tex_view,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = frame_sets[i], .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              .pImageInfo = &image_info};
                vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
            }
        } else {
            for (uint32_t i = 0; i < FRAME_SETS; ++i) frame_sets[i] = texture_sets[i & 1];
        }
        const double record_start = sample_timing ? clock_ns() : 0;
        RECORD_FRAME(slot, (uint32_t)frames + 1, 0);
        VkSubmitInfo frame_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                     .pCommandBuffers = &cbs[slot]};
        if (sample_timing) submit_begin[slot] = clock_ns();
        CHECK(vkQueueSubmit(queue, 1, &frame_submit, fences[slot]));
        if (frames > warmup && samples < MAX_SAMPLES) {
            const double submit_end = clock_ns();
            cpu_ms[samples] = (submit_end - cpu_start) / 1e6;
            if (sample_timing) {
                timings->wait[samples] = (wait_end - wait_start) / 1e6;
                timings->read_queries[samples] = (now - wait_end) / 1e6;
                timings->prepare[samples] = (record_start - cpu_start) / 1e6;
                timings->record[samples] = (submit_begin[slot] - record_start) / 1e6;
                timings->submit[samples] = (submit_end - submit_begin[slot]) / 1e6;
            }
            ++samples;
        }
        ++frames;
    }
    const double elapsed = (last - start) / 1e9;
    const unsigned long measured = frames - warmup;
    const double thread_cpu = timings ? thread_cpu_ns() - thread_start : 0;
    CHECK(vkDeviceWaitIdle(device));

    /* 検算用の固定フレーム（seed=0）。game シーンは静的な Set を使う */
    {
        VkDescriptorSet frame_sets[FRAME_SETS];
        for (uint32_t i = 0; i < FRAME_SETS; ++i) frame_sets[i] = texture_sets[i & 1];
        CHECK(vkResetFences(device, 1, &fences[0]));
        RECORD_FRAME(0, 0u, 1);
        VkSubmitInfo verify_submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                      .pCommandBuffers = &cbs[0]};
        CHECK(vkQueueSubmit(queue, 1, &verify_submit, fences[0]));
        CHECK(vkWaitForFences(device, 1, &fences[0], VK_TRUE, UINT64_C(10000000000)));
    }
    uint64_t hash = 1469598103934665603ull;
    const uint8_t *pixels = staging_map;
    for (VkDeviceSize i = 0; i < image_bytes; ++i) hash = (hash ^ pixels[i]) * 1099511628211ull;

    const size_t frame_count = samples < MAX_SAMPLES ? samples : MAX_SAMPLES;
    double f_mean = 0.0, f_var = 0.0;
    for (size_t i = 0; i < frame_count; ++i) f_mean += frame_ms[i];
    if (frame_count) f_mean /= (double)frame_count;
    for (size_t i = 0; i < frame_count; ++i) {
        double d = frame_ms[i] - f_mean;
        f_var += d * d;
    }
    const double f_cv = (frame_count > 1 && f_mean > 0.0) ? (sqrt(f_var / (double)(frame_count - 1)) / f_mean * 100.0) : 0.0;
    const double cpu50 = percentile(cpu_ms, frame_count, 0.5), cpu95 = percentile(cpu_ms, frame_count, 0.95);
    const double gpu50 = percentile(gpu_ms, gpu_samples, 0.5), gpu95 = percentile(gpu_ms, gpu_samples, 0.95);
    const double f50 = percentile(frame_ms, frame_count, 0.5), f95 = percentile(frame_ms, frame_count, 0.95),
                 f99 = percentile(frame_ms, frame_count, 0.99);
    const double fps_1pct_low = f99 > 0.0 ? (1000.0 / f99) : 0.0;
    printf("{\"scene\":\"%s\",\"device\":\"%s\",\"driver_version\":%u,\"width\":%d,\"height\":%d,\"in_flight\":%u,"
           "\"pace_fps\":%.1f,\"frames\":%lu,\"seconds\":%.3f,\"fps\":%.2f,\"fps_1pct_low\":%.2f,"
           "\"cpu_ms\":{\"p50\":%.3f,\"p95\":%.3f},\"gpu_ms\":{\"p50\":%.3f,\"p95\":%.3f},"
           "\"frame_ms\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"mean\":%.3f,\"cv_pct\":%.2f},"
           "\"checksum\":\"%016" PRIx64 "\"",
           scene, props.deviceName, props.driverVersion, WIDTH, HEIGHT, in_flight, pace_fps, measured, elapsed,
           elapsed > 0 ? (double)measured / elapsed : 0, fps_1pct_low, cpu50, cpu95, gpu50, gpu95,
           f50, f95, f99, f_mean, f_cv, hash);
    if (timings) {
        const uint32_t draws = !strcmp(scene, "draws") ? DRAWS : !strcmp(scene, "sync") ? SYNC_DRAWS : 0;
        printf(",\"timings\":{\"samples\":%zu,\"completion_samples\":%zu,\"thread_cpu_ns_per_frame\":%.1f",
               samples, completion_samples, thread_cpu / measured);
        if (draws) {
            printf(",\"draws_per_frame\":%u,\"thread_cpu_ns_per_draw\":%.2f", draws,
                   thread_cpu / measured / draws);
        }
#define PRINT_TIMING(field, count) do { \
            const double p50 = percentile(timings->field, count, 0.50); \
            const double p95 = percentile(timings->field, count, 0.95); \
            printf(",\"%s_ms\":{\"p50\":%.6f,\"p95\":%.6f}", #field, p50, p95); \
        } while (0)
        PRINT_TIMING(wait, samples);
        PRINT_TIMING(read_queries, samples);
        PRINT_TIMING(prepare, samples);
        PRINT_TIMING(record, samples);
        PRINT_TIMING(submit, samples);
        PRINT_TIMING(submit_to_wait_return, completion_samples);
#undef PRINT_TIMING
        printf("}");
    }
    printf("}\n");

    CHECK(vkDeviceWaitIdle(device));
    vkDestroyQueryPool(device, queries, NULL);
    for (int i = 0; i < IN_FLIGHT; ++i) {
        vkDestroyFence(device, fences[i], NULL);
        vkDestroyDescriptorPool(device, frame_pools[i], NULL);
    }
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyDescriptorPool(device, static_pool, NULL);
    for (int i = 0; i < 2; ++i) {
        vkDestroySampler(device, samplers[i], NULL);
        vkDestroyPipeline(device, pipelines[i], NULL);
    }
    vkDestroyShaderModule(device, vs, NULL);
    vkDestroyShaderModule(device, fs, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyRenderPass(device, render_pass, NULL);
    vkDestroyImageView(device, color_view, NULL);
    vkDestroyImageView(device, depth_view, NULL);
    vkDestroyImageView(device, tex_view, NULL);
    vkDestroyImage(device, color, NULL);
    vkDestroyImage(device, depth, NULL);
    vkDestroyImage(device, texture, NULL);
    vkUnmapMemory(device, staging_memory);
    vkDestroyBuffer(device, staging, NULL);
    vkFreeMemory(device, staging_memory, NULL);
    vkFreeMemory(device, color_memory, NULL);
    vkFreeMemory(device, depth_memory, NULL);
    vkFreeMemory(device, tex_memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(cpu_ms);
    free(frame_ms);
    free(gpu_ms);
    free(timings);
    return 0;
}
