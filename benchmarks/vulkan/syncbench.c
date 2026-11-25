/* Vulkan の同期 API と Descriptor の API のマイクロベンチ（Windows と Android で同一ソース）。
 *
 * gfxstream では、結果を返す Vulkan API の多くがホストとの同期的な往復になる。AnTuTu V11 の
 * GPU シーンで律速していた vkResetDescriptorPool と vkGetFenceStatus を中心に、1 回あたりの
 * 時間、連続実行、スレッド数による変化、GPU 処理を伴う場合を測る。
 *
 * 正しさの確認:
 *   - Descriptor Set の内容を GPU のシェーダーが読み、結果を CPU で全件照合する（プールを毎回
 *     リセットし、同時に複数のプールを使う場合も含む）
 *   - プールの上限、リセット後の再割り当て、FREE フラグ付きプールの解放と再割り当て
 *   - Fence は GPU 処理の完了前に VK_SUCCESS を返さないこと（完了後に読む値で確認する）
 * 照合に失敗すると終了コード 2 で終わり、結果を出力しない。
 *
 * 出力: 標準出力に 1 つの JSON。各試験は samples（1 サンプル = batch 回の平均、ns/回）を持つ。
 * 統計（中央値、P95、P99、CV など）は scripts/run_vk_syncbench.py が計算する。
 *
 * 使い方: vk-syncbench [--quick] [--mt-stress rounds | --transport-stress rounds] [device-index]
 *   Android で VELOX_VK_HAL=/path/vulkan.xxx.so を指定すると、その HAL を直接読み込む。
 */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <string.h>
#include <stdlib.h>
#include "vulkan_util.h"
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#endif

static const uint32_t ubo_shader[] =
#include "syncbench_ubo.spv.h"
;
static const uint32_t busy_shader[] =
#include "syncbench_busy.spv.h"
;

enum {
    POOL_SETS = 64,
    VERIFY_ITERATIONS = 256,
    RING = 4,
    MAX_SAMPLES = 256,
    MAX_THREADS = 8,
    SLOT_BYTES = 256,
    BUSY_ELEMENTS = 64 * 1024,
};

#define INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkGetSemaphoreCounterValue) X(vkSignalSemaphore) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkGetFenceStatus) X(vkWaitForFences) \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdPipelineBarrier) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkGetQueryPoolResults) X(vkCmdUpdateBuffer)

#define DECLARE(name) static PFN_##name name;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)

static VkDevice device;
static VkQueue queue, second_queue;
static uint32_t family;
static VkPhysicalDeviceMemoryProperties memory_properties;
static VkDescriptorSetLayout set_layout;
static VkBuffer ubo_buffer, out_buffer;
static uint32_t *ubo_words, *out_words;
static int quick;
static unsigned mt_stress_rounds;
static unsigned transport_stress_rounds;
static int skip_mt_desc;
static int mt_fence_sweep;
static int mt_fence_signaled;
static int mt_fence_nobarrier;
static int mt_fence_nowarmup;
static unsigned mt_fence_calls = 500;
static int wakeup_sweep;
static int first_test = 1;

/* ---- 出力 ---- */
static void begin_test(const char *name, const char *unit, unsigned batch) {
    printf("%s\"%s\":{\"unit\":\"%s\",\"batch\":%u,\"samples\":[", first_test ? "" : ",", name, unit, batch);
    first_test = 0;
}
static void sample(unsigned i, double value) { printf("%s%.1f", i ? "," : "", value); }
static void end_test(void) {
    printf("]}");
    fflush(stdout);
}

static void emit_loop(const char *name, unsigned samples, unsigned batch, void (*op)(void *), void *ctx) {
    begin_test(name, "ns", batch);
    for (unsigned s = 0; s < samples; ++s) {
        double t0 = clock_ns();
        for (unsigned b = 0; b < batch; ++b) op(ctx);
        sample(s, (clock_ns() - t0) / batch);
    }
    end_test();
}

/* ---- バッファ ---- */
static VkDeviceMemory make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, void **mapped) {
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CHECK(vkCreateBuffer(device, &info, NULL, buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, *buffer, &req);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(&memory_properties, req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &alloc, NULL, &memory));
    CHECK(vkBindBufferMemory(device, *buffer, memory, 0));
    CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped));
    return memory;
}

static VkDescriptorPool make_pool(uint32_t max_sets, VkDescriptorPoolCreateFlags flags) {
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, max_sets},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, max_sets},
    };
    VkDescriptorPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = flags, .maxSets = max_sets,
        .poolSizeCount = 2, .pPoolSizes = sizes,
    };
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(device, &info, NULL, &pool));
    return pool;
}

static VkResult alloc_sets(VkDescriptorPool pool, uint32_t count, VkDescriptorSet *sets) {
    VkDescriptorSetLayout layouts[POOL_SETS];
    for (uint32_t i = 0; i < count; ++i) layouts[i] = set_layout;
    VkDescriptorSetAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
        .descriptorSetCount = count, .pSetLayouts = layouts,
    };
    return vkAllocateDescriptorSets(device, &info, sets);
}

/* set に UBO（slot 番目の 256 バイト）と出力 SSBO を書く */
static void write_set(VkDescriptorSet set, uint32_t slot) {
    VkDescriptorBufferInfo ubo = {ubo_buffer, (VkDeviceSize)slot * SLOT_BYTES, 16};
    VkDescriptorBufferInfo out = {out_buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ubo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &out},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
}

static uint32_t expected_word(uint32_t value) { return value * UINT32_C(2654435761) + UINT32_C(0x9e3779b9); }

/* ---- 単純な往復 ---- */
static VkSemaphore timeline;
static VkFence fence_unsignaled, fence_signaled;
static VkDescriptorPool loop_pool, empty_pool;
static VkDescriptorSet loop_set;

static void op_semaphore(void *ctx) {
    (void)ctx;
    uint64_t value = 0;
    CHECK(vkGetSemaphoreCounterValue(device, timeline, &value));
}

static void test_semaphore_rpc(void) {
    enum { WARMUP = 64, SAMPLES = 1024 };
    double times[SAMPLES];
    uint64_t expected = 0, value = 0;
    CHECK(vkGetSemaphoreCounterValue(device, timeline, &expected));
    for (unsigned i = 0; i < WARMUP; ++i) {
        CHECK(vkGetSemaphoreCounterValue(device, timeline, &value));
        if (value != expected) die("timeline counter changed during warmup");
    }
    for (unsigned i = 0; i < SAMPLES; ++i) {
        const double start = clock_ns();
        CHECK(vkGetSemaphoreCounterValue(device, timeline, &value));
        times[i] = clock_ns() - start;
        if (value != expected) die("timeline counter changed during measurement");
    }
    begin_test("semaphore_counter_rpc", "ns", 1);
    for (unsigned i = 0; i < SAMPLES; ++i) sample(i, times[i]);
    end_test();
}
static void op_fence_unsignaled(void *ctx) {
    (void)ctx;
    if (vkGetFenceStatus(device, fence_unsignaled) != VK_NOT_READY) die("unsignaled fence reported ready");
}
static void op_fence_signaled(void *ctx) {
    (void)ctx;
    if (vkGetFenceStatus(device, fence_signaled) != VK_SUCCESS) die("signaled fence not ready");
}
static void op_fence_reset(void *ctx) {
    (void)ctx;
    CHECK(vkResetFences(device, 1, &fence_unsignaled));
}
static void op_reset_empty(void *ctx) {
    (void)ctx;
    /* Set を割り当てていないプール（loop_pool の Set を無効にしないため別のプール） */
    CHECK(vkResetDescriptorPool(device, empty_pool, 0));
}
static void op_update(void *ctx) {
    (void)ctx;
    write_set(loop_set, 0);
}
static void op_pool_create_destroy(void *ctx) {
    (void)ctx;
    VkDescriptorPool pool = make_pool(POOL_SETS, 0);
    vkDestroyDescriptorPool(device, pool, NULL);
}

static void submit_empty_and_wait(VkFence fence) {
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
}
static VkFence submit_fence;
static void op_submit_wait(void *ctx) {
    (void)ctx;
    submit_empty_and_wait(submit_fence);
    CHECK(vkResetFences(device, 1, &submit_fence));
}

/* ---- 1 回ずつ計る試験（前後の準備を計測から外す） ---- */
static double timer_overhead_ns(void) {
    double best = 1e30;
    for (int i = 0; i < 200; ++i) {
        double t0 = clock_ns();
        double t1 = clock_ns();
        if (t1 - t0 < best) best = t1 - t0;
    }
    return best;
}

static void test_alloc_and_reset(unsigned samples) {
    VkDescriptorPool pool = make_pool(POOL_SETS, 0);
    VkDescriptorSet sets[POOL_SETS];
    /* 1 個ずつ 16 回割り当てる時間（1 回あたり） */
    begin_test("dset_alloc1", "ns", 16);
    for (unsigned s = 0; s < samples; ++s) {
        double t0 = clock_ns();
        for (uint32_t i = 0; i < 16; ++i) CHECK(alloc_sets(pool, 1, &sets[i]));
        sample(s, (clock_ns() - t0) / 16);
        CHECK(vkResetDescriptorPool(device, pool, 0));
    }
    end_test();
    /* 16 個を 1 回で割り当てる時間 */
    begin_test("dset_alloc16_call", "ns", 1);
    for (unsigned s = 0; s < samples; ++s) {
        double t0 = clock_ns();
        CHECK(alloc_sets(pool, 16, sets));
        sample(s, clock_ns() - t0);
        CHECK(vkResetDescriptorPool(device, pool, 0));
    }
    end_test();
    /* 16 個割り当て・更新した後のリセット 1 回 */
    begin_test("dpool_reset_after16", "ns", 1);
    for (unsigned s = 0; s < samples; ++s) {
        CHECK(alloc_sets(pool, 16, sets));
        for (uint32_t i = 0; i < 16; ++i) write_set(sets[i], i);
        double t0 = clock_ns();
        CHECK(vkResetDescriptorPool(device, pool, 0));
        sample(s, clock_ns() - t0);
    }
    end_test();
    vkDestroyDescriptorPool(device, pool, NULL);
}

/* Unreal の RHIThread に近いフレーム単位の使い方: リセット → 1 個ずつ 32 個割り当て → 各 2 書き込み */
static void frame_pattern(VkDescriptorPool pool, VkDescriptorSet *sets, uint32_t count) {
    CHECK(vkResetDescriptorPool(device, pool, 0));
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(alloc_sets(pool, 1, &sets[i]));
        write_set(sets[i], i % POOL_SETS);
    }
}

typedef struct {
    unsigned frames;
    double elapsed_ns;
} ThreadJob;

#ifdef _WIN32
static unsigned __stdcall frame_thread(void *p)
#else
static void *frame_thread(void *p)
#endif
{
    ThreadJob *job = p;
    VkDescriptorPool pool = make_pool(POOL_SETS, 0);
    VkDescriptorSet sets[32];
    frame_pattern(pool, sets, 32);
    double t0 = clock_ns();
    for (unsigned f = 0; f < job->frames; ++f) frame_pattern(pool, sets, 32);
    job->elapsed_ns = clock_ns() - t0;
    vkDestroyDescriptorPool(device, pool, NULL);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

typedef struct {
    unsigned calls;
    double elapsed_ns;
    VkFence fence;
} FenceJob;

#ifdef _WIN32
static unsigned __stdcall fence_thread(void *p)
#else
static void *fence_thread(void *p)
#endif
{
    FenceJob *job = p;
    double t0 = clock_ns();
    for (unsigned i = 0; i < job->calls; ++i)
        if (vkGetFenceStatus(device, job->fence) != VK_NOT_READY) die("unsignaled fence reported ready (mt)");
    job->elapsed_ns = clock_ns() - t0;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
typedef unsigned (__stdcall *ThreadEntry)(void *);
#else
typedef void *(*ThreadEntry)(void *);
#endif

static void run_threads(unsigned count, void *jobs, size_t job_size, ThreadEntry work) {
#ifdef _WIN32
    HANDLE handles[MAX_THREADS];
    for (unsigned t = 0; t < count; ++t) {
        handles[t] = (HANDLE)_beginthreadex(NULL, 0, work, (char *)jobs + t * job_size, 0, NULL);
        if (!handles[t]) die("_beginthreadex failed");
    }
    for (unsigned t = 0; t < count; ++t) {
        WaitForSingleObject(handles[t], INFINITE);
        CloseHandle(handles[t]);
    }
#else
    pthread_t threads[MAX_THREADS];
    for (unsigned t = 0; t < count; ++t)
        if (pthread_create(&threads[t], NULL, work, (char *)jobs + t * job_size))
            die("pthread_create failed");
    for (unsigned t = 0; t < count; ++t) pthread_join(threads[t], NULL);
#endif
}

/* vkCmdUpdateBuffer はコマンドにデータを含む。小さな更新と 256 KiB の更新を交互に送る。 */
enum { TRANSPORT_THREADS = 4, TRANSPORT_CHUNK_WORDS = 16384, TRANSPORT_CHUNKS = 4 };

#ifdef _WIN32
static SRWLOCK transport_queue_locks[2] = {SRWLOCK_INIT, SRWLOCK_INIT};
static unsigned __stdcall transport_thread(void *p)
#else
static pthread_mutex_t transport_queue_locks[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
static void *transport_thread(void *p)
#endif
{
    unsigned index = *(unsigned *)p;
    unsigned queue_index = second_queue ? index % 2 : 0;
    VkQueue worker_queue = queue_index ? second_queue : queue;
    VkBuffer target;
    uint32_t *output;
    VkDeviceMemory memory = make_buffer(TRANSPORT_CHUNKS * TRANSPORT_CHUNK_WORDS * sizeof(*output),
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, &target, (void **)&output);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         .queueFamilyIndex = family};
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &allocation, &cb));
    VkFence fence;
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence));

    uint32_t payload[TRANSPORT_CHUNK_WORDS];
    for (unsigned round = 0; round < transport_stress_rounds; ++round) {
        unsigned chunks = round % 2 ? TRANSPORT_CHUNKS : 1;
        unsigned words = round % 2 ? TRANSPORT_CHUNK_WORDS : 16;
        uint32_t seed = expected_word(index + round * TRANSPORT_THREADS);
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cb, &begin));
        for (unsigned chunk = 0; chunk < chunks; ++chunk) {
            unsigned start = chunk * words;
            for (unsigned i = 0; i < words; ++i) payload[i] = expected_word(seed + start + i);
            vkCmdUpdateBuffer(cb, target, start * sizeof(*payload), words * sizeof(*payload), payload);
        }
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             1, &barrier, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                               .pCommandBuffers = &cb};
#ifdef _WIN32
        AcquireSRWLockExclusive(&transport_queue_locks[queue_index]);
        CHECK(vkQueueSubmit(worker_queue, 1, &submit, fence));
        ReleaseSRWLockExclusive(&transport_queue_locks[queue_index]);
#else
        pthread_mutex_lock(&transport_queue_locks[queue_index]);
        CHECK(vkQueueSubmit(worker_queue, 1, &submit, fence));
        pthread_mutex_unlock(&transport_queue_locks[queue_index]);
#endif
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        for (unsigned i = 0; i < chunks * words; ++i) {
            if (output[i] != expected_word(seed + i)) {
                fprintf(stderr, "transport verification failed: thread=%u round=%u word=%u\n", index, round, i);
                exit(2);
            }
        }
        CHECK(vkResetFences(device, 1, &fence));
        CHECK(vkResetCommandBuffer(cb, 0));
    }
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, target, NULL);
    vkFreeMemory(device, memory, NULL);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- GPU を伴う Descriptor の検算 ---- */
static VkPipelineLayout ubo_layout, busy_layout;
static VkPipeline ubo_pipeline, busy_pipeline;
static VkCommandPool cmd_pool;
static VkCommandBuffer cmds[RING];
static VkFence ring_fences[RING];

static void record_ubo_dispatch(VkCommandBuffer cb, VkDescriptorSet set) {
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkResetCommandBuffer(cb, 0));
    CHECK(vkBeginCommandBuffer(cb, &begin));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ubo_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ubo_layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                         0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb));
}

/* depth 個のプールを輪番で使い、各プールは自分の Fence が完了してからリセットする。
 * depth > 1 では、あるプールをリセットする時点で別のプールの Set が GPU で使用中になりうる。 */
static double descriptor_gpu_verify(unsigned depth, unsigned iterations, uint32_t seed) {
    VkDescriptorPool pools[RING];
    int pending[RING] = {0};
    for (unsigned r = 0; r < depth; ++r) pools[r] = make_pool(4, 0);
    memset(out_words, 0, POOL_SETS * sizeof(uint32_t) * 4);
    double t0 = clock_ns();
    for (unsigned it = 0; it < iterations; ++it) {
        unsigned r = it % depth;
        uint32_t slot = it % (POOL_SETS * 4 / 4);
        if (pending[r]) {
            CHECK(vkWaitForFences(device, 1, &ring_fences[r], VK_TRUE, UINT64_C(10000000000)));
            CHECK(vkResetFences(device, 1, &ring_fences[r]));
            pending[r] = 0;
        }
        CHECK(vkResetDescriptorPool(device, pools[r], 0));
        VkDescriptorSet set;
        CHECK(alloc_sets(pools[r], 1, &set));
        /* UBO の値は Set ごとに異なる。古い Set や別の Set の内容を読むと照合で検出できる。 */
        uint32_t value = seed + it * UINT32_C(7919);
        ubo_words[slot * (SLOT_BYTES / 4) + 0] = value;
        ubo_words[slot * (SLOT_BYTES / 4) + 1] = it % (POOL_SETS * 4);
        write_set(set, slot);
        record_ubo_dispatch(cmds[r], set);
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmds[r],
        };
        CHECK(vkQueueSubmit(queue, 1, &submit, ring_fences[r]));
        pending[r] = 1;
        /* UBO の slot を次に再利用する前に、その Set を使う処理を完了させる */
        if ((it + 1) % depth == 0) {
            for (unsigned q = 0; q < depth; ++q) {
                if (!pending[q]) continue;
                CHECK(vkWaitForFences(device, 1, &ring_fences[q], VK_TRUE, UINT64_C(10000000000)));
                CHECK(vkResetFences(device, 1, &ring_fences[q]));
                pending[q] = 0;
            }
            for (unsigned k = it + 1 - depth; k <= it; ++k) {
                uint32_t want = expected_word(seed + k * UINT32_C(7919));
                uint32_t got = out_words[k % (POOL_SETS * 4)];
                if (got != want) {
                    fprintf(stderr, "descriptor verify depth=%u it=%u got %08x want %08x\n", depth, k, got, want);
                    exit(2);
                }
            }
        }
    }
    for (unsigned q = 0; q < depth; ++q) {
        if (pending[q]) {
            CHECK(vkWaitForFences(device, 1, &ring_fences[q], VK_TRUE, UINT64_C(10000000000)));
            CHECK(vkResetFences(device, 1, &ring_fences[q]));
        }
    }
    double per_iteration = (clock_ns() - t0) / iterations;
    for (unsigned r = 0; r < depth; ++r) vkDestroyDescriptorPool(device, pools[r], NULL);
    return per_iteration;
}

/* ---- ライフサイクルとエラー処理 ---- */
static void lifecycle_checks(void) {
    VkDescriptorSet sets[8];
    /* 上限 4 のプール。5 個目は失敗してよい（仕様上は成功も許される）が、リセット後の 4 個は必ず成功する */
    VkDescriptorPool pool = make_pool(4, 0);
    CHECK(alloc_sets(pool, 4, sets));
    VkResult over = alloc_sets(pool, 1, &sets[4]);
    if (over != VK_SUCCESS && over != VK_ERROR_OUT_OF_POOL_MEMORY && over != VK_ERROR_FRAGMENTED_POOL)
        die("unexpected result when exceeding pool capacity");
    for (int round = 0; round < 3; ++round) {
        CHECK(vkResetDescriptorPool(device, pool, 0));
        CHECK(alloc_sets(pool, 4, sets));
        for (uint32_t i = 0; i < 4; ++i) write_set(sets[i], i);
    }
    /* Set を割り当てたままのプールを破棄（仕様上有効） */
    vkDestroyDescriptorPool(device, pool, NULL);

    /* FREE フラグ付き: 4 個 → 2 個解放 → 2 個再割り当て */
    VkDescriptorPool free_pool = make_pool(4, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
    CHECK(alloc_sets(free_pool, 4, sets));
    CHECK(vkFreeDescriptorSets(device, free_pool, 2, sets));
    CHECK(alloc_sets(free_pool, 2, sets));
    VkResult over_free = alloc_sets(free_pool, 1, &sets[4]);
    if (over_free != VK_SUCCESS && over_free != VK_ERROR_OUT_OF_POOL_MEMORY &&
        over_free != VK_ERROR_FRAGMENTED_POOL)
        die("unexpected result when exceeding free pool capacity");
    CHECK(vkResetDescriptorPool(device, free_pool, 0));
    CHECK(alloc_sets(free_pool, 4, sets));
    vkDestroyDescriptorPool(device, free_pool, NULL);

    /* セット数ではなく UBO の予約が途中で尽きる。失敗後に同じ予約を再利用できることを確認する。 */
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
    };
    VkDescriptorPoolCreateInfo limited_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                               .maxSets = 4, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool limited;
    CHECK(vkCreateDescriptorPool(device, &limited_info, NULL, &limited));
    sets[0] = sets[1] = loop_set;
    VkResult batch = alloc_sets(limited, 2, sets);
    if (batch != VK_SUCCESS) {
        if (batch != VK_ERROR_OUT_OF_POOL_MEMORY && batch != VK_ERROR_FRAGMENTED_POOL)
            die("unexpected result when exhausting one descriptor type");
        if (sets[0] != VK_NULL_HANDLE || sets[1] != VK_NULL_HANDLE)
            die("failed descriptor allocation left non-null results");
        CHECK(alloc_sets(limited, 1, sets));
    }
    vkDestroyDescriptorPool(device, limited, NULL);
    printf(",\"lifecycle\":{\"over_capacity_result\":%d,\"over_capacity_free_result\":%d,"
           "\"over_descriptor_capacity_result\":%d,\"passed\":true}", over, over_free, batch);
}

static void fence_reuse_check(void) {
    VkFenceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence fences[2];
    CHECK(vkCreateFence(device, &create, NULL, &fences[0]));
    CHECK(vkCreateFence(device, &create, NULL, &fences[1]));
    CHECK(vkGetFenceStatus(device, fences[0]));
    CHECK(vkResetFences(device, 1, fences));
    if (vkGetFenceStatus(device, fences[0]) != VK_NOT_READY) die("reset fence retained completion");

    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkSemaphore gate;
    CHECK(vkCreateSemaphore(device, &sem_info, NULL, &gate));
    uint64_t signal_value = 1;
    uint64_t completion_value = 2;
    VkTimelineSemaphoreSubmitInfo wait_value = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                               .waitSemaphoreValueCount = 1,
                                               .pWaitSemaphoreValues = &signal_value,
                                               .signalSemaphoreValueCount = 1,
                                               .pSignalSemaphoreValues = &completion_value};
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo blocked = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &wait_value,
                            .waitSemaphoreCount = 1, .pWaitSemaphores = &gate,
                            .pWaitDstStageMask = &wait_stage,
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &gate};
    CHECK(vkQueueSubmit(queue, 1, &blocked, fences[0]));
    CHECK(vkWaitForFences(device, 2, fences, VK_FALSE, 0));
    if (vkWaitForFences(device, 2, fences, VK_TRUE, 0) != VK_TIMEOUT)
        die("waitAll completed while a fence is pending");

    int independent_queue_completed = 0;
    if (second_queue != VK_NULL_HANDLE) {
        CHECK(vkResetFences(device, 1, &fences[1]));
        VkSubmitInfo empty = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        CHECK(vkQueueSubmit(second_queue, 1, &empty, fences[1]));
        VkResult result = vkWaitForFences(device, 1, &fences[1], VK_TRUE, UINT64_C(500000000));
        if (result != VK_SUCCESS && result != VK_TIMEOUT) die("independent queue wait failed");
        independent_queue_completed = result == VK_SUCCESS;
    }
    for (int i = 0; i < 16; ++i) {
        if (vkGetFenceStatus(device, fences[0]) != VK_NOT_READY)
            die("fence reported completion before the timeline was signaled");
    }

    VkSemaphoreSignalInfo unblock = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                     .semaphore = gate, .value = signal_value};
    CHECK(vkSignalSemaphore(device, &unblock));
    CHECK(vkWaitForFences(device, 2, fences, VK_TRUE, UINT64_C(10000000000)));
    CHECK(vkGetFenceStatus(device, fences[0]));
    CHECK(vkResetFences(device, 2, fences));
    if (vkGetFenceStatus(device, fences[0]) != VK_NOT_READY) die("reused fence retained completion");
    vkDestroyFence(device, fences[1], NULL);
    vkDestroyFence(device, fences[0], NULL);
    vkDestroySemaphore(device, gate, NULL);
    printf(",\"fence_reuse\":{\"passed\":true,\"independent_queue_completed\":%s}",
           independent_queue_completed ? "true" : "false");
}

/* ---- GPU 実行中の Fence ---- */
static VkDescriptorPool busy_pool;
static VkDescriptorSet busy_set;
static VkQueryPool query;

static void record_busy(VkCommandBuffer cb, uint32_t iterations, uint32_t seed) {
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkResetCommandBuffer(cb, 0));
    CHECK(vkBeginCommandBuffer(cb, &begin));
    vkCmdResetQueryPool(cb, query, 0, 2);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, busy_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, busy_layout, 0, 1, &busy_set, 0, NULL);
    uint32_t params[2] = {iterations, seed};
    vkCmdPushConstants(cb, busy_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), params);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query, 0);
    vkCmdDispatch(cb, BUSY_ELEMENTS / 64, 1, 1);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, query, 1);
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                         0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb));
}

static uint32_t busy_reference(uint32_t index, uint32_t iterations, uint32_t seed) {
    uint32_t x = index ^ seed;
    for (uint32_t i = 0; i < iterations; ++i) x = x * 1664525u + 1013904223u;
    return x;
}

static void fence_gpu_tests(const VkPhysicalDeviceProperties *props, unsigned trials) {
    uint32_t *busy_words;
    VkBuffer busy_buffer;
    VkDeviceMemory busy_memory = make_buffer(BUSY_ELEMENTS * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             &busy_buffer, (void **)&busy_words);
    VkDescriptorBufferInfo info = {busy_buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = busy_set, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &info,
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    const uint32_t iterations = 400000; /* RTX 5070 Ti で約 0.9 ms */
    double gpu_ns[MAX_SAMPLES], poll_done[MAX_SAMPLES], polls_per_ms[MAX_SAMPLES], wait_done[MAX_SAMPLES];
    double period = props->limits.timestampPeriod;
    uint64_t mask = props->limits.timestampComputeAndGraphics ? UINT64_MAX : 0;
    for (unsigned t = 0; t <= 2 * trials; ++t) {
        int use_poll = t % 2 == 0;
        uint32_t seed = 0x1234u + t;
        record_busy(cmds[0], iterations, seed);
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmds[0],
        };
        double t0 = clock_ns();
        CHECK(vkQueueSubmit(queue, 1, &submit, ring_fences[0]));
        unsigned polls = 0;
        if (use_poll) {
            VkResult r;
            while ((r = vkGetFenceStatus(device, ring_fences[0])) == VK_NOT_READY) ++polls;
            if (r != VK_SUCCESS) die("vkGetFenceStatus failed during GPU work");
        } else {
            CHECK(vkWaitForFences(device, 1, &ring_fences[0], VK_TRUE, UINT64_C(10000000000)));
        }
        double done = clock_ns() - t0;
        /* Fence が完了と答えた時点で、GPU の書き込みはすべて見えていなければならない */
        for (uint32_t i = 0; i < BUSY_ELEMENTS; i += 997) {
            if (busy_words[i] != busy_reference(i, iterations, seed)) {
                fprintf(stderr, "fence signaled before GPU results were visible (index %u)\n", i);
                exit(2);
            }
        }
        uint64_t ticks[2];
        CHECK(vkGetQueryPoolResults(device, query, 0, 2, sizeof(ticks), ticks, sizeof(ticks[0]),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        CHECK(vkResetFences(device, 1, &ring_fences[0]));
        if (t == 0) continue; /* ウォームアップ */
        unsigned k = (t - 1) / 2;
        double g = mask ? (double)(ticks[1] - ticks[0]) * period : 0;
        if (use_poll) {
            gpu_ns[k] = g;
            poll_done[k] = done;
            polls_per_ms[k] = polls / (done / 1e6);
        } else {
            wait_done[k] = done;
        }
    }
    begin_test("busy_gpu_time", "ns", 1);
    for (unsigned k = 0; k < trials; ++k) sample(k, gpu_ns[k]);
    end_test();
    begin_test("busy_submit_to_poll_success", "ns", 1);
    for (unsigned k = 0; k < trials; ++k) sample(k, poll_done[k]);
    end_test();
    begin_test("busy_submit_to_wait_return", "ns", 1);
    for (unsigned k = 0; k < trials; ++k) sample(k, wait_done[k]);
    end_test();
    begin_test("busy_fence_polls_per_ms", "polls/ms", 1);
    for (unsigned k = 0; k < trials; ++k) sample(k, polls_per_ms[k]);
    end_test();
    vkUnmapMemory(device, busy_memory);
    vkDestroyBuffer(device, busy_buffer, NULL);
    vkFreeMemory(device, busy_memory, NULL);
}

static VkPipeline make_pipeline(const uint32_t *code, size_t size, VkPipelineLayout layout) {
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code,
    };
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader));
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
    };
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
    vkDestroyShaderModule(device, shader, NULL);
    return pipeline;
}

/* ---- 第 16 段階: マルチスレッド Fence 照会詳細計測・KI-41 起床遅延スイープ ---- */
enum { MT_PROBE_MAX = 16 };

static int cmp_double_p16(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static double pct_sorted(const double *arr, size_t n, double p) {
    if (!n) return 0.0;
    double idx = p * (double)(n - 1);
    size_t i = (size_t)idx;
    if (i + 1 >= n) return arr[n - 1];
    double f = idx - (double)i;
    return arr[i] * (1.0 - f) + arr[i + 1] * f;
}

static void sleep_us_precise(unsigned us) {
    if (us == 0) return;
    double target = clock_ns() + (double)us * 1000.0;
    if (us >= 2000) {
#ifdef _WIN32
        DWORD ms = (DWORD)(us / 1000);
        if (ms > 1) Sleep(ms - 1);
#else
        struct timespec ts = { .tv_sec = (time_t)((us - 500) / 1000000),
                               .tv_nsec = (long)(((us - 500) % 1000000) * 1000) };
        nanosleep(&ts, NULL);
#endif
    } else if (us >= 200) {
#ifndef _WIN32
        struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)((us - 100) * 1000) };
        nanosleep(&ts, NULL);
#endif
    }
    while (clock_ns() < target) {
#ifdef _WIN32
        YieldProcessor();
#else
        __asm__ volatile("pause");
#endif
    }
}

typedef struct {
    volatile long arrived;
    volatile long go;
    unsigned total;
} ProbeBarrier;

static void probe_barrier_wait(ProbeBarrier *b) {
#ifdef _WIN32
    long n = InterlockedIncrement(&b->arrived);
    if ((unsigned)n >= b->total) {
        InterlockedExchange(&b->go, 1);
    } else {
        while (!InterlockedCompareExchange(&b->go, 0, 0)) YieldProcessor();
    }
#else
    long n = __atomic_add_fetch(&b->arrived, 1, __ATOMIC_ACQ_REL);
    if ((unsigned)n >= b->total) {
        __atomic_store_n(&b->go, 1, __ATOMIC_RELEASE);
    } else {
        while (!__atomic_load_n(&b->go, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    }
#endif
}

typedef struct {
    VkFence fence;
    VkResult expected;
    unsigned calls;
    int use_barrier;
    int do_warmup;
    ProbeBarrier *barrier;
    double *lat_ns;
    double elapsed_ns;
    double warmup_ns;
    double user_ms;
    double sys_ms;
    long nvcsw;
    long nivcsw;
    uint64_t cpu_mask;
} MtFenceProbeJob;

#ifdef _WIN32
static unsigned __stdcall mt_fence_probe_worker(void *arg)
#else
static void *mt_fence_probe_worker(void *arg)
#endif
{
    MtFenceProbeJob *j = (MtFenceProbeJob *)arg;
    double tw0 = clock_ns();
    if (j->do_warmup) {
        for (unsigned i = 0; i < 16; ++i) {
            if (vkGetFenceStatus(device, j->fence) != j->expected) die("unexpected fence status (warmup)");
        }
    }
    j->warmup_ns = clock_ns() - tw0;
    if (j->use_barrier && j->barrier) probe_barrier_wait(j->barrier);

#ifndef _WIN32
    struct rusage ru0, ru1;
    getrusage(RUSAGE_THREAD, &ru0);
#endif
    uint64_t mask = 0;
    double t_start = clock_ns();
    for (unsigned i = 0; i < j->calls; ++i) {
        double c0 = clock_ns();
        VkResult r = vkGetFenceStatus(device, j->fence);
        double c1 = clock_ns();
        if (r != j->expected) die("unexpected fence status (probe)");
        j->lat_ns[i] = c1 - c0;
#ifndef _WIN32
        if ((i & 31u) == 0) {
            int cpu = sched_getcpu();
            if (cpu >= 0 && cpu < 64) mask |= (UINT64_C(1) << cpu);
        }
#endif
    }
    j->elapsed_ns = clock_ns() - t_start;
    j->cpu_mask = mask;
#ifndef _WIN32
    getrusage(RUSAGE_THREAD, &ru1);
    j->user_ms = ((double)(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) * 1e3) +
                 ((double)(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e3);
    j->sys_ms = ((double)(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) * 1e3) +
                ((double)(ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e3);
    j->nvcsw = ru1.ru_nvcsw - ru0.ru_nvcsw;
    j->nivcsw = ru1.ru_nivcsw - ru0.ru_nivcsw;
#else
    j->user_ms = 0.0;
    j->sys_ms = 0.0;
    j->nvcsw = 0;
    j->nivcsw = 0;
#endif
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void run_mt_fence_probe_threads(unsigned count, MtFenceProbeJob *jobs) {
#ifdef _WIN32
    HANDLE handles[MT_PROBE_MAX];
    for (unsigned t = 0; t < count; ++t) {
        handles[t] = (HANDLE)_beginthreadex(NULL, 0, mt_fence_probe_worker, &jobs[t], 0, NULL);
        if (!handles[t]) die("_beginthreadex failed");
    }
    for (unsigned t = 0; t < count; ++t) {
        WaitForSingleObject(handles[t], INFINITE);
        CloseHandle(handles[t]);
    }
#else
    pthread_t threads[MT_PROBE_MAX];
    for (unsigned t = 0; t < count; ++t) {
        if (pthread_create(&threads[t], NULL, mt_fence_probe_worker, &jobs[t])) die("pthread_create failed");
    }
    for (unsigned t = 0; t < count; ++t) pthread_join(threads[t], NULL);
#endif
}

static void run_mt_fence_sweep_mode(const VkPhysicalDeviceProperties *props) {
    static const unsigned counts[] = {1, 2, 4, 6, 8, 12, 16};
    const unsigned calls = mt_fence_calls;
    const VkResult expected = mt_fence_signaled ? VK_SUCCESS : VK_NOT_READY;
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = mt_fence_signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0,
    };
    double *all_lat = (double *)malloc((size_t)MT_PROBE_MAX * calls * sizeof(double));
    if (!all_lat) die("malloc failed");

    printf("{\"mode\":\"mt_fence_sweep\",\"device_name\":\"%s\",\"signaled\":%s,\"barrier\":%s,\"warmup\":%s,"
           "\"calls_per_thread\":%u,\"results\":[",
           props->deviceName, mt_fence_signaled ? "true" : "false",
           mt_fence_nobarrier ? "false" : "true", mt_fence_nowarmup ? "false" : "true", calls);

    for (unsigned ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ++ci) {
        unsigned nthreads = counts[ci];
        MtFenceProbeJob jobs[MT_PROBE_MAX];
        VkFence fences[MT_PROBE_MAX];
        ProbeBarrier barrier = {0, 0, nthreads};
        for (unsigned t = 0; t < nthreads; ++t) {
            CHECK(vkCreateFence(device, &fci, NULL, &fences[t]));
            memset(&jobs[t], 0, sizeof(jobs[t]));
            jobs[t].fence = fences[t];
            jobs[t].expected = expected;
            jobs[t].calls = calls;
            jobs[t].use_barrier = !mt_fence_nobarrier;
            jobs[t].do_warmup = !mt_fence_nowarmup;
            jobs[t].barrier = &barrier;
            jobs[t].lat_ns = all_lat + (size_t)t * calls;
        }
        double t_wall0 = clock_ns();
        run_mt_fence_probe_threads(nthreads, jobs);
        double outer_wall_ns = clock_ns() - t_wall0;

        double max_thread_ns = 0.0, sum_user_ms = 0.0, sum_sys_ms = 0.0, max_warmup_ns = 0.0;
        long sum_nvcsw = 0, sum_nivcsw = 0;
        uint64_t all_cpus = 0;
        for (unsigned t = 0; t < nthreads; ++t) {
            if (jobs[t].elapsed_ns > max_thread_ns) max_thread_ns = jobs[t].elapsed_ns;
            if (jobs[t].warmup_ns > max_warmup_ns) max_warmup_ns = jobs[t].warmup_ns;
            sum_user_ms += jobs[t].user_ms;
            sum_sys_ms += jobs[t].sys_ms;
            sum_nvcsw += jobs[t].nvcsw;
            sum_nivcsw += jobs[t].nivcsw;
            all_cpus |= jobs[t].cpu_mask;
            vkDestroyFence(device, fences[t], NULL);
        }
        size_t total_calls = (size_t)nthreads * calls;
        double sum_lat = 0.0;
        for (size_t k = 0; k < total_calls; ++k) sum_lat += all_lat[k];
        qsort(all_lat, total_calls, sizeof(double), cmp_double_p16);

        double mean_ns = sum_lat / (double)total_calls;
        double p50_ns = pct_sorted(all_lat, total_calls, 0.50);
        double p95_ns = pct_sorted(all_lat, total_calls, 0.95);
        double p99_ns = pct_sorted(all_lat, total_calls, 0.99);
        double max_ns = all_lat[total_calls - 1];
        double syncbench_metric_ns = max_thread_ns / (double)total_calls;
        double throughput = (double)total_calls * 1e9 / max_thread_ns;

        printf("%s{\"threads\":%u,\"total_calls\":%zu,\"wall_ms\":%.3f,\"outer_wall_ms\":%.3f,"
               "\"warmup_max_ms\":%.3f,\"syncbench_ns_per_call\":%.1f,\"throughput_calls_s\":%.1f,"
               "\"call_mean_ns\":%.1f,\"call_p50_ns\":%.1f,\"call_p95_ns\":%.1f,\"call_p99_ns\":%.1f,"
               "\"call_max_ns\":%.1f,\"guest_user_ms\":%.2f,\"guest_sys_ms\":%.2f,"
               "\"guest_nvcsw\":%ld,\"guest_nivcsw\":%ld,\"guest_cpu_mask\":\"0x%" PRIx64 "\"}",
               ci ? "," : "", nthreads, total_calls, max_thread_ns / 1e6, outer_wall_ns / 1e6,
               max_warmup_ns / 1e6, syncbench_metric_ns, throughput,
               mean_ns, p50_ns, p95_ns, p99_ns, max_ns,
               sum_user_ms, sum_sys_ms, sum_nvcsw, sum_nivcsw, all_cpus);
        fflush(stdout);
    }
    printf("],\"validated\":true}\n");
    fflush(stdout);
    free(all_lat);
}

static void print_lat_stats_json(const char *key, double *vals, size_t n, int comma) {
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += vals[i];
    qsort(vals, n, sizeof(double), cmp_double_p16);
    printf("%s\"%s\":{\"n\":%zu,\"mean_ns\":%.1f,\"p50_ns\":%.1f,\"p95_ns\":%.1f,\"p99_ns\":%.1f,"
           "\"min_ns\":%.1f,\"max_ns\":%.1f}",
           comma ? "," : "", key, n, sum / (double)n,
           pct_sorted(vals, n, 0.50), pct_sorted(vals, n, 0.95), pct_sorted(vals, n, 0.99),
           vals[0], vals[n - 1]);
}

static void run_wakeup_sweep_mode(const VkPhysicalDeviceProperties *props) {
    static const unsigned gaps_us[] = {0, 10, 50, 100, 500, 1000, 8330, 16670};
    double vals[40];
    printf("{\"mode\":\"wakeup_sweep\",\"device_name\":\"%s\",\"gaps\":[", props->deviceName);
    for (unsigned gi = 0; gi < sizeof(gaps_us) / sizeof(gaps_us[0]); ++gi) {
        unsigned gap = gaps_us[gi];
        unsigned n = (gap >= 8000) ? 20u : 30u;
        printf("%s{\"idle_us\":%u,", gi ? "," : "", gap);

        /* 1. unsignaled fence status (always synchronous RPC to host RenderThread) */
        for (unsigned w = 0; w < 3; ++w) op_fence_unsignaled(NULL);
        for (unsigned i = 0; i < n; ++i) {
            sleep_us_precise(gap);
            double t0 = clock_ns();
            op_fence_unsignaled(NULL);
            vals[i] = clock_ns() - t0;
        }
        print_lat_stats_json("unsignaled_fence_rpc", vals, n, 0);

        /* 2. timeline semaphore counter (pure synchronous RPC without mFenceInfo lock) */
        for (unsigned w = 0; w < 3; ++w) op_semaphore(NULL);
        for (unsigned i = 0; i < n; ++i) {
            sleep_us_precise(gap);
            double t0 = clock_ns();
            op_semaphore(NULL);
            vals[i] = clock_ns() - t0;
        }
        print_lat_stats_json("timeline_counter_rpc", vals, n, 1);

        /* 3. completed fence first status (exercises VELOX_FENCE_SHARED table after idle) */
        for (unsigned i = 0; i < n; ++i) {
            CHECK(vkResetFences(device, 1, &submit_fence));
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
            CHECK(vkQueueSubmit(queue, 1, &submit, submit_fence));
            sleep_us_precise(gap < 500 ? 500 : gap);
            if (gap < 500) {
                /* Wait until GPU finished first, then measure with exact gap if small */
                sleep_us_precise(gap);
            }
            double t0 = clock_ns();
            VkResult r;
            while ((r = vkGetFenceStatus(device, submit_fence)) == VK_NOT_READY) {}
            CHECK(r);
            vals[i] = clock_ns() - t0;
        }
        print_lat_stats_json("completed_fence_first_status", vals, n, 1);

        /* 4. submit empty + wait after idle gap */
        for (unsigned i = 0; i < n; ++i) {
            CHECK(vkResetFences(device, 1, &submit_fence));
            sleep_us_precise(gap);
            double t0 = clock_ns();
            submit_empty_and_wait(submit_fence);
            vals[i] = clock_ns() - t0;
        }
        print_lat_stats_json("submit_empty_wait_after_idle", vals, n, 1);

        printf("}");
        fflush(stdout);
    }
    CHECK(vkResetFences(device, 1, &submit_fence));
    printf("],\"validated\":true}\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    unsigned long index = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--quick")) {
            quick = 1;
        } else if (!strcmp(argv[i], "--skip-mt-desc")) {
            /* 標準ドライバーの参照カウント競合（patches/mesa3d/0002）で落ちる試験を除く */
            skip_mt_desc = 1;
        } else if (!strcmp(argv[i], "--mt-fence-sweep")) {
            mt_fence_sweep = 1;
        } else if (!strcmp(argv[i], "--mt-fence-signaled")) {
            mt_fence_signaled = 1;
        } else if (!strcmp(argv[i], "--mt-fence-nobarrier")) {
            mt_fence_nobarrier = 1;
        } else if (!strcmp(argv[i], "--mt-fence-nowarmup")) {
            mt_fence_nowarmup = 1;
        } else if (!strcmp(argv[i], "--mt-fence-calls")) {
            if (++i == argc) die("expected call count after --mt-fence-calls");
            mt_fence_calls = (unsigned)strtoul(argv[i], NULL, 10);
            if (!mt_fence_calls || mt_fence_calls > 10000) die("mt-fence-calls must be 1..10000");
        } else if (!strcmp(argv[i], "--wakeup-sweep")) {
            wakeup_sweep = 1;
        } else if (!strcmp(argv[i], "--mt-stress") || !strcmp(argv[i], "--transport-stress")) {
            unsigned *rounds = !strcmp(argv[i], "--mt-stress") ? &mt_stress_rounds : &transport_stress_rounds;
            if (++i == argc || mt_stress_rounds || transport_stress_rounds) die("expected one stress mode and a round count");
            char *end;
            unsigned long value = strtoul(argv[i], &end, 10);
            if (argv[i][0] < '0' || argv[i][0] > '9' || !value || value > 100000 || *end)
                die("stress rounds must be between 1 and 100000");
            *rounds = (unsigned)value;
        } else {
            char *end = NULL;
            index = strtoul(argv[i], &end, 10);
            if (end == argv[i] || *end) die("usage: vk-syncbench [--quick] [--skip-mt-desc] [--mt-stress rounds | --transport-stress rounds | --mt-fence-sweep | --wakeup-sweep] [device-index]");
        }
    }
    const unsigned samples = quick ? 10 : 40;
    const unsigned batch = quick ? 16 : 64;

    PFN_vkGetInstanceProcAddr lookup = load_vulkan();
    if (!lookup) die("Vulkan loader unavailable");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)lookup(NULL, "vkCreateInstance");
    if (!vkCreateInstance) die("Missing vkCreateInstance");
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Velox syncbench",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application,
    };
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
#define LOAD_INSTANCE(name) name = (PFN_##name)lookup(instance, #name); if (!name) die("Missing " #name);
    INSTANCE_FUNCS(LOAD_INSTANCE)

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (index >= count) die("Physical device index does not exist");
    VkPhysicalDevice devices[16];
    if (count > 16) count = 16;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = devices[index];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    if (props.apiVersion < VK_API_VERSION_1_2 || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        die("A Vulkan 1.2 hardware device is required");
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &timeline_features,
    };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!timeline_features.timelineSemaphore) die("timelineSemaphore is required");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties families[16];
    if (count > 16) count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == count) die("No compute queue");
    const uint32_t queue_count = families[family].queueCount > 1 ? 2 : 1;
    float priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
        .queueCount = queue_count, .pQueuePriorities = priorities,
    };
    VkPhysicalDeviceTimelineSemaphoreFeatures enable_timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enable_timeline, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
    };
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
#define LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name); if (!name) die("Missing " #name);
    DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(device, family, 0, &queue);
    if (queue_count == 2) vkGetDeviceQueue(device, family, 1, &second_queue);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

    if (transport_stress_rounds) {
        unsigned jobs[TRANSPORT_THREADS] = {0, 1, 2, 3};
        double start = clock_ns();
        run_threads(TRANSPORT_THREADS, jobs, sizeof(jobs[0]), transport_thread);
        double seconds = (clock_ns() - start) / 1e9;
        const uint64_t large = transport_stress_rounds / 2;
        const uint64_t small = transport_stress_rounds - large;
        const uint64_t bytes = (large * TRANSPORT_CHUNKS * TRANSPORT_CHUNK_WORDS + small * 16) *
                               TRANSPORT_THREADS * sizeof(uint32_t);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        printf("{\"transport_stress_rounds\":%u,\"threads\":%d,\"queues\":%u,"
               "\"verified_bytes\":%" PRIu64 ",\"seconds\":%.3f,\"passed\":true}\n",
               transport_stress_rounds, TRANSPORT_THREADS, queue_count, bytes, seconds);
        return 0;
    }

    /* 共通の資源 */
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings,
    };
    CHECK(vkCreateDescriptorSetLayout(device, &layout_info, NULL, &set_layout));
    VkDescriptorSetLayoutBinding busy_binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo busy_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &busy_binding,
    };
    VkDescriptorSetLayout busy_set_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &busy_layout_info, NULL, &busy_set_layout));
    VkPipelineLayoutCreateInfo pl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout,
    };
    CHECK(vkCreatePipelineLayout(device, &pl, NULL, &ubo_layout));
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo bpl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
        .pSetLayouts = &busy_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    CHECK(vkCreatePipelineLayout(device, &bpl, NULL, &busy_layout));
    ubo_pipeline = make_pipeline(ubo_shader, sizeof(ubo_shader), ubo_layout);
    busy_pipeline = make_pipeline(busy_shader, sizeof(busy_shader), busy_layout);
    VkDeviceMemory ubo_memory = make_buffer((VkDeviceSize)POOL_SETS * SLOT_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                            &ubo_buffer, (void **)&ubo_words);
    VkDeviceMemory out_memory = make_buffer(POOL_SETS * 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            &out_buffer, (void **)&out_words);
    VkCommandPoolCreateInfo cp = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family,
    };
    CHECK(vkCreateCommandPool(device, &cp, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cba = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = RING,
    };
    CHECK(vkAllocateCommandBuffers(device, &cba, cmds));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int r = 0; r < RING; ++r) CHECK(vkCreateFence(device, &fci, NULL, &ring_fences[r]));
    CHECK(vkCreateFence(device, &fci, NULL, &fence_unsignaled));
    CHECK(vkCreateFence(device, &fci, NULL, &fence_signaled));
    CHECK(vkCreateFence(device, &fci, NULL, &submit_fence));
    submit_empty_and_wait(fence_signaled);
    VkSemaphoreTypeCreateInfo sti = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti};
    CHECK(vkCreateSemaphore(device, &sci, NULL, &timeline));
    VkQueryPoolCreateInfo qpi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
    };
    CHECK(vkCreateQueryPool(device, &qpi, NULL, &query));
    loop_pool = make_pool(POOL_SETS, 0);
    empty_pool = make_pool(POOL_SETS, 0);
    busy_pool = make_pool(4, 0);
    CHECK(alloc_sets(loop_pool, 1, &loop_set));
    VkDescriptorSetAllocateInfo bai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = busy_pool,
        .descriptorSetCount = 1, .pSetLayouts = &busy_set_layout,
    };
    CHECK(vkAllocateDescriptorSets(device, &bai, &busy_set));

    if (mt_stress_rounds) {
        /* 8 スレッドが同じ Set Layout で、それぞれのプールのリセット・割り当て・更新を繰り返す */
        for (unsigned r = 0; r < mt_stress_rounds; ++r) {
            ThreadJob jobs[MAX_THREADS];
            for (unsigned t = 0; t < MAX_THREADS; ++t) jobs[t].frames = 200;
            run_threads(MAX_THREADS, jobs, sizeof(jobs[0]), frame_thread);
        }
        printf("{\"mt_stress_rounds\":%u,\"threads\":%d,\"frames_per_thread\":200,\"passed\":true}\n",
               mt_stress_rounds, MAX_THREADS);
        goto cleanup;
    }
    if (mt_fence_sweep) {
        run_mt_fence_sweep_mode(&props);
        goto cleanup;
    }
    if (wakeup_sweep) {
        run_wakeup_sweep_mode(&props);
        goto cleanup;
    }
    printf("{\"schema\":1,\"vendor_id\":%u,\"device_id\":%u,\"api_version\":%u,\"driver_version\":%u,"
           "\"device_name\":\"%s\",\"hal\":\"%s\",\"quick\":%s,\"timer_overhead_ns\":%.1f,\"tests\":{",
           props.vendorID, props.deviceID, props.apiVersion, props.driverVersion, props.deviceName,
#ifdef _WIN32
           "",
#else
           getenv("VELOX_VK_HAL") ? getenv("VELOX_VK_HAL") : "",
#endif
           quick ? "true" : "false", timer_overhead_ns());

    /* 1. 単純な往復と Fence */
    test_semaphore_rpc();
    emit_loop("semaphore_counter_value", samples, batch, op_semaphore, NULL);
    emit_loop("fence_status_unsignaled", samples, batch, op_fence_unsignaled, NULL);
    emit_loop("fence_status_signaled", samples, batch, op_fence_signaled, NULL);
    emit_loop("fence_reset", samples, batch, op_fence_reset, NULL);
    emit_loop("submit_empty_wait", samples, quick ? 4 : 8, op_submit_wait, NULL);
    /* 第 14 段階: GPU が終えた後の最初の照会・待ち（それまで誰も完了を確かめていない Fence）。
       ゲストの完了キャッシュは使えず、ホストの Fence 共有表（VELOX_FENCE_SHARED）があれば RPC を省ける。 */
    for (int mode = 0; mode < 2; ++mode) {
        begin_test(mode ? "fence_completed_first_wait" : "fence_completed_first_status", "ns", 1);
        for (unsigned s = 0; s < samples; ++s) {
            CHECK(vkResetFences(device, 1, &submit_fence));
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
            CHECK(vkQueueSubmit(queue, 1, &submit, submit_fence));
#ifdef _WIN32
            Sleep(3);
#else
            usleep(3000);
#endif
            double t0 = clock_ns();
            if (mode) {
                CHECK(vkWaitForFences(device, 1, &submit_fence, VK_TRUE, UINT64_C(10000000000)));
            } else {
                VkResult r;
                while ((r = vkGetFenceStatus(device, submit_fence)) == VK_NOT_READY) {}
                CHECK(r);
            }
            sample(s, clock_ns() - t0);
        }
        end_test();
    }
    CHECK(vkResetFences(device, 1, &submit_fence));

    /* 2. Descriptor */
    emit_loop("dpool_create_destroy", samples, quick ? 4 : 16, op_pool_create_destroy, NULL);
    emit_loop("dpool_reset_empty", samples, batch, op_reset_empty, NULL);
    emit_loop("dset_update_2writes", samples, batch, op_update, NULL);
    test_alloc_and_reset(samples);
    {
        VkDescriptorSet sets[32];
        begin_test("frame_reset_alloc32_update", "ns", 1);
        for (unsigned s = 0; s < samples; ++s) {
            double t0 = clock_ns();
            frame_pattern(loop_pool, sets, 32);
            sample(s, clock_ns() - t0);
        }
        end_test();
        CHECK(vkResetDescriptorPool(device, loop_pool, 0));
        CHECK(alloc_sets(loop_pool, 1, &loop_set));
    }
    /* 3. スレッド数による変化（各スレッドが自分のプールでフレーム処理を繰り返す） */
    static const unsigned thread_counts[] = {1, 2, 4, 8};
    for (unsigned c = 0; c < sizeof(thread_counts) / sizeof(thread_counts[0]); ++c) {
        unsigned threads = thread_counts[c];
        char name[64];
        snprintf(name, sizeof(name), "frame_mt%u_ns_per_frame", threads);
        if (!skip_mt_desc) begin_test(name, "ns", 1);
        for (unsigned s = 0; !skip_mt_desc && s < (quick ? 3u : 5u); ++s) {
            ThreadJob jobs[MAX_THREADS];
            for (unsigned t = 0; t < threads; ++t) jobs[t].frames = quick ? 10 : 30;
            run_threads(threads, jobs, sizeof(jobs[0]), frame_thread);
            /* スレッド生成とプール作成を除くため、各スレッド内で測った時間の最大値を使う */
            double wall = 0;
            for (unsigned t = 0; t < threads; ++t) wall = jobs[t].elapsed_ns > wall ? jobs[t].elapsed_ns : wall;
            sample(s, wall / (threads * jobs[0].frames));
        }
        if (!skip_mt_desc) end_test();
        snprintf(name, sizeof(name), "fence_status_mt%u_ns_per_call", threads);
        begin_test(name, "ns", 1);
        for (unsigned s = 0; s < (quick ? 3u : 5u); ++s) {
            FenceJob jobs[MAX_THREADS];
            VkFence fences[MAX_THREADS];
            for (unsigned t = 0; t < threads; ++t) {
                CHECK(vkCreateFence(device, &fci, NULL, &fences[t]));
                jobs[t].fence = fences[t];
                jobs[t].calls = quick ? 200 : 1000;
            }
            run_threads(threads, jobs, sizeof(jobs[0]), fence_thread);
            double wall = 0;
            for (unsigned t = 0; t < threads; ++t) wall = jobs[t].elapsed_ns > wall ? jobs[t].elapsed_ns : wall;
            sample(s, wall / (threads * jobs[0].calls));
            for (unsigned t = 0; t < threads; ++t) vkDestroyFence(device, fences[t], NULL);
        }
        end_test();
    }
    /* 4. GPU を伴う検算 */
    for (unsigned depth = 1; depth <= RING; depth *= 2) {
        char name[64];
        snprintf(name, sizeof(name), "gpu_descriptor_verify_depth%u", depth);
        begin_test(name, "ns", 1);
        for (unsigned s = 0; s < (quick ? 3u : 5u); ++s)
            sample(s, descriptor_gpu_verify(depth, quick ? 64 : VERIFY_ITERATIONS, 0xabc00000u + s * 131u + depth));
        end_test();
    }
    fence_gpu_tests(&props, quick ? 5 : 20);
    printf("}");
    lifecycle_checks();
    fence_reuse_check();
    printf(",\"validated\":true}\n");
    fflush(stdout);

cleanup:
    CHECK(vkDeviceWaitIdle(device));
    vkDestroyQueryPool(device, query, NULL);
    vkDestroySemaphore(device, timeline, NULL);
    vkDestroyFence(device, submit_fence, NULL);
    vkDestroyFence(device, fence_signaled, NULL);
    vkDestroyFence(device, fence_unsignaled, NULL);
    for (int r = 0; r < RING; ++r) vkDestroyFence(device, ring_fences[r], NULL);
    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyDescriptorPool(device, busy_pool, NULL);
    vkDestroyDescriptorPool(device, loop_pool, NULL);
    vkDestroyDescriptorPool(device, empty_pool, NULL);
    vkUnmapMemory(device, ubo_memory);
    vkUnmapMemory(device, out_memory);
    vkDestroyBuffer(device, ubo_buffer, NULL);
    vkDestroyBuffer(device, out_buffer, NULL);
    vkFreeMemory(device, ubo_memory, NULL);
    vkFreeMemory(device, out_memory, NULL);
    vkDestroyPipeline(device, ubo_pipeline, NULL);
    vkDestroyPipeline(device, busy_pipeline, NULL);
    vkDestroyPipelineLayout(device, ubo_layout, NULL);
    vkDestroyPipelineLayout(device, busy_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyDescriptorSetLayout(device, busy_set_layout, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
