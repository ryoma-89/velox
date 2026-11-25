/* Android EGL/GLES 3.2 smoke test with an exact framebuffer readback. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn static void egl_fail(const char *operation) {
    fprintf(stderr, "%s: EGL error 0x%x\n", operation, eglGetError());
    exit(1);
}

static GLuint compile(GLenum stage, const char *source) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024] = {0};
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "GLSL ES 3.20 compilation failed: %s\n", log);
        exit(2);
    }
    return shader;
}

int main(void) {
    enum { SIZE = 16 };
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL))
        egl_fail("eglInitialize");
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        egl_fail("eglBindAPI");
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count)
        egl_fail("eglChooseConfig");
    const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE,
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT)
        egl_fail("eglCreateContext GLES 3.2");
    const EGLint surface_attributes[] = {EGL_WIDTH, SIZE, EGL_HEIGHT, SIZE, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    if (surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context))
        egl_fail("eglMakeCurrent");

    GLint major, minor;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major != 3 || minor < 2) {
        fprintf(stderr, "GLES 3.2 context required, got %d.%d (%s)\n",
                major, minor, (const char *)glGetString(GL_VERSION));
        return 2;
    }
    const char *vertex_source =
        "#version 320 es\n"
        "void main() {\n"
        "  vec2 vertices[3] = vec2[3](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));\n"
        "  gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);\n"
        "}\n";
    const char *fragment_source =
        "#version 320 es\n"
        "precision highp float;\n"
        "layout(location=0) out vec4 color;\n"
        "void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {0};
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "GLSL ES 3.20 link failed: %s\n", log);
        return 2;
    }
    glUseProgram(program);
    glViewport(0, 0, SIZE, SIZE);
    glDisable(GL_DITHER);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLubyte result[SIZE * SIZE][4];
    glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, result);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "Draw/readback: GL error 0x%x\n", error);
        return 2;
    }
    for (size_t i = 0; i < SIZE * SIZE; ++i) {
        if (result[i][0] != 255 || result[i][1] || result[i][2] || result[i][3] != 255) {
            fprintf(stderr, "Incorrect pixel at index %zu\n", i);
            return 2;
        }
    }
    printf("PASS GLES %d.%d GLSL ES 3.20, %d pixels verified (%s)\n",
           major, minor, SIZE * SIZE, (const char *)glGetString(GL_RENDERER));
    glDeleteProgram(program);
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return 0;
}
