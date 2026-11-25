/* Windows ANGLE と Android で同じ GLES 3.0 描画・アップロードを測る。 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define EGL_FUNCTIONS(X) \
    X(eglGetDisplay, GETDISPLAY) X(eglInitialize, INITIALIZE) X(eglBindAPI, BINDAPI) \
    X(eglChooseConfig, CHOOSECONFIG) X(eglCreateContext, CREATECONTEXT) \
    X(eglCreatePbufferSurface, CREATEPBUFFERSURFACE) X(eglMakeCurrent, MAKECURRENT) \
    X(eglSwapBuffers, SWAPBUFFERS) X(eglDestroySurface, DESTROYSURFACE) \
    X(eglDestroyContext, DESTROYCONTEXT) X(eglTerminate, TERMINATE) X(eglGetError, GETERROR)
#define GL_FUNCTIONS(X) \
    X(glCreateShader, CREATESHADER) X(glShaderSource, SHADERSOURCE) X(glCompileShader, COMPILESHADER) \
    X(glGetShaderiv, GETSHADERIV) X(glGetShaderInfoLog, GETSHADERINFOLOG) X(glDeleteShader, DELETESHADER) \
    X(glCreateProgram, CREATEPROGRAM) X(glAttachShader, ATTACHSHADER) X(glLinkProgram, LINKPROGRAM) \
    X(glGetProgramiv, GETPROGRAMIV) X(glGetProgramInfoLog, GETPROGRAMINFOLOG) X(glUseProgram, USEPROGRAM) \
    X(glGetUniformLocation, GETUNIFORMLOCATION) X(glUniform1i, UNIFORM1I) X(glUniform4f, UNIFORM4F) \
    X(glGenBuffers, GENBUFFERS) X(glBindBuffer, BINDBUFFER) X(glBufferData, BUFFERDATA) \
    X(glBufferSubData, BUFFERSUBDATA) X(glVertexAttribPointer, VERTEXATTRIBPOINTER) \
    X(glEnableVertexAttribArray, ENABLEVERTEXATTRIBARRAY) X(glGenTextures, GENTEXTURES) \
    X(glBindTexture, BINDTEXTURE) X(glTexParameteri, TEXPARAMETERI) X(glTexStorage2D, TEXSTORAGE2D) \
    X(glTexSubImage2D, TEXSUBIMAGE2D) X(glMapBufferRange, MAPBUFFERRANGE) X(glUnmapBuffer, UNMAPBUFFER) \
    X(glDisable, DISABLE) X(glViewport, VIEWPORT) X(glClearColor, CLEARCOLOR) X(glClear, CLEAR) \
    X(glDrawArrays, DRAWARRAYS) X(glFenceSync, FENCESYNC) X(glClientWaitSync, CLIENTWAITSYNC) \
    X(glDeleteSync, DELETESYNC) X(glReadPixels, READPIXELS) X(glGetString, GETSTRING) X(glGetError, GETERROR)

#ifdef _WIN32
#define EGL_DECLARE(name, type) static PFNEGL##type##PROC p_##name;
#define GL_DECLARE(name, type) static PFNGL##type##PROC p_##name;
EGL_FUNCTIONS(EGL_DECLARE)
GL_FUNCTIONS(GL_DECLARE)
#define EGL(name) p_##name
#define GL(name) p_##name
#else
#define EGL(name) name
#define GL(name) name
#endif

enum { SURFACE_SIZE = 512, BYTES = SURFACE_SIZE * SURFACE_SIZE * 4, DRAWS = 20000 };
typedef enum { DRAW, UNIFORM, BUFFER, UPLOAD, FILL, SYNC, SWAP } Scene;
static const char *const scene_names[] = {"draws", "uniform", "buffer", "upload", "fill", "sync", "swap"};
static const uint8_t red[] = {255, 0, 0, 255}, black[] = {0, 0, 0, 255};
static uint8_t pixels[BYTES], pattern[2][BYTES];
static GLfloat vertices[1024] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
static GLint tint;

_Noreturn static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void load_apis(void) {
#ifdef _WIN32
    wchar_t path[32768];
    const DWORD length = GetEnvironmentVariableW(L"VELOX_ANGLE_EGL", path, sizeof(path) / sizeof(*path));
    if (!length || length >= sizeof(path) / sizeof(*path)) fail("Set VELOX_ANGLE_EGL to the absolute ANGLE libEGL.dll path");
    const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    HMODULE egl = LoadLibraryExW(path, NULL, flags);
    if (!egl) fail("Failed to load ANGLE libEGL.dll");
    wchar_t *basename = wcsrchr(path, L'\\');
    if (!basename || (size_t)(basename - path) + 15 >= sizeof(path) / sizeof(*path)) fail("Invalid ANGLE DLL path");
    memcpy(basename + 1, L"libGLESv2.dll", sizeof(L"libGLESv2.dll"));
    HMODULE gl = LoadLibraryExW(path, NULL, flags);
    if (!gl) fail("Failed to load ANGLE libGLESv2.dll");
#define EGL_LOAD(name, type) p_##name = (PFNEGL##type##PROC)GetProcAddress(egl, #name); if (!p_##name) fail(#name);
#define GL_LOAD(name, type) p_##name = (PFNGL##type##PROC)GetProcAddress(gl, #name); if (!p_##name) fail(#name);
    EGL_FUNCTIONS(EGL_LOAD)
    GL_FUNCTIONS(GL_LOAD)
#undef EGL_LOAD
#undef GL_LOAD
#endif
}

static double wall_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) fail("CLOCK_MONOTONIC failed");
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
#endif
}

static GLuint compile(GLenum stage, const char *source) {
    GLuint shader = GL(glCreateShader)(stage);
    GL(glShaderSource)(shader, 1, &source, NULL);
    GL(glCompileShader)(shader);
    GLint ok;
    GL(glGetShaderiv)(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        GL(glGetShaderInfoLog)(shader, sizeof(log), NULL, log);
        fail(log);
    }
    return shader;
}

static void frame(Scene scene, unsigned iteration) {
    GL(glClear)(GL_COLOR_BUFFER_BIT);
    if (scene == UPLOAD) {
        for (unsigned i = 0; i < 8; ++i)
            GL(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, SURFACE_SIZE, SURFACE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE,
                                pattern[(iteration + i) % 2]);
    }
    const unsigned count = scene == DRAW || scene == UNIFORM ? DRAWS : scene == BUFFER ? 64 : scene == FILL ? 8 : 1;
    for (unsigned i = 0; i < count; ++i) {
        if (scene == UNIFORM) GL(glUniform4f)(tint, (float)(i & 1), (float)(1 - (i & 1)), 0, 1);
        if (scene == BUFFER) {
            vertices[0] = -1.0f - (float)((iteration + i) & 1);
            GL(glBufferSubData)(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        }
        GL(glDrawArrays)(GL_TRIANGLES, 0, 3);
    }
}

static void wait_fence(GLsync fence) {
    const GLenum status = GL(glClientWaitSync)(fence, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_C(10000000000));
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) fail("glClientWaitSync failed");
    GL(glDeleteSync)(fence);
}

static void print_json_string(const unsigned char *value) {
    putchar('"');
    for (; *value; ++value) {
        if (*value == '"' || *value == '\\') putchar('\\');
        if (*value < 32) printf("\\u%04x", (unsigned)*value);
        else putchar(*value);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    if (argc != 3) fail("usage: gles-render draws|uniform|buffer|upload|fill|sync|swap seconds");
    Scene scene = DRAW;
    const unsigned scenes = sizeof(scene_names) / sizeof(*scene_names);
    while ((unsigned)scene < scenes && strcmp(argv[1], scene_names[scene])) scene = (Scene)(scene + 1);
    char *end;
    const double seconds = strtod(argv[2], &end);
    if ((unsigned)scene == scenes || *end || !(seconds > 0 && seconds <= 120)) fail("Invalid scene or seconds");
    load_apis();

    EGLDisplay display = EGL(eglGetDisplay)(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !EGL(eglInitialize)(display, NULL, NULL) ||
        !EGL(eglBindAPI)(EGL_OPENGL_ES_API)) fail("EGL initialization failed");
    const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint configs;
    if (!EGL(eglChooseConfig)(display, config_attrs, &config, 1, &configs) || !configs) fail("No RGBA8 EGL config");
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint surf_attrs[] = {EGL_WIDTH, SURFACE_SIZE, EGL_HEIGHT, SURFACE_SIZE, EGL_NONE};
    EGLContext context = EGL(eglCreateContext)(display, config, EGL_NO_CONTEXT, ctx_attrs);
    EGLSurface surface = EGL(eglCreatePbufferSurface)(display, config, surf_attrs);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !EGL(eglMakeCurrent)(display, surface, surface, context)) {
        fprintf(stderr, "GLES 3 context failed: 0x%x\n", (unsigned)EGL(eglGetError)());
        return 1;
    }
    const char *vs = "#version 300 es\nlayout(location=0) in vec2 pos;"
                     "void main(){gl_Position=vec4(pos,0.0,1.0);}";
    const char *fs = "#version 300 es\nprecision highp float; uniform vec4 tint; uniform highp sampler2D tex;"
                     "uniform int textured; out vec4 color; void main(){"
                     "color=textured!=0?texelFetch(tex,ivec2(gl_FragCoord.xy),0):tint;}";
    GLuint program = GL(glCreateProgram)(), vertex = compile(GL_VERTEX_SHADER, vs), fragment = compile(GL_FRAGMENT_SHADER, fs);
    GL(glAttachShader)(program, vertex);
    GL(glAttachShader)(program, fragment);
    GL(glDeleteShader)(vertex);
    GL(glDeleteShader)(fragment);
    GL(glLinkProgram)(program);
    GLint linked;
    GL(glGetProgramiv)(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {0};
        GL(glGetProgramInfoLog)(program, sizeof(log), NULL, log);
        fail(log);
    }
    GL(glUseProgram)(program);
    tint = GL(glGetUniformLocation)(program, "tint");
    GL(glUniform4f)(tint, 1, 0, 0, 1);
    const int textured = scene == FILL || scene == UPLOAD;
    GL(glUniform1i)(GL(glGetUniformLocation)(program, "textured"), textured);
    GL(glUniform1i)(GL(glGetUniformLocation)(program, "tex"), 0);
    GLuint buffer, texture;
    GL(glGenBuffers)(1, &buffer);
    GL(glBindBuffer)(GL_ARRAY_BUFFER, buffer);
    GL(glBufferData)(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    GL(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    GL(glEnableVertexAttribArray)(0);
    for (unsigned i = 0; i < SURFACE_SIZE * SURFACE_SIZE; ++i) {
        const uint8_t rgba[] = {(uint8_t)i, (uint8_t)(i >> 9), (uint8_t)(i >> 5), 255};
        memcpy(pattern[1] + i * 4, rgba, 4);
        memcpy(pattern[0] + i * 4, black, 4);
    }
    GL(glGenTextures)(1, &texture);
    GL(glBindTexture)(GL_TEXTURE_2D, texture);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL(glTexStorage2D)(GL_TEXTURE_2D, 1, GL_RGBA8, SURFACE_SIZE, SURFACE_SIZE);
    GL(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, SURFACE_SIZE, SURFACE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pattern[1]);
    GL(glDisable)(GL_DITHER);
    GL(glClearColor)(0, 0, 0, 1);
    const GLint viewport = scene == DRAW || scene == UNIFORM ? 4 : SURFACE_SIZE;
    GL(glViewport)(0, 0, viewport, viewport);

    for (unsigned i = 0; i < 20; ++i) frame(scene, i);
    wait_fence(GL(glFenceSync)(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    GLsync fences[2] = {0};
    const unsigned depth = scene == SYNC ? 1 : 2;
    const double start = wall_seconds();
    unsigned frames = 0;
    do {
        unsigned slot = frames % depth;
        if (fences[slot]) wait_fence(fences[slot]);
        frame(scene, frames);
        fences[slot] = GL(glFenceSync)(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fences[slot]) fail("glFenceSync failed");
        if (scene == SWAP && !EGL(eglSwapBuffers)(display, surface)) fail("eglSwapBuffers(pbuffer) failed");
        ++frames;
    } while (wall_seconds() - start < seconds);
    for (unsigned i = 0; i < depth; ++i) if (fences[i]) wait_fence(fences[i]);
    const double elapsed = wall_seconds() - start;

    frame(scene, 0);
    GL(glReadPixels)(0, 0, SURFACE_SIZE, SURFACE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (scene == BUFFER) {
        const void *mapped = GL(glMapBufferRange)(GL_ARRAY_BUFFER, 0, sizeof(vertices), GL_MAP_READ_BIT);
        if (!mapped || memcmp(mapped, vertices, sizeof(vertices)) || !GL(glUnmapBuffer)(GL_ARRAY_BUFFER))
            fail("Dynamic vertex buffer readback failed");
    }
    if (GL(glGetError)() != GL_NO_ERROR) fail("GLES drawing or readback failed");
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < SURFACE_SIZE * SURFACE_SIZE; ++i) {
        const uint8_t *expected = textured ? pattern[1] + i * 4 :
            i % SURFACE_SIZE < (unsigned)viewport && i / SURFACE_SIZE < (unsigned)viewport ? red : black;
        if (memcmp(pixels + i * 4, expected, 4)) {
            fprintf(stderr, "Incorrect pixel at %u\n", i);
            return 1;
        }
        for (unsigned j = 0; j < 4; ++j) hash = (hash ^ pixels[i * 4 + j]) * UINT64_C(1099511628211);
    }
    printf("{\"scene\":\"%s\",\"width\":%d,\"height\":%d,\"frames\":%u,\"fps\":%.2f,"
           "\"checksum\":\"%016" PRIx64 "\",\"renderer\":", scene_names[scene], SURFACE_SIZE, SURFACE_SIZE, frames, frames / elapsed, hash);
    print_json_string(GL(glGetString)(GL_RENDERER));
    printf(",\"version\":");
    print_json_string(GL(glGetString)(GL_VERSION));
    printf("}\n");
    EGL(eglMakeCurrent)(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGL(eglDestroySurface)(display, surface);
    EGL(eglDestroyContext)(display, context);
    EGL(eglTerminate)(display);
    return 0;
}
