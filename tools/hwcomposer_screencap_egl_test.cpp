/*
 * Gate-2.1 libhybris EGL interoperability test.
 *
 * This executable is deliberately outside the QPA plugin and does not use Qt
 * or Android framework C++ headers. It renders test bars through the public
 * libhybris EGL/GLES frontends into the MediaCodec input Surface published by
 * the opaque libminisf screen-capture ABI.
 *
 * A passing run proves that an Android MediaCodec input ANativeWindow can be
 * used by the hwcomposer libhybris EGL platform at an ordinary, non-reentrant
 * EGL call site. It does not test QPA capture scheduling or screen blitting.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

#ifndef LIBHYBRIS_COMMON_PATH
#define LIBHYBRIS_COMMON_PATH "libhybris-common.so.1"
#endif

#ifndef LIB_MINISF_PATH
#define LIB_MINISF_PATH "libminisf.so"
#endif

struct MinisfApi {
    typedef void *(*AndroidDlopen)(const char *filename, int flags);
    typedef void *(*AndroidDlsym)(void *handle, const char *symbol);
    typedef int (*AndroidDlclose)(void *handle);
    typedef void *(*TargetAcquire)(int width, int height, uint64_t *generation);
    typedef void *(*TargetNativeWindow)(void *target);
    typedef void (*TargetRelease)(void *target);
    typedef int (*TargetIsCurrent)(void *target, uint64_t generation);

    void *hybrisHandle;
    void *minisfHandle;
    AndroidDlopen androidDlopen;
    AndroidDlsym androidDlsym;
    AndroidDlclose androidDlclose;
    TargetAcquire targetAcquire;
    TargetNativeWindow targetNativeWindow;
    TargetRelease targetRelease;
    TargetIsCurrent targetIsCurrent;

    MinisfApi()
        : hybrisHandle(nullptr)
        , minisfHandle(nullptr)
        , androidDlopen(nullptr)
        , androidDlsym(nullptr)
        , androidDlclose(nullptr)
        , targetAcquire(nullptr)
        , targetNativeWindow(nullptr)
        , targetRelease(nullptr)
        , targetIsCurrent(nullptr)
    {
    }

    bool load()
    {
        hybrisHandle = dlopen(LIBHYBRIS_COMMON_PATH, RTLD_NOW | RTLD_LOCAL);
        if (!hybrisHandle) {
            fprintf(stderr, "cannot load %s: %s\n", LIBHYBRIS_COMMON_PATH,
                    dlerror());
            return false;
        }

        androidDlopen = reinterpret_cast<AndroidDlopen>(
            dlsym(hybrisHandle, "android_dlopen"));
        androidDlsym = reinterpret_cast<AndroidDlsym>(
            dlsym(hybrisHandle, "android_dlsym"));
        androidDlclose = reinterpret_cast<AndroidDlclose>(
            dlsym(hybrisHandle, "android_dlclose"));
        if (!androidDlopen || !androidDlsym || !androidDlclose) {
            fprintf(stderr, "%s does not export the Android linker bridge\n",
                    LIBHYBRIS_COMMON_PATH);
            unload();
            return false;
        }

        minisfHandle = androidDlopen(LIB_MINISF_PATH, RTLD_NOW);
        if (!minisfHandle) {
            fprintf(stderr, "android_dlopen(%s) failed\n", LIB_MINISF_PATH);
            unload();
            return false;
        }

        targetAcquire = reinterpret_cast<TargetAcquire>(androidDlsym(
            minisfHandle, "minisf_screen_capture_target_acquire"));
        targetNativeWindow = reinterpret_cast<TargetNativeWindow>(androidDlsym(
            minisfHandle, "minisf_screen_capture_target_native_window"));
        targetRelease = reinterpret_cast<TargetRelease>(androidDlsym(
            minisfHandle, "minisf_screen_capture_target_release"));
        targetIsCurrent = reinterpret_cast<TargetIsCurrent>(androidDlsym(
            minisfHandle, "minisf_screen_capture_target_is_current"));
        if (!targetAcquire || !targetNativeWindow || !targetRelease ||
            !targetIsCurrent) {
            fprintf(stderr, "libminisf has no compatible opaque screen-capture ABI\n");
            unload();
            return false;
        }
        return true;
    }

    void unload()
    {
        if (minisfHandle && androidDlclose) androidDlclose(minisfHandle);
        minisfHandle = nullptr;
        if (hybrisHandle) dlclose(hybrisHandle);
        hybrisHandle = nullptr;
        androidDlopen = nullptr;
        androidDlsym = nullptr;
        androidDlclose = nullptr;
        targetAcquire = nullptr;
        targetNativeWindow = nullptr;
        targetRelease = nullptr;
        targetIsCurrent = nullptr;
    }
};

struct EglObjects {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;

    EglObjects()
        : display(EGL_NO_DISPLAY)
        , context(EGL_NO_CONTEXT)
        , surface(EGL_NO_SURFACE)
    {
    }

    void destroy()
    {
        if (display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
        display = EGL_NO_DISPLAY;
        context = EGL_NO_CONTEXT;
        surface = EGL_NO_SURFACE;
    }
};

static int64_t monotonicNs()
{
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static void sleepUntilNs(int64_t deadlineNs)
{
    for (;;) {
        const int64_t remainingNs = deadlineNs - monotonicNs();
        if (remainingNs <= 0) return;
        const timespec delay = {
            static_cast<time_t>(remainingNs / 1000000000LL),
            static_cast<long>(remainingNs % 1000000000LL)
        };
        if (nanosleep(&delay, nullptr) == 0 || errno != EINTR) return;
    }
}

static GLuint compileShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    char log[512] = {};
    GLsizei length = 0;
    glGetShaderInfoLog(shader, sizeof(log), &length, log);
    fprintf(stderr, "shader compilation failed: %.*s\n", static_cast<int>(length), log);
    glDeleteShader(shader);
    return 0;
}

static GLuint createBarsProgram()
{
    static const char *vertexSource =
        "attribute vec2 aPosition;\n"
        "varying vec2 vPosition;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "  vPosition = aPosition * 0.5 + 0.5;\n"
        "}\n";
    static const char *fragmentSource =
        "precision mediump float;\n"
        "varying vec2 vPosition;\n"
        "uniform float uFrame;\n"
        "void main() {\n"
        "  float x = vPosition.x;\n"
        "  if (x > 0.90 && vPosition.y < 0.10) {\n"
        "    float bit = mod(uFrame, 2.0);\n"
        "    gl_FragColor = bit < 1.0 ? vec4(0.0, 0.0, 0.0, 1.0)\n"
        "                             : vec4(1.0, 1.0, 1.0, 1.0);\n"
        "  } else if (x < 0.143) gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
        "  else if (x < 0.286) gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0);\n"
        "  else if (x < 0.429) gl_FragColor = vec4(0.0, 1.0, 1.0, 1.0);\n"
        "  else if (x < 0.572) gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
        "  else if (x < 0.715) gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n"
        "  else if (x < 0.858) gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "  else gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0);\n"
        "}\n";

    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "aPosition");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) return program;

    char log[512] = {};
    GLsizei length = 0;
    glGetProgramInfoLog(program, sizeof(log), &length, log);
    fprintf(stderr, "program link failed: %.*s\n", static_cast<int>(length), log);
    glDeleteProgram(program);
    return 0;
}

static void logConfig(EGLDisplay display, EGLConfig config)
{
    EGLint configId = 0;
    EGLint surfaceType = 0;
    EGLint renderableType = 0;
    EGLint recordable = 0;
    EGLint nativeVisualId = 0;
    EGLint red = 0;
    EGLint green = 0;
    EGLint blue = 0;
    EGLint alpha = 0;
    eglGetConfigAttrib(display, config, EGL_CONFIG_ID, &configId);
    eglGetConfigAttrib(display, config, EGL_SURFACE_TYPE, &surfaceType);
    eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE, &renderableType);
    eglGetConfigAttrib(display, config, EGL_RECORDABLE_ANDROID, &recordable);
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &nativeVisualId);
    eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &alpha);
    fprintf(stderr, "EGL config: id=%d surface=0x%x renderable=0x%x "
            "recordable=%d visual=%d rgba=%d/%d/%d/%d\n", configId,
            surfaceType, renderableType, recordable, nativeVisualId,
            red, green, blue, alpha);
}

static bool setupEgl(void *nativeWindow, int width, int height, EglObjects *egl)
{
    egl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetDisplay failed: 0x%x\n", eglGetError());
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(egl->display, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
        return false;
    }
    fprintf(stderr, "libhybris EGL initialized: %d.%d, vendor=%s\n", major, minor,
            eglQueryString(egl->display, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "eglBindAPI failed: 0x%x\n", eglGetError());
        return false;
    }

    const EGLint configAttrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RECORDABLE_ANDROID, EGL_TRUE,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (!eglChooseConfig(egl->display, configAttrs, &config, 1, &configCount) ||
        configCount == 0) {
        fprintf(stderr, "eglChooseConfig(recordable RGBA GLES2) failed: 0x%x\n",
                eglGetError());
        return false;
    }

    logConfig(egl->display, config);

    const EGLint contextAttrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    egl->context = eglCreateContext(egl->display, config, EGL_NO_CONTEXT,
                                    contextAttrs);
    if (egl->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext failed: 0x%x\n", eglGetError());
        return false;
    }

    egl->surface = eglCreateWindowSurface(
        egl->display, config, reinterpret_cast<EGLNativeWindowType>(nativeWindow),
        nullptr);
    if (egl->surface == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreateWindowSurface(MediaCodec input Surface) failed: 0x%x\n",
                eglGetError());
        return false;
    }

    if (!eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context)) {
        fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
        return false;
    }
    if (!eglSwapInterval(egl->display, 0)) {
        fprintf(stderr, "eglSwapInterval(0) failed: 0x%x\n", eglGetError());
        return false;
    }

    glViewport(0, 0, width, height);
    return true;
}

static void printUsage(const char *program)
{
    fprintf(stderr, "usage: %s [width] [height] [fps] [frames] [wait-seconds]\n", program);
    fprintf(stderr, "defaults: 1080 2520 30 60 15\n");
}

} // namespace

int main(int argc, char **argv)
{
    const int width = argc > 1 ? atoi(argv[1]) : 1080;
    const int height = argc > 2 ? atoi(argv[2]) : 2520;
    const int fps = argc > 3 ? atoi(argv[3]) : 30;
    const int frameCount = argc > 4 ? atoi(argv[4]) : 60;
    const int waitSeconds = argc > 5 ? atoi(argv[5]) : 15;
    if (width <= 0 || height <= 0 || fps <= 0 || frameCount <= 0 ||
        waitSeconds <= 0) {
        printUsage(argv[0]);
        return 2;
    }

    const char *platform = getenv("HYBRIS_EGLPLATFORM");
    if (!platform || (strcmp(platform, "hwcomposer") != 0 &&
                      strcmp(platform, "null") != 0)) {
        fprintf(stderr, "set HYBRIS_EGLPLATFORM=hwcomposer for Gate 2.1, or "
                "HYBRIS_EGLPLATFORM=null for its passthrough control\n");
        return 2;
    }
    fprintf(stderr, "using libhybris EGL platform: %s\n", platform);

    MinisfApi minisf;
    if (!minisf.load()) return 1;

    void *target = nullptr;
    uint64_t generation = 0;
    const int64_t waitDeadline = monotonicNs() + waitSeconds * 1000000000LL;
    fprintf(stderr, "waiting up to %d seconds for %dx%d recorder target...\n",
            waitSeconds, width, height);
    while (!target && monotonicNs() < waitDeadline) {
        target = minisf.targetAcquire(width, height, &generation);
        if (!target) sleepUntilNs(monotonicNs() + 100000000LL);
    }
    if (!target) {
        fprintf(stderr, "no matching active recorder target; start "
                "screencap_surface_capture_test first\n");
        minisf.unload();
        return 1;
    }

    void *nativeWindow = minisf.targetNativeWindow(target);
    if (!nativeWindow) {
        fprintf(stderr, "opaque target generation=%" PRIu64 " has no native window\n",
                generation);
        minisf.targetRelease(target);
        minisf.unload();
        return 1;
    }
    fprintf(stderr, "acquired recorder target generation=%" PRIu64
            " native-window=%p\n", generation, nativeWindow);

    EglObjects egl;
    bool success = setupEgl(nativeWindow, width, height, &egl);
    GLuint program = 0;
    PFNEGLPRESENTATIONTIMEANDROIDPROC presentationTime = nullptr;
    if (success) {
        presentationTime = reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(
            eglGetProcAddress("eglPresentationTimeANDROID"));
        if (!presentationTime) {
            fprintf(stderr, "eglPresentationTimeANDROID is unavailable\n");
            success = false;
        }
    }
    if (success) {
        program = createBarsProgram();
        success = program != 0;
    }

    if (success) {
        static const GLfloat vertices[] = {
            -1.0f, -1.0f, 1.0f, -1.0f,
            -1.0f,  1.0f, 1.0f,  1.0f,
        };
        const GLint position = 0;
        const GLint frameUniform = glGetUniformLocation(program, "uFrame");
        const int64_t periodNs = 1000000000LL / fps;
        const int64_t startNs = monotonicNs();

        glUseProgram(program);
        glEnableVertexAttribArray(position);
        glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        for (int frame = 0; frame < frameCount; ++frame) {
            sleepUntilNs(startNs + frame * periodNs);
            glUniform1f(frameUniform, static_cast<GLfloat>(frame));
            glViewport(0, 0, width, height);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            const GLenum glError = glGetError();
            if (glError != GL_NO_ERROR) {
                fprintf(stderr, "GL draw failed at frame %d: 0x%x\n", frame, glError);
                success = false;
                break;
            }

            const int64_t timestampNs = monotonicNs();
            if (!presentationTime(egl.display, egl.surface,
                                  static_cast<EGLnsecsANDROID>(timestampNs))) {
                fprintf(stderr, "eglPresentationTimeANDROID failed at frame %d: 0x%x\n",
                        frame, eglGetError());
                success = false;
                break;
            }

            const int64_t swapStartNs = monotonicNs();
            if (!eglSwapBuffers(egl.display, egl.surface)) {
                fprintf(stderr, "eglSwapBuffers failed at frame %d: 0x%x\n",
                        frame, eglGetError());
                success = false;
                break;
            }
            const int64_t swapMs = (monotonicNs() - swapStartNs) / 1000000LL;
            fprintf(stderr, "frame %d/%d: pts=%" PRId64 " swap=%" PRId64 "ms\n",
                    frame + 1, frameCount, timestampNs, swapMs);
        }
        glDisableVertexAttribArray(position);
    }

    if (success && !minisf.targetIsCurrent(target, generation)) {
        fprintf(stderr, "recorder target generation changed before completion\n");
        success = false;
    }

    if (program) glDeleteProgram(program);
    egl.destroy();
    minisf.targetRelease(target);
    minisf.unload();

    fprintf(stderr, "Gate-2.1 hwcomposer/libhybris EGL test: %s\n",
            success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
