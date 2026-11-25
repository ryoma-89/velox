/*
 * codec-stress: 録画デモの停止（KI-24）を AnTuTu なしで再現するための動画デコード負荷試験。
 *
 * MediaCodec（既定ではシステムが選ぶ H.264 デコーダー = c2.goldfish.h264.decoder）で
 * MP4 を繰り返しデコードし、出力を AImageReader（GPU で読める PRIVATE 形式）へ描画する。
 * 消費側スレッドは、Unreal の Android 動画再生（SurfaceTexture → GLES → glReadPixels）に近い処理として、
 * 受け取ったフレームを EGLImage として GLES の外部テクスチャに結び付け、FBO に描いて glReadPixels する。
 *
 * 出力または開始・終了処理が --stall-ms の間進まなければ、監視スレッドが
 * 停止箇所を JSON で出して --hold 秒待つ。その間に debuggerd などで状態を採取できる。
 * 終了コード: 0 正常、3 停止を検出、1 その他の失敗。
 *
 * 使い方: codec-stress <file.mp4> [--seconds N] [--stall-ms N] [--hold N]
 *                     [--consumer gl|cpu|none] [--max-images N] [--decoder NAME]
 *                     [--restart-ms N] [--switch-ms N] [--fifo] [--release-delay-ms N]
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <errno.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void die(const char *message) {
    fprintf(stderr, "codec-stress: %s\n", message);
    exit(1);
}

enum { CONSUMER_NONE, CONSUMER_CPU, CONSUMER_GL };
enum { MAX_GAPS = 1 << 20 };

struct Consumer {
    AImageReader *readers[2];
    size_t reader_count;
    bool fifo;
    int release_delay_ms;
    atomic_bool stop;
};

struct Watchdog {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    const char *stage;
    int session;
    int hold_seconds;
    int64_t stall_ns;
    int64_t deadline_ns;
    bool finished;
    bool stalled;
};

static struct Watchdog watch = { .mutex = PTHREAD_MUTEX_INITIALIZER };
static int consumer_mode = CONSUMER_GL;
static atomic_llong consumed, consumer_errors, last_consumed_ns;
static int32_t video_width, video_height;

static void watch_stage(const char *stage, int session, int64_t since_ns) {
    pthread_mutex_lock(&watch.mutex);
    /* 採取中に処理が戻っても、停止時のオブジェクトを解放せず監視スレッドの終了を待つ。 */
    while (watch.stalled) pthread_cond_wait(&watch.changed, &watch.mutex);
    watch.stage = stage;
    watch.session = session;
    watch.deadline_ns = since_ns + watch.stall_ns;
    pthread_mutex_unlock(&watch.mutex);
}

static void *watchdog_thread(void *unused) {
    (void)unused;
    pthread_mutex_lock(&watch.mutex);
    while (!watch.finished) {
        const int64_t now = now_ns();
        if (now >= watch.deadline_ns) {
            watch.stalled = true;
            const char *stage = watch.stage;
            const int session = watch.session;
            pthread_mutex_unlock(&watch.mutex);
            const int64_t last_image = atomic_load(&last_consumed_ns);
            dprintf(STDOUT_FILENO, "{\"stalled\":true,\"stage\":\"%s\",\"session\":%d,\"consumed\":%lld,"
                    "\"consumer_errors\":%lld,\"ms_since_consumed\":%.1f}\n",
                    stage, session, (long long)atomic_load(&consumed), (long long)atomic_load(&consumer_errors),
                    last_image ? (now - last_image) / 1e6 : -1.0);
            if (watch.hold_seconds) sleep((unsigned)watch.hold_seconds);
            _exit(3);
        }
        const struct timespec deadline = { .tv_sec = watch.deadline_ns / 1000000000,
                                           .tv_nsec = watch.deadline_ns % 1000000000 };
        /* 期限の更新は同じ時刻か後ろに延びるため、元の期限で再確認するだけでよい。 */
        const int status = pthread_cond_timedwait(&watch.changed, &watch.mutex, &deadline);
        if (status && status != ETIMEDOUT) die("watchdog wait failed");
    }
    pthread_mutex_unlock(&watch.mutex);
    return NULL;
}

/* ---- GLES 消費側 ---- */
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_client_buffer;
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) die("shader compile failed");
    return shader;
}

static void *consumer_thread(void *arg) {
    struct Consumer *consumer = arg;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0, texture = 0, fbo = 0, target = 0;
    /* 読み戻しは縮小せず、動画と同じ解像度で行う（Unreal の Vulkan 経路は全画素を CPU に戻す） */
    uint8_t *pixels = NULL;
    if (consumer_mode == CONSUMER_GL) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) die("eglInitialize failed");
        const EGLint config_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                       EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        if (!eglChooseConfig(display, config_attrs, &config, 1, &count) || count < 1) die("eglChooseConfig failed");
        const EGLint pbuffer_attrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, pbuffer_attrs);
        const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT) die("EGL context creation failed");
        if (!eglMakeCurrent(display, surface, surface, context)) die("eglMakeCurrent failed");
        get_client_buffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
        create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!get_client_buffer || !create_image || !destroy_image || !image_target_texture)
            die("EGLImage extensions are missing");
        const char *vs = "attribute vec2 p; varying vec2 uv;"
                         "void main(){ uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }";
        const char *fs = "#extension GL_OES_EGL_image_external : require\n"
                         "precision mediump float; varying vec2 uv; uniform samplerExternalOES t;"
                         "void main(){ gl_FragColor = texture2D(t, uv); }";
        program = glCreateProgram();
        glAttachShader(program, compile(GL_VERTEX_SHADER, vs));
        glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
        glBindAttribLocation(program, 0, "p");
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) die("program link failed");
        glGenTextures(1, &texture);
        glGenTextures(1, &target);
        glBindTexture(GL_TEXTURE_2D, target);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, video_width, video_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("framebuffer incomplete");
        pixels = malloc((size_t)video_width * (size_t)video_height * 4);
        if (!pixels) die("out of memory");
    }
    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    size_t next_reader = 0;
    while (!atomic_load(&consumer->stop)) {
        AImage *image = NULL;
        for (size_t attempt = 0; attempt < consumer->reader_count; ++attempt) {
            AImageReader *reader = consumer->readers[next_reader];
            next_reader = (next_reader + 1) % consumer->reader_count;
            media_status_t status = consumer->fifo ? AImageReader_acquireNextImage(reader, &image)
                                                  : AImageReader_acquireLatestImage(reader, &image);
            if (status == AMEDIA_OK && image) break;
            if (status != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) atomic_fetch_add(&consumer_errors, 1);
        }
        if (!image) {
            usleep(2000);
            continue;
        }
        if (consumer_mode == CONSUMER_GL) {
            AHardwareBuffer *buffer = NULL;
            if (AImage_getHardwareBuffer(image, &buffer) != AMEDIA_OK || !buffer) {
                atomic_fetch_add(&consumer_errors, 1);
            } else {
                const EGLint image_attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
                EGLImageKHR egl_image = create_image(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                                     get_client_buffer(buffer), image_attrs);
                if (egl_image == EGL_NO_IMAGE_KHR) {
                    atomic_fetch_add(&consumer_errors, 1);
                } else {
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
                    image_target_texture(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)egl_image);
                    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glViewport(0, 0, video_width, video_height);
                    glUseProgram(program);
                    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
                    glEnableVertexAttribArray(0);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glReadPixels(0, 0, video_width, video_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    if (glGetError() != GL_NO_ERROR) {
                        glFinish();
                        atomic_fetch_add(&consumer_errors, 1);
                    }
                    destroy_image(display, egl_image);
                }
            }
        }
        if (consumer->release_delay_ms) usleep((unsigned)consumer->release_delay_ms * 1000);
        AImage_delete(image);
        atomic_fetch_add(&consumed, 1);
        atomic_store(&last_consumed_ns, now_ns());
    }
    if (consumer_mode == CONSUMER_GL) {
        free(pixels);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    return NULL;
}

static int compare_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int parse_int(const char *value) {
    char *end;
    errno = 0;
    long number = strtol(value, &end, 10);
    if (errno || end == value || *end || number < 0 || number > INT_MAX) die("invalid integer argument");
    return (int)number;
}

static void check_media(media_status_t status, const char *operation) {
    if (status == AMEDIA_OK) return;
    fprintf(stderr, "codec-stress: %s failed (%d)\n", operation, status);
    exit(1);
}

int main(int argc, char **argv) {
    const char *path = NULL, *decoder_name = NULL;
    double seconds = 60;
    int stall_ms = 3000, hold = 0, max_images = 4;
    int restart_ms = 0, switch_ms = 0, release_delay_ms = 0;
    bool fifo = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            const char *value = argv[++i];
            char *end;
            seconds = strtod(value, &end);
            if (end == value || *end) die("invalid --seconds");
        }
        else if (!strcmp(argv[i], "--stall-ms") && i + 1 < argc) stall_ms = parse_int(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = parse_int(argv[++i]);
        else if (!strcmp(argv[i], "--max-images") && i + 1 < argc) max_images = parse_int(argv[++i]);
        else if (!strcmp(argv[i], "--restart-ms") && i + 1 < argc) restart_ms = parse_int(argv[++i]);
        else if (!strcmp(argv[i], "--switch-ms") && i + 1 < argc) switch_ms = parse_int(argv[++i]);
        else if (!strcmp(argv[i], "--release-delay-ms") && i + 1 < argc) release_delay_ms = parse_int(argv[++i]);
        else if (!strcmp(argv[i], "--fifo")) fifo = true;
        else if (!strcmp(argv[i], "--decoder") && i + 1 < argc) decoder_name = argv[++i];
        else if (!strcmp(argv[i], "--consumer") && i + 1 < argc) {
            const char *mode = argv[++i];
            consumer_mode = !strcmp(mode, "gl") ? CONSUMER_GL : !strcmp(mode, "cpu") ? CONSUMER_CPU
                          : !strcmp(mode, "none") ? CONSUMER_NONE : -1;
            if (consumer_mode < 0) die("--consumer must be gl, cpu or none");
        } else if (argv[i][0] != '-' && !path) path = argv[i];
        else die("usage: codec-stress <file.mp4> [--seconds N] [--stall-ms N] [--hold N] "
                 "[--consumer gl|cpu|none] [--max-images N] [--decoder NAME] "
                 "[--restart-ms N] [--switch-ms N] [--fifo] [--release-delay-ms N]");
    }
    if (!path || !(seconds >= 0.1 && seconds <= 7200) || stall_ms < 100 || hold > 3600 ||
        max_images < 2 || max_images > 16 || release_delay_ms > 500)
        die("invalid arguments");

    /* Codec2 サービスは出力バッファの確保をクライアント側へ binder で依頼する（IGraphicBufferAllocator）。
     * 受け手のスレッドがないと最初のフレームで止まるので、アプリと同じくスレッドプールを起動する。 */
    /* ABinderProcess_* は NDK の公開ヘッダーにないので、libbinder_ndk から実行時に取得する */
    void *binder = dlopen("libbinder_ndk.so", RTLD_NOW);
    bool (*set_max_threads)(uint32_t) = binder ? dlsym(binder, "ABinderProcess_setThreadPoolMaxThreadCount") : NULL;
    void (*start_pool)(void) = binder ? dlsym(binder, "ABinderProcess_startThreadPool") : NULL;
    if (!set_max_threads || !start_pool) die("cannot start the binder thread pool");
    set_max_threads(4);
    start_pool();

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) die("cannot open input");
    AMediaExtractor *extractor = AMediaExtractor_new();
    if (!extractor) die("cannot create extractor");
    check_media(AMediaExtractor_setDataSourceFd(extractor, fd, 0, st.st_size), "setDataSource");
    AMediaFormat *format = NULL;
    const char *mime = NULL;
    for (size_t t = 0; t < AMediaExtractor_getTrackCount(extractor); ++t) {
        AMediaFormat *candidate = AMediaExtractor_getTrackFormat(extractor, t);
        const char *m = NULL;
        if (AMediaFormat_getString(candidate, AMEDIAFORMAT_KEY_MIME, &m) && !strncmp(m, "video/", 6)) {
            check_media(AMediaExtractor_selectTrack(extractor, t), "selectTrack");
            format = candidate;
            mime = m;
            break;
        }
        AMediaFormat_delete(candidate);
    }
    if (!format) die("no video track");
    int64_t duration_us = 0;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &video_width);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &video_height);
    AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration_us);
    if (video_width <= 0 || video_height <= 0 || duration_us <= 0 ||
        (size_t)video_width > SIZE_MAX / 4 / (size_t)video_height)
        die("invalid video track");

    int64_t *gaps = calloc(MAX_GAPS, sizeof(*gaps));
    if (!gaps) die("out of memory");
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) || pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) ||
        pthread_cond_init(&watch.changed, &attr))
        die("cannot initialize watchdog");
    pthread_condattr_destroy(&attr);
    watch.stall_ns = (int64_t)stall_ms * 1000000;
    watch.hold_seconds = hold;
    watch_stage("create_surfaces", 1, now_ns());
    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, watchdog_thread, NULL)) die("cannot start watchdog");

    /* PRIVATE 形式は CPU 読み取り用の usage を指定できない。 */
    const int32_t image_format = consumer_mode == CONSUMER_CPU ? AIMAGE_FORMAT_YUV_420_888 : AIMAGE_FORMAT_PRIVATE;
    const uint64_t usage = consumer_mode == CONSUMER_CPU ? AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
                                                        : AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    char *codec_name = NULL;
    int sessions = 0, switches = 0;
    size_t gap_count = 0;
    int64_t start = 0, deadline = 0, end = 0, last_output = 0, max_first_output_ns = 0;
    int64_t queued = 0, rendered = 0, format_changes = 0, loops = 0, try_again = 0, sessions_with_output = 0;
    do {
        ++sessions;
        watch_stage("create_surfaces", sessions, now_ns());
        struct Consumer consumer = { .reader_count = switch_ms ? 2 : 1, .fifo = fifo,
                                     .release_delay_ms = release_delay_ms };
        atomic_init(&consumer.stop, false);
        ANativeWindow *windows[2];
        for (size_t i = 0; i < consumer.reader_count; ++i) {
            check_media(AImageReader_newWithUsage(video_width, video_height, image_format, usage,
                                                 max_images, &consumer.readers[i]), "AImageReader_newWithUsage");
            check_media(AImageReader_getWindow(consumer.readers[i], &windows[i]), "AImageReader_getWindow");
        }
        pthread_t consumer_id;
        if (pthread_create(&consumer_id, NULL, consumer_thread, &consumer)) die("cannot start consumer");

        watch_stage("create_codec", sessions, now_ns());
        AMediaCodec *codec = decoder_name ? AMediaCodec_createCodecByName(decoder_name)
                                          : AMediaCodec_createDecoderByType(mime);
        if (!codec) die("cannot create decoder");
        if (!codec_name) {
            char *name = NULL;
            if (AMediaCodec_getName(codec, &name) == AMEDIA_OK && name) {
                codec_name = strdup(name);
                AMediaCodec_releaseName(codec, name);
            }
        }

        watch_stage("configure_codec", sessions, now_ns());
        check_media(AMediaCodec_configure(codec, format, windows[0], NULL, 0), "configure");
        watch_stage("start_codec", sessions, now_ns());
        check_media(AMediaCodec_start(codec), "start");
        const int64_t session_start = now_ns();
        if (!start) {
            start = session_start;
            deadline = start + (int64_t)(seconds * 1e9);
        }
        last_output = session_start;
        watch_stage("decode", sessions, last_output);
        const int64_t restart_at = restart_ms ? session_start + (int64_t)restart_ms * 1000000 : deadline;
        const int64_t switch_interval = (int64_t)switch_ms * 1000000;
        int64_t next_switch = session_start + switch_interval;
        size_t active_window = 0;
        int64_t loop_offset_us = 0, max_pts_us = 0;
        bool first_output = false;
        if (sessions > 1)
            check_media(AMediaExtractor_seekTo(extractor, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC), "seekTo");
        while ((end = now_ns()) < deadline && (end < restart_at || !first_output)) {
            if (switch_interval && end >= next_switch) {
                watch_stage("switch_surface", sessions, last_output);
                active_window = 1 - active_window;
                check_media(AMediaCodec_setOutputSurface(codec, windows[active_window]), "setOutputSurface");
                ++switches;
                next_switch = now_ns() + switch_interval;
                watch_stage("decode", sessions, last_output);
            }
            ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 5000);
            if (in >= 0) {
                size_t capacity = 0;
                uint8_t *buffer = AMediaCodec_getInputBuffer(codec, (size_t)in, &capacity);
                if (!buffer || !capacity) die("cannot get input buffer");
                ssize_t size = AMediaExtractor_readSampleData(extractor, buffer, capacity);
                if (size < 0) {
                    /* 終端から先頭へ戻っても、同じセッション内では PTS を進める。 */
                    if (max_pts_us > INT64_MAX - 33333) die("input timestamp overflow");
                    loop_offset_us = max_pts_us + 33333;
                    ++loops;
                    check_media(AMediaExtractor_seekTo(extractor, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC), "seekTo");
                    size = AMediaExtractor_readSampleData(extractor, buffer, capacity);
                }
                if (size < 0 || (size_t)size > capacity) die("invalid input sample size");
                int64_t sample_time = AMediaExtractor_getSampleTime(extractor);
                if (sample_time < 0 || sample_time > INT64_MAX - loop_offset_us) die("invalid sample timestamp");
                int64_t pts = sample_time + loop_offset_us;
                if (pts > max_pts_us) max_pts_us = pts;
                check_media(AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, (size_t)size, (uint64_t)pts, 0),
                            "queueInputBuffer");
                AMediaExtractor_advance(extractor);
                ++queued;
            } else if (in != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                die("dequeueInputBuffer failed");
            }
            AMediaCodecBufferInfo info;
            ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
            if (out >= 0) {
                check_media(AMediaCodec_releaseOutputBuffer(codec, (size_t)out, info.size > 0), "releaseOutputBuffer");
                if (info.size <= 0) continue;
                int64_t now = now_ns();
                ++rendered;
                if (gap_count < MAX_GAPS) gaps[gap_count++] = now - last_output;
                last_output = now;
                watch_stage("decode", sessions, last_output);
                if (!first_output) {
                    first_output = true;
                    ++sessions_with_output;
                    int64_t startup_ns = now - session_start;
                    if (startup_ns > max_first_output_ns) max_first_output_ns = startup_ns;
                }
            } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                ++format_changes;
            } else if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                ++try_again;
            } else if (out != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                die("dequeueOutputBuffer failed");
            }
        }
        /* デコーダーの終了処理中も、両方の Surface から画像を回収する。 */
        watch_stage("stop_codec", sessions, now_ns());
        check_media(AMediaCodec_stop(codec), "stop");
        watch_stage("delete_codec", sessions, now_ns());
        check_media(AMediaCodec_delete(codec), "delete");
        watch_stage("destroy_surfaces", sessions, now_ns());
        atomic_store(&consumer.stop, true);
        if (pthread_join(consumer_id, NULL)) die("cannot join consumer");
        for (size_t i = 0; i < consumer.reader_count; ++i) AImageReader_delete(consumer.readers[i]);
    } while (now_ns() < deadline);
    pthread_mutex_lock(&watch.mutex);
    watch.finished = true;
    pthread_cond_signal(&watch.changed);
    pthread_mutex_unlock(&watch.mutex);
    if (pthread_join(watchdog, NULL)) die("cannot join watchdog");

    qsort(gaps, gap_count, sizeof(*gaps), compare_i64);
    const double elapsed = (end - start) / 1e9;
    printf("{\"decoder\":\"%s\",\"width\":%d,\"height\":%d,\"consumer\":\"%s\",\"max_images\":%d,"
           "\"seconds\":%.3f,\"queued\":%" PRId64 ",\"rendered\":%" PRId64 ",\"consumed\":%lld,"
           "\"consumer_errors\":%lld,\"loops\":%" PRId64 ",\"format_changes\":%" PRId64 ",\"try_again\":%" PRId64 ","
           "\"sessions\":%d,\"sessions_with_output\":%" PRId64 ",\"surface_switches\":%d,\"max_first_output_ms\":%.2f,"
           "\"output_fps\":%.2f,\"gap_ms\":{\"p50\":%.2f,\"p95\":%.2f,\"p99\":%.2f,\"max\":%.2f},"
           "\"stalled\":false,\"ms_since_output\":%.1f,\"ms_since_consumed\":%.1f}\n",
           codec_name ? codec_name : "?", video_width, video_height,
           consumer_mode == CONSUMER_GL ? "gl" : consumer_mode == CONSUMER_CPU ? "cpu" : "none", max_images, elapsed,
           queued, rendered, (long long)atomic_load(&consumed), (long long)atomic_load(&consumer_errors), loops,
           format_changes, try_again, sessions, sessions_with_output, switches, max_first_output_ns / 1e6,
           rendered / elapsed,
           gap_count ? gaps[gap_count / 2] / 1e6 : 0, gap_count ? gaps[gap_count * 95 / 100] / 1e6 : 0,
           gap_count ? gaps[gap_count * 99 / 100] / 1e6 : 0, gap_count ? gaps[gap_count - 1] / 1e6 : 0,
           (end - last_output) / 1e6,
           atomic_load(&last_consumed_ns) ? (now_ns() - atomic_load(&last_consumed_ns)) / 1e6 : -1.0);

    free(codec_name);
    AMediaFormat_delete(format);
    AMediaExtractor_delete(extractor);
    close(fd);
    free(gaps);
    pthread_cond_destroy(&watch.changed);
    pthread_mutex_destroy(&watch.mutex);
    return rendered && atomic_load(&consumed) && !atomic_load(&consumer_errors) ? 0 : 1;
}
