/* Velox Phase 19 Independent GLES 3.0/3.1 Benchmark Suite.
 * Builds for:
 *   - Android x86_64 & ARM64 (Host GLES 3.1 / GuestAngle GLES 3.2)
 *   - Windows ANGLE (D3D11 via VELOX_ANGLE_EGL)
 *   - Windows Native OpenGL 4.5 (compile with -DUSE_WGL, maps GLES 3.0/3.1 calls to core GL 4.3+)
 * All scenes verify exact pixel or SSBO FNV-1a 64-bit checksums.
 */
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#ifndef USE_WGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#else
#include <GL/gl.h>
#include <GLES3/gl31.h>
#endif

#if defined(_WIN32) && !defined(USE_WGL)
#define EGL_FUNCS(X) \
    X(eglGetDisplay, GETDISPLAY) X(eglInitialize, INITIALIZE) X(eglBindAPI, BINDAPI) \
    X(eglChooseConfig, CHOOSECONFIG) X(eglCreateContext, CREATECONTEXT) \
    X(eglCreatePbufferSurface, CREATEPBUFFERSURFACE) X(eglMakeCurrent, MAKECURRENT) \
    X(eglSwapBuffers, SWAPBUFFERS) X(eglDestroySurface, DESTROYSURFACE) \
    X(eglDestroyContext, DESTROYCONTEXT) X(eglTerminate, TERMINATE) X(eglGetError, GETERROR)
#define EGL_DECL(n, t) static PFNEGL##t##PROC p_##n;
EGL_FUNCS(EGL_DECL)
#define EGL(n) p_##n
#elif !defined(USE_WGL)
#define EGL(n) n
#endif

#define GL_CORE_FUNCS(X) \
    X(glCreateShader, CREATESHADER) X(glShaderSource, SHADERSOURCE) X(glCompileShader, COMPILESHADER) \
    X(glGetShaderiv, GETSHADERIV) X(glGetShaderInfoLog, GETSHADERINFOLOG) X(glDeleteShader, DELETESHADER) \
    X(glCreateProgram, CREATEPROGRAM) X(glAttachShader, ATTACHSHADER) X(glLinkProgram, LINKPROGRAM) \
    X(glGetProgramiv, GETPROGRAMIV) X(glGetProgramInfoLog, GETPROGRAMINFOLOG) X(glDeleteProgram, DELETEPROGRAM) \
    X(glUseProgram, USEPROGRAM) X(glGetUniformLocation, GETUNIFORMLOCATION) \
    X(glUniform1i, UNIFORM1I) X(glUniform4f, UNIFORM4F) \
    X(glGenBuffers, GENBUFFERS) X(glDeleteBuffers, DELETEBUFFERS) X(glBindBuffer, BINDBUFFER) \
    X(glBufferData, BUFFERDATA) X(glBufferSubData, BUFFERSUBDATA) \
    X(glMapBufferRange, MAPBUFFERRANGE) X(glUnmapBuffer, UNMAPBUFFER) \
    X(glVertexAttribPointer, VERTEXATTRIBPOINTER) X(glEnableVertexAttribArray, ENABLEVERTEXATTRIBARRAY) \
    X(glGenTextures, GENTEXTURES) X(glDeleteTextures, DELETETEXTURES) X(glBindTexture, BINDTEXTURE) \
    X(glTexParameteri, TEXPARAMETERI) X(glTexStorage2D, TEXSTORAGE2D) X(glTexSubImage2D, TEXSUBIMAGE2D) \
    X(glDisable, DISABLE) X(glViewport, VIEWPORT) X(glClearColor, CLEARCOLOR) X(glClear, CLEAR) \
    X(glDrawArrays, DRAWARRAYS) X(glDrawElements, DRAWELEMENTS) \
    X(glFenceSync, FENCESYNC) X(glClientWaitSync, CLIENTWAITSYNC) X(glDeleteSync, DELETESYNC) \
    X(glFlush, FLUSH) X(glFinish, FINISH) X(glReadPixels, READPIXELS) \
    X(glGetString, GETSTRING) X(glGetError, GETERROR)

#define GL_OPT_FUNCS(X) \
    X(glBindBufferBase, BINDBUFFERBASE) X(glDispatchCompute, DISPATCHCOMPUTE) X(glMemoryBarrier, MEMORYBARRIER) \
    X(glGenVertexArrays, GENVERTEXARRAYS) X(glBindVertexArray, BINDVERTEXARRAY) X(glDeleteVertexArrays, DELETEVERTEXARRAYS) \
    X(glGenFramebuffers, GENFRAMEBUFFERS) X(glBindFramebuffer, BINDFRAMEBUFFER) \
    X(glFramebufferTexture2D, FRAMEBUFFERTEXTURE2D) X(glCheckFramebufferStatus, CHECKFRAMEBUFFERSTATUS) \
    X(glDeleteFramebuffers, DELETEFRAMEBUFFERS)

#ifdef _WIN32
#define GL_DECL(n, t) static PFNGL##t##PROC p_##n;
GL_CORE_FUNCS(GL_DECL)
GL_OPT_FUNCS(GL_DECL)
#define GL(n) p_##n
#else
#define GL(n) n
#endif

enum { SURF_W = 512, SURF_H = 512, SURF_BYTES = SURF_W * SURF_H * 4 };
static uint8_t g_pixels[SURF_BYTES];
static uint8_t g_tex_pat[2][SURF_BYTES];
static uint8_t g_tex_small[256 * 256 * 4];
static int g_has_compute = 0;

static void die(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}

static double now_s(void) {
#ifdef _WIN32
    static double inv_freq = 0.0;
    if (inv_freq == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        inv_freq = 1.0 / (double)f.QuadPart;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * inv_freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static void sleep_until_s(double target_s) {
    for (;;) {
        double rem = target_s - now_s();
        if (rem <= 0.0) break;
#ifdef _WIN32
        if (rem > 0.002) Sleep((DWORD)((rem - 0.001) * 1000.0));
        else YieldProcessor();
#else
        if (rem > 0.0002) {
            struct timespec req;
            double s = rem - 0.0001;
            req.tv_sec = (time_t)s;
            req.tv_nsec = (long)((s - (double)req.tv_sec) * 1e9);
            nanosleep(&req, NULL);
        }
#endif
    }
}

static uint64_t fnv1a64(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

#ifdef USE_WGL
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int *);
static HWND g_hwnd;
static HDC g_hdc;
static HGLRC g_hglrc;
static GLuint g_fbo, g_fbo_tex, g_vao;

static void *wgl_load_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        HMODULE gl32 = GetModuleHandleA("opengl32.dll");
        if (gl32) p = (void *)GetProcAddress(gl32, name);
    }
    return p;
}

static void init_context(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "VeloxGlesBenchWgl";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA(wc.lpszClassName, "velox", WS_OVERLAPPEDWINDOW, 0, 0, SURF_W, SURF_H, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) die("CreateWindowA failed");
    g_hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32, 0,0,0,0,0,0, 8,0, 0,0,0,0,0, 24,8,0, PFD_MAIN_PLANE, 0,0,0,0 };
    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!SetPixelFormat(g_hdc, pf, &pfd)) die("SetPixelFormat failed");
    HGLRC tmp = wglCreateContext(g_hdc);
    if (!tmp || !wglMakeCurrent(g_hdc, tmp)) die("wglCreateContext failed");
    PFNWGLCREATECONTEXTATTRIBSARBPROC createAttribs = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (createAttribs) {
        int attrs[] = { 0x2091, 4, 0x2092, 3, 0x9126, 1, 0 }; /* 4.3 Core */
        g_hglrc = createAttribs(g_hdc, 0, attrs);
    }
    if (g_hglrc) {
        wglMakeCurrent(g_hdc, g_hglrc);
        wglDeleteContext(tmp);
    } else {
        g_hglrc = tmp;
    }
#define LOAD_C(n, t) p_##n = (PFNGL##t##PROC)wgl_load_proc(#n); if (!p_##n) die("missing " #n);
#define LOAD_O(n, t) p_##n = (PFNGL##t##PROC)wgl_load_proc(#n);
    GL_CORE_FUNCS(LOAD_C)
    GL_OPT_FUNCS(LOAD_O)
#undef LOAD_C
#undef LOAD_O
    g_has_compute = (p_glDispatchCompute != NULL && p_glBindBufferBase != NULL && p_glMemoryBarrier != NULL);
    /* Core profile needs VAO and offscreen FBO of exact 512x512 */
    GL(glGenVertexArrays)(1, &g_vao);
    GL(glBindVertexArray)(g_vao);
    GL(glGenTextures)(1, &g_fbo_tex);
    GL(glBindTexture)(GL_TEXTURE_2D, g_fbo_tex);
    GL(glTexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, SURF_W, SURF_H);
    GL(glGenFramebuffers)(1, &g_fbo);
    GL(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
    GL(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbo_tex, 0);
    if (GL(glCheckFramebufferStatus)(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("FBO incomplete");
}

static void swap_surface(void) {
    SwapBuffers(g_hdc);
}

static void destroy_context(void) {
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(g_hglrc);
    ReleaseDC(g_hwnd, g_hdc);
    DestroyWindow(g_hwnd);
}
#else
static EGLDisplay g_dpy;
static EGLContext g_ctx;
static EGLSurface g_surf;
static EGLConfig g_cfg;

static void init_context(void) {
#ifdef _WIN32
    wchar_t path[32768];
    DWORD len = GetEnvironmentVariableW(L"VELOX_ANGLE_EGL", path, 32768);
    if (!len || len >= 32768) die("Set VELOX_ANGLE_EGL");
    DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    HMODULE egl = LoadLibraryExW(path, NULL, flags);
    if (!egl) die("LoadLibrary libEGL.dll failed");
    wchar_t *base = wcsrchr(path, L'\\');
    if (!base) die("Invalid ANGLE path");
    memcpy(base + 1, L"libGLESv2.dll", sizeof(L"libGLESv2.dll"));
    HMODULE gl = LoadLibraryExW(path, NULL, flags);
    if (!gl) die("LoadLibrary libGLESv2.dll failed");
#define LOAD_E(n, t) p_##n = (PFNEGL##t##PROC)GetProcAddress(egl, #n); if (!p_##n) die("missing " #n);
#define LOAD_C(n, t) p_##n = (PFNGL##t##PROC)GetProcAddress(gl, #n); if (!p_##n) die("missing " #n);
#define LOAD_O(n, t) p_##n = (PFNGL##t##PROC)GetProcAddress(gl, #n);
    EGL_FUNCS(LOAD_E)
    GL_CORE_FUNCS(LOAD_C)
    GL_OPT_FUNCS(LOAD_O)
#undef LOAD_E
#undef LOAD_C
#undef LOAD_O
#endif
    g_dpy = EGL(eglGetDisplay)(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY) die("eglGetDisplay failed");
    EGLint maj = 0, min = 0;
    if (!EGL(eglInitialize)(g_dpy, &maj, &min)) die("eglInitialize failed");
    EGL(eglBindAPI)(EGL_OPENGL_ES_API);
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint ncfg = 0;
    if (!EGL(eglChooseConfig)(g_dpy, cfg_attr, &g_cfg, 1, &ncfg) || ncfg < 1) die("eglChooseConfig failed");
    EGLint surf_attr[] = { EGL_WIDTH, SURF_W, EGL_HEIGHT, SURF_H, EGL_NONE };
    g_surf = EGL(eglCreatePbufferSurface)(g_dpy, g_cfg, surf_attr);
    if (g_surf == EGL_NO_SURFACE) die("eglCreatePbufferSurface failed");
    EGLint ctx31[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
    EGLint ctx30[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    g_ctx = EGL(eglCreateContext)(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx31);
    if (g_ctx != EGL_NO_CONTEXT) {
        g_has_compute = 1;
    } else {
        g_ctx = EGL(eglCreateContext)(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx30);
        g_has_compute = 0;
    }
    if (g_ctx == EGL_NO_CONTEXT) die("eglCreateContext failed");
    if (!EGL(eglMakeCurrent)(g_dpy, g_surf, g_surf, g_ctx)) die("eglMakeCurrent failed");
#ifdef _WIN32
    if (!p_glDispatchCompute || !p_glBindBufferBase || !p_glMemoryBarrier) g_has_compute = 0;
#endif
}

static void swap_surface(void) {
    EGL(eglSwapBuffers)(g_dpy, g_surf);
}

static void destroy_context(void) {
    EGL(eglMakeCurrent)(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL(eglDestroyContext)(g_dpy, g_ctx);
    EGL(eglDestroySurface)(g_dpy, g_surf);
    EGL(eglTerminate)(g_dpy);
}
#endif

#ifdef USE_WGL
static const char *VS_SRC =
    "#version 430 core\n"
    "layout(location=0) in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main(){ uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS_SRC =
    "#version 430 core\n"
    "in vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 tint;\n"
    "out vec4 col;\n"
    "void main(){ col = texture(tex, uv) * tint; }\n";
static const char *CS_SRC =
    "#version 430 core\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Buf { uint data[]; };\n"
    "void main(){ uint i = gl_GlobalInvocationID.x; uint v = data[i]; v = (v * 1664525u + 1013904223u) ^ (i * 2654435761u); data[i] = v; }\n";
#else
static const char *VS_SRC =
    "#version 300 es\n"
    "layout(location=0) in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main(){ uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 tint;\n"
    "out vec4 col;\n"
    "void main(){ col = texture(tex, uv) * tint; }\n";
static const char *CS_SRC =
    "#version 310 es\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Buf { uint data[]; };\n"
    "void main(){ uint i = gl_GlobalInvocationID.x; uint v = data[i]; v = (v * 1664525u + 1013904223u) ^ (i * 2654435761u); data[i] = v; }\n";
#endif

static GLuint compile_shader(GLenum stage, const char *src) {
    GLuint s = GL(glCreateShader)(stage);
    GL(glShaderSource)(s, 1, &src, NULL);
    GL(glCompileShader)(s);
    GLint ok = 0;
    GL(glGetShaderiv)(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        GL(glGetShaderInfoLog)(s, sizeof(log), NULL, log);
        die(log);
    }
    return s;
}

static GLuint link_program(const char *vs, const char *fs) {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    GLuint p = GL(glCreateProgram)();
    GL(glAttachShader)(p, v);
    GL(glAttachShader)(p, f);
    GL(glLinkProgram)(p);
    GLint ok = 0;
    GL(glGetProgramiv)(p, GL_LINK_STATUS, &ok);
    GL(glDeleteShader)(v);
    GL(glDeleteShader)(f);
    if (!ok) {
        char log[1024] = {0};
        GL(glGetProgramInfoLog)(p, sizeof(log), NULL, log);
        die(log);
    }
    return p;
}

static void init_patterns(void) {
    for (int p = 0; p < 2; ++p) {
        for (int y = 0; y < SURF_H; ++y) {
            for (int x = 0; x < SURF_W; ++x) {
                size_t off = (size_t)(y * SURF_W + x) * 4;
                g_tex_pat[p][off + 0] = (uint8_t)((x + p * 64) & 255);
                g_tex_pat[p][off + 1] = (uint8_t)((y + p * 32) & 255);
                g_tex_pat[p][off + 2] = (uint8_t)(((x ^ y) + p * 17) & 255);
                g_tex_pat[p][off + 3] = 255;
            }
        }
    }
    for (int i = 0; i < 256 * 256; ++i) {
        g_tex_small[i * 4 + 0] = (uint8_t)(i & 255);
        g_tex_small[i * 4 + 1] = (uint8_t)((i >> 8) & 255);
        g_tex_small[i * 4 + 2] = (uint8_t)((i * 7) & 255);
        g_tex_small[i * 4 + 3] = 255;
    }
}

static void wait_fence(void) {
    GLsync sync = GL(glFenceSync)(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!sync) die("glFenceSync failed");
    GLenum res = GL(glClientWaitSync)(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ULL);
    GL(glDeleteSync)(sync);
    if (res != GL_ALREADY_SIGNALED && res != GL_CONDITION_SATISFIED) die("glClientWaitSync failed");
}

static void print_json_str(const char *s) {
    putchar('"');
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') putchar('\\');
        if ((unsigned char)*s >= 0x20) putchar(*s);
    }
    putchar('"');
}

/* Multi-context thread test (Android / Windows ANGLE only) */
#ifndef USE_WGL
typedef struct {
    int tid;
    int frames;
    uint64_t checksum;
    double elapsed_s;
    int ok;
} MtArg;

#ifdef _WIN32
static DWORD WINAPI mt_worker(LPVOID arg)
#else
static void *mt_worker(void *arg)
#endif
{
    MtArg *a = (MtArg *)arg;
    EGLint sa[] = { EGL_WIDTH, 128, EGL_HEIGHT, 128, EGL_NONE };
    EGLSurface s = EGL(eglCreatePbufferSurface)(g_dpy, g_cfg, sa);
    EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext c = EGL(eglCreateContext)(g_dpy, g_cfg, EGL_NO_CONTEXT, ca);
    if (s == EGL_NO_SURFACE || c == EGL_NO_CONTEXT || !EGL(eglMakeCurrent)(g_dpy, s, s, c)) {
        a->ok = 0;
        return 0;
    }
    GLuint prog = link_program(VS_SRC, FS_SRC);
    GL(glUseProgram)(prog);
    GLuint vbo = 0, tex = 0;
    GLfloat tri[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
    GL(glGenBuffers)(1, &vbo);
    GL(glBindBuffer)(GL_ARRAY_BUFFER, vbo);
    GL(glBufferData)(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    GL(glEnableVertexAttribArray)(0);
    GL(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    GL(glGenTextures)(1, &tex);
    GL(glBindTexture)(GL_TEXTURE_2D, tex);
    GL(glTexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, 128, 128);
    uint8_t pat[128 * 128 * 4];
    for (int i = 0; i < 128 * 128 * 4; i += 4) {
        pat[i] = (uint8_t)(a->tid * 80 + 40); pat[i + 1] = 120; pat[i + 2] = 200; pat[i + 3] = 255;
    }
    GL(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, pat);
    GL(glUniform1i)(GL(glGetUniformLocation)(prog, "tex"), 0);
    GL(glUniform4f)(GL(glGetUniformLocation)(prog, "tint"), 1.f, 1.f, 1.f, 1.f);
    GL(glViewport)(0, 0, 128, 128);
    double t0 = now_s();
    for (int f = 0; f < a->frames; ++f) {
        GL(glClear)(GL_COLOR_BUFFER_BIT);
        for (int d = 0; d < 200; ++d) GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
        wait_fence();
    }
    a->elapsed_s = now_s() - t0;
    uint8_t out[128 * 128 * 4];
    GL(glReadPixels)(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, out);
    a->checksum = fnv1a64(out, sizeof(out));
    a->ok = (GL(glGetError)() == GL_NO_ERROR);
    GL(glDeleteTextures)(1, &tex);
    GL(glDeleteBuffers)(1, &vbo);
    GL(glDeleteProgram)(prog);
    EGL(eglMakeCurrent)(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL(eglDestroyContext)(g_dpy, c);
    EGL(eglDestroySurface)(g_dpy, s);
    return 0;
}
#endif

int main(int argc, char **argv) {
    const char *scene = "all";
    double duration = 2.0;
    int pace_fps = 0;
    int frames_override = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pace-fps") && i + 1 < argc) pace_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames_override = atoi(argv[++i]);
    }

    init_patterns();
    init_context();

    const char *vendor = (const char *)GL(glGetString)(GL_VENDOR);
    const char *renderer = (const char *)GL(glGetString)(GL_RENDERER);
    const char *version = (const char *)GL(glGetString)(GL_VERSION);

    GLuint prog = link_program(VS_SRC, FS_SRC);
    GL(glUseProgram)(prog);
    GLint loc_tex = GL(glGetUniformLocation)(prog, "tex");
    GLint loc_tint = GL(glGetUniformLocation)(prog, "tint");
    GL(glUniform1i)(loc_tex, 0);
    GL(glUniform4f)(loc_tint, 1.f, 1.f, 1.f, 1.f);

    /* Main VBO (triangle + 4 KiB dynamic buffer) */
    GLfloat tri_buf[1024];
    memset(tri_buf, 0, sizeof(tri_buf));
    tri_buf[0] = -1.f; tri_buf[1] = -1.f;
    tri_buf[2] =  3.f; tri_buf[3] = -1.f;
    tri_buf[4] = -1.f; tri_buf[5] =  3.f;

    GLuint vbo = 0, mesh_vbo = 0, mesh_ibo = 0, tex = 0, tex_small = 0;
    GL(glGenBuffers)(1, &vbo);
    GL(glBindBuffer)(GL_ARRAY_BUFFER, vbo);
    GL(glBufferData)(GL_ARRAY_BUFFER, sizeof(tri_buf), tri_buf, GL_DYNAMIC_DRAW);
    GL(glEnableVertexAttribArray)(0);
    GL(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    /* High-vertex mesh: 65,536 vertices (grid triangles) */
    const int MESH_VERTS = 65536;
    GLfloat *mv = (GLfloat *)malloc((size_t)MESH_VERTS * 2 * sizeof(GLfloat));
    uint32_t *mi = (uint32_t *)malloc((size_t)MESH_VERTS * sizeof(uint32_t));
    for (int k = 0; k < MESH_VERTS; ++k) {
        int gx = k & 255, gy = k >> 8;
        mv[k * 2 + 0] = (float)gx / 128.f - 1.f;
        mv[k * 2 + 1] = (float)gy / 128.f - 1.f;
        mi[k] = (uint32_t)k;
    }
    GL(glGenBuffers)(1, &mesh_vbo);
    GL(glBindBuffer)(GL_ARRAY_BUFFER, mesh_vbo);
    GL(glBufferData)(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)MESH_VERTS * 2 * sizeof(GLfloat)), mv, GL_STATIC_DRAW);
    GL(glGenBuffers)(1, &mesh_ibo);
    GL(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, mesh_ibo);
    GL(glBufferData)(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)MESH_VERTS * sizeof(uint32_t)), mi, GL_STATIC_DRAW);
    free(mv); free(mi);

    GL(glGenTextures)(1, &tex);
    GL(glBindTexture)(GL_TEXTURE_2D, tex);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL(glTexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, SURF_W, SURF_H);
    GL(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, SURF_W, SURF_H, GL_RGBA, GL_UNSIGNED_BYTE, g_tex_pat[0]);

    GL(glGenTextures)(1, &tex_small);
    GL(glBindTexture)(GL_TEXTURE_2D, tex_small);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL(glTexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, 256, 256);
    GL(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, g_tex_small);

    GL(glBindTexture)(GL_TEXTURE_2D, tex);
    GL(glBindBuffer)(GL_ARRAY_BUFFER, vbo);
    GL(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    GL(glDisable)(GL_DEPTH_TEST);
    GL(glDisable)(GL_BLEND);
    GL(glClearColor)(0.f, 0.f, 0.f, 1.f);

    printf("{\n  \"backend\": \"%s\",\n  \"vendor\": ",
#ifdef USE_WGL
        "windows_wgl_native"
#elif defined(_WIN32)
        "windows_angle"
#else
        "android_gles"
#endif
    );
    print_json_str(vendor);
    printf(",\n  \"renderer\": ");
    print_json_str(renderer);
    printf(",\n  \"version\": ");
    print_json_str(version);
    printf(",\n  \"has_compute\": %s,\n  \"results\": {\n", g_has_compute ? "true" : "false");

    const char *all_scenes[] = {
        "draws", "uniform", "vertex", "buffer_subdata", "map_buffer",
        "tex_upload", "fill", "sync_fence", "sync_finish", "sync_flush",
        "swap", "shader_compile", "compute", "readback", "game_paced", "mt_ctx"
    };
    int n_scenes = (int)(sizeof(all_scenes) / sizeof(all_scenes[0]));
    int printed = 0;

    for (int si = 0; si < n_scenes; ++si) {
        const char *sc = all_scenes[si];
        if (strcmp(scene, "all") != 0 && strcmp(scene, sc) != 0) continue;
        if (printed++) printf(",\n");

        /* Reset standard bindings */
        GL(glUseProgram)(prog);
        GL(glBindTexture)(GL_TEXTURE_2D, tex);
        GL(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, SURF_W, SURF_H, GL_RGBA, GL_UNSIGNED_BYTE, g_tex_pat[0]);
        GL(glBindBuffer)(GL_ARRAY_BUFFER, vbo);
        GL(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        GL(glUniform4f)(loc_tint, 1.f, 1.f, 1.f, 1.f);
        GL(glViewport)(0, 0, SURF_W, SURF_H);
        wait_fence();

        if (!strcmp(sc, "shader_compile")) {
            int reps = 20;
            double t0 = now_s();
            for (int r = 0; r < reps; ++r) {
                char fs_var[512];
                snprintf(fs_var, sizeof(fs_var),
#ifdef USE_WGL
                    "#version 430 core\nin vec2 uv; uniform sampler2D tex; uniform vec4 tint; out vec4 col;\n"
                    "void main(){ col = texture(tex, uv) * tint + vec4(%d.0 * 0.0001); }\n", r
#else
                    "#version 300 es\nprecision mediump float; in vec2 uv; uniform sampler2D tex; uniform vec4 tint; out vec4 col;\n"
                    "void main(){ col = texture(tex, uv) * tint + vec4(%d.0 * 0.0001); }\n", r
#endif
                );
                GLuint p2 = link_program(VS_SRC, fs_var);
                GL(glDeleteProgram)(p2);
            }
            double dt = now_s() - t0;
            printf("    \"shader_compile\": {\"programs\": %d, \"total_ms\": %.3f, \"ms_per_program\": %.3f}",
                   reps, dt * 1000.0, (dt * 1000.0) / reps);
            continue;
        }

        if (!strcmp(sc, "compute")) {
            if (!g_has_compute) {
                printf("    \"compute\": {\"skipped\": true}");
                continue;
            }
            const int N = 65536;
            uint32_t *init_data = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
            uint32_t *expected = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
            for (int k = 0; k < N; ++k) init_data[k] = (uint32_t)(k * 3 + 1);
            GLuint cs = compile_shader(GL_COMPUTE_SHADER, CS_SRC);
            GLuint cp = GL(glCreateProgram)();
            GL(glAttachShader)(cp, cs);
            GL(glLinkProgram)(cp);
            GL(glDeleteShader)(cs);
            GLuint ssbo = 0;
            GL(glGenBuffers)(1, &ssbo);
            GL(glBindBuffer)(GL_SHADER_STORAGE_BUFFER, ssbo);
            GL(glBufferData)(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((size_t)N * sizeof(uint32_t)), init_data, GL_DYNAMIC_COPY);
            GL(glBindBufferBase)(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
            GL(glUseProgram)(cp);
            wait_fence();
            int dispatches = 0;
            double t0 = now_s();
            while (now_s() - t0 < duration || dispatches < 50) {
                GL(glDispatchCompute)((GLuint)(N / 64), 1, 1);
                GL(glMemoryBarrier)(GL_SHADER_STORAGE_BARRIER_BIT);
                wait_fence();
                dispatches++;
                if (now_s() - t0 >= duration && dispatches >= 50) break;
            }
            double dt = now_s() - t0;
            /* Verify exact CPU reference */
            memcpy(expected, init_data, (size_t)N * sizeof(uint32_t));
            for (int d = 0; d < dispatches; ++d) {
                for (int k = 0; k < N; ++k) {
                    uint32_t v = expected[k];
                    expected[k] = (v * 1664525u + 1013904223u) ^ ((uint32_t)k * 2654435761u);
                }
            }
            void *mapped = GL(glMapBufferRange)(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)((size_t)N * sizeof(uint32_t)), GL_MAP_READ_BIT);
            uint64_t got_h = mapped ? fnv1a64(mapped, (size_t)N * sizeof(uint32_t)) : 0;
            uint64_t exp_h = fnv1a64(expected, (size_t)N * sizeof(uint32_t));
            if (mapped) GL(glUnmapBuffer)(GL_SHADER_STORAGE_BUFFER);
            GL(glDeleteBuffers)(1, &ssbo);
            GL(glDeleteProgram)(cp);
            free(init_data); free(expected);
            printf("    \"compute\": {\"dispatches\": %d, \"dispatches_per_s\": %.2f, \"checksum_ok\": %s, \"checksum\": \"%016" PRIx64 "\"}",
                   dispatches, (double)dispatches / dt, got_h == exp_h ? "true" : "false", got_h);
            continue;
        }

        if (!strcmp(sc, "mt_ctx")) {
#ifdef USE_WGL
            printf("    \"mt_ctx\": {\"skipped\": true, \"reason\": \"wgl_single_ctx\"}");
#else
            MtArg a[2] = { {0, 120, 0, 0.0, 0}, {1, 120, 0, 0.0, 0} };
            double t0 = now_s();
#ifdef _WIN32
            HANDLE th[2];
            th[0] = CreateThread(NULL, 0, mt_worker, &a[0], 0, NULL);
            th[1] = CreateThread(NULL, 0, mt_worker, &a[1], 0, NULL);
            WaitForMultipleObjects(2, th, TRUE, INFINITE);
            CloseHandle(th[0]); CloseHandle(th[1]);
#else
            pthread_t th[2];
            pthread_create(&th[0], NULL, mt_worker, &a[0]);
            pthread_create(&th[1], NULL, mt_worker, &a[1]);
            pthread_join(th[0], NULL); pthread_join(th[1], NULL);
#endif
            double dt = now_s() - t0;
            printf("    \"mt_ctx\": {\"threads\": 2, \"frames_per_thread\": 120, \"wall_s\": %.3f, "
                   "\"agg_fps\": %.2f, \"ok\": %s, \"c0\": \"%016" PRIx64 "\", \"c1\": \"%016" PRIx64 "\"}",
                   dt, 240.0 / dt, (a[0].ok && a[1].ok) ? "true" : "false", a[0].checksum, a[1].checksum);
#endif
            continue;
        }

        /* Frame-based scenes */
        int cap = 100000;
        double *ft_ms = (double *)malloc((size_t)cap * sizeof(double));
        int frames = 0;
        double enc_acc = 0.0, upl_acc = 0.0, wait_acc = 0.0, swap_acc = 0.0;
        int is_paced = (!strcmp(sc, "game_paced") && pace_fps > 0);
        int target_frames = frames_override > 0 ? frames_override : (is_paced ? pace_fps * 3 : 0);

        /* Warmup 3 frames */
        for (int w = 0; w < 3; ++w) {
            GL(glClear)(GL_COLOR_BUFFER_BIT);
            GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
            wait_fence();
        }

        double t_start = now_s();
        double next_deadline = t_start;
        double frame_interval = is_paced ? (1.0 / (double)pace_fps) : 0.0;

        for (;;) {
            double f0 = now_s();
            if (!strcmp(sc, "draws")) {
                GL(glViewport)(0, 0, 16, 16);
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                for (int d = 0; d < 20000; ++d) GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                double f1 = now_s();
                wait_fence();
                enc_acc += (f1 - f0);
                wait_acc += (now_s() - f1);
            } else if (!strcmp(sc, "uniform")) {
                GL(glViewport)(0, 0, 16, 16);
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                for (int d = 0; d < 20000; ++d) {
                    GL(glUniform4f)(loc_tint, (float)(d & 1), (float)(1 - (d & 1)), 0.f, 1.f);
                    GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                }
                double f1 = now_s();
                wait_fence();
                enc_acc += (f1 - f0);
                wait_acc += (now_s() - f1);
            } else if (!strcmp(sc, "vertex")) {
                GL(glBindBuffer)(GL_ARRAY_BUFFER, mesh_vbo);
                GL(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
                GL(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, mesh_ibo);
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                for (int d = 0; d < 16; ++d) GL(glDrawElements)(GL_TRIANGLES, 65532, GL_UNSIGNED_INT, 0);
                wait_fence();
            } else if (!strcmp(sc, "buffer_subdata")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                for (int d = 0; d < 64; ++d) {
                    tri_buf[6] = (float)(frames + d);
                    GL(glBufferSubData)(GL_ARRAY_BUFFER, 0, sizeof(tri_buf), tri_buf);
                    GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                }
                wait_fence();
            } else if (!strcmp(sc, "map_buffer")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                for (int d = 0; d < 64; ++d) {
                    void *mp = GL(glMapBufferRange)(GL_ARRAY_BUFFER, 0, sizeof(tri_buf), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
                    if (mp) {
                        memcpy(mp, tri_buf, sizeof(tri_buf));
                        GL(glUnmapBuffer)(GL_ARRAY_BUFFER);
                    }
                    GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                }
                wait_fence();
            } else if (!strcmp(sc, "tex_upload")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                for (int u = 0; u < 8; ++u) {
                    GL(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, SURF_W, SURF_H, GL_RGBA, GL_UNSIGNED_BYTE, g_tex_pat[(frames + u) & 1]);
                }
                GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                wait_fence();
            } else if (!strcmp(sc, "fill")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                for (int d = 0; d < 8; ++d) GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                wait_fence();
            } else if (!strcmp(sc, "sync_fence")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                wait_fence();
            } else if (!strcmp(sc, "sync_finish")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                GL(glFinish)();
            } else if (!strcmp(sc, "sync_flush")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                GL(glFlush)();
                if ((frames & 15) == 15) wait_fence();
            } else if (!strcmp(sc, "swap")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                swap_surface();
            } else if (!strcmp(sc, "readback")) {
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                GL(glReadPixels)(0, 0, SURF_W, SURF_H, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
            } else if (!strcmp(sc, "game_paced")) {
                double u0 = now_s();
                GL(glBindTexture)(GL_TEXTURE_2D, tex_small);
                GL(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, g_tex_small);
                GL(glBufferSubData)(GL_ARRAY_BUFFER, 0, sizeof(tri_buf), tri_buf);
                double u1 = now_s();
                GL(glClear)(GL_COLOR_BUFFER_BIT);
                for (int d = 0; d < 1000; ++d) {
                    if ((d & 63) == 0) GL(glUniform4f)(loc_tint, 1.f, 1.f, 1.f, 1.f);
                    GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
                }
                double e1 = now_s();
                wait_fence();
                double w1 = now_s();
                swap_surface();
                double s1 = now_s();
                upl_acc += (u1 - u0);
                enc_acc += (e1 - u1);
                wait_acc += (w1 - e1);
                swap_acc += (s1 - w1);
                if (is_paced) {
                    next_deadline += frame_interval;
                    sleep_until_s(next_deadline);
                }
            }
            double f_end = now_s();
            if (frames < cap) ft_ms[frames] = (f_end - f0) * 1000.0;
            frames++;
            if (target_frames > 0) {
                if (frames >= target_frames) break;
            } else {
                if (f_end - t_start >= duration && frames >= 10) break;
            }
        }
        double total_s = now_s() - t_start;
        GL(glFinish)();
        GL(glReadPixels)(0, 0, SURF_W, SURF_H, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
        uint64_t csum = fnv1a64(g_pixels, SURF_BYTES);

        int n_rec = frames < cap ? frames : cap;
        qsort(ft_ms, (size_t)n_rec, sizeof(double), cmp_double);
        double p50 = ft_ms[n_rec / 2];
        double p95 = ft_ms[(int)(n_rec * 0.95) < n_rec ? (int)(n_rec * 0.95) : n_rec - 1];
        double p99 = ft_ms[(int)(n_rec * 0.99) < n_rec ? (int)(n_rec * 0.99) : n_rec - 1];
        double p999 = ft_ms[(int)(n_rec * 0.999) < n_rec ? (int)(n_rec * 0.999) : n_rec - 1];
        int top1_start = (int)(n_rec * 0.99);
        double top1_sum = 0.0;
        for (int k = top1_start; k < n_rec; ++k) top1_sum += ft_ms[k];
        double one_pct_low = 1000.0 / (top1_sum / (double)(n_rec - top1_start));

        printf("    \"%s\": {\"frames\": %d, \"seconds\": %.4f, \"fps\": %.2f, \"one_pct_low\": %.2f, "
               "\"p50_ms\": %.4f, \"p95_ms\": %.4f, \"p99_ms\": %.4f, \"p999_ms\": %.4f, "
               "\"enc_us\": %.1f, \"upl_us\": %.1f, \"wait_us\": %.1f, \"swap_us\": %.1f, \"checksum\": \"%016" PRIx64 "\"}",
               sc, frames, total_s, (double)frames / total_s, one_pct_low, p50, p95, p99, p999,
               (enc_acc * 1e6) / frames, (upl_acc * 1e6) / frames, (wait_acc * 1e6) / frames, (swap_acc * 1e6) / frames, csum);
        free(ft_ms);
    }

    printf("\n  }\n}\n");
    destroy_context();
    return 0;
}
