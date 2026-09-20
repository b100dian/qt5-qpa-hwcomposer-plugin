/****************************************************************************
**
** Copyright (C) 2013 Jolla Ltd.
** Contact: Thomas Perl <thomas.perl@jolla.com>
**
** This file is part of the hwcomposer plugin.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <android-version.h>
#include "hwcomposer_backend_v20.h"
#include "qeglfswindow.h"

#include <string>
#include <vector>
#include <map>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimerEvent>
#include <QtCore/QCoreApplication>
#include <private/qwindow_p.h>

#include "qsystrace_selector.h"

#include <inttypes.h>
#include <time.h>

/* Screen capture uses only the opaque libminisf C ABI. */
#include "minisf_screen_capture.h"

/* EGL extensions needed for GPU blit — available via hybris libEGL */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef EGL_RECORDABLE_ANDROID
#define EGL_RECORDABLE_ANDROID 0x3142
#endif

/* The target SDK does not enable extension prototypes. Resolve these through
 * EGL so the calls also pass through libhybris' EGLImage handle mapping. */
typedef EGLImageKHR (*EglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                         EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*EglDestroyImageKHR)(EGLDisplay, EGLImageKHR);
typedef void (*GlEglImageTargetTexture2DOES)(GLenum, GLeglImageOES);
typedef EGLBoolean (*EglPresentationTimeANDROID)(EGLDisplay, EGLSurface,
                                                  EGLnsecsANDROID);

static int64_t captureMonotonicNs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static EglPresentationTimeANDROID eglPresentationTimeAndroidProc()
{
    static EglPresentationTimeANDROID proc = nullptr;
    if (!proc) {
        proc = (EglPresentationTimeANDROID) eglGetProcAddress(
            "eglPresentationTimeANDROID");
    }
    return proc;
}

static bool captureDebugEnabled()
{
    static const bool enabled = qEnvironmentVariableIsSet(
        "QPA_HWC_SCREENCAP_DEBUG");
    return enabled;
}

static void logCaptureConfig(EGLDisplay dpy, EGLConfig config)
{
    if (!captureDebugEnabled()) return;

    EGLint configId = 0;
    EGLint surfaceType = 0;
    EGLint renderableType = 0;
    EGLint recordable = 0;
    EGLint visual = 0;
    EGLint red = 0;
    EGLint green = 0;
    EGLint blue = 0;
    EGLint alpha = 0;
    eglGetConfigAttrib(dpy, config, EGL_CONFIG_ID, &configId);
    eglGetConfigAttrib(dpy, config, EGL_SURFACE_TYPE, &surfaceType);
    eglGetConfigAttrib(dpy, config, EGL_RENDERABLE_TYPE, &renderableType);
    eglGetConfigAttrib(dpy, config, EGL_RECORDABLE_ANDROID, &recordable);
    eglGetConfigAttrib(dpy, config, EGL_NATIVE_VISUAL_ID, &visual);
    eglGetConfigAttrib(dpy, config, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(dpy, config, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(dpy, config, EGL_BLUE_SIZE, &blue);
    eglGetConfigAttrib(dpy, config, EGL_ALPHA_SIZE, &alpha);
    qDebug("screencap: EGL config id=%d surface=0x%x renderable=0x%x "
           "recordable=%d visual=%d rgba=%d/%d/%d/%d", configId,
           surfaceType, renderableType, recordable, visual,
           red, green, blue, alpha);
}

static EglCreateImageKHR eglCreateImageKhrProc()
{
    static EglCreateImageKHR proc = nullptr;
    if (!proc) {
        proc = (EglCreateImageKHR) eglGetProcAddress("eglCreateImageKHR");
    }
    return proc;
}

static EglDestroyImageKHR eglDestroyImageKhrProc()
{
    static EglDestroyImageKHR proc = nullptr;
    if (!proc) {
        proc = (EglDestroyImageKHR) eglGetProcAddress("eglDestroyImageKHR");
    }
    return proc;
}

static GlEglImageTargetTexture2DOES glEglImageTargetTexture2dOesProc()
{
    static GlEglImageTargetTexture2DOES proc = nullptr;
    if (!proc) {
        proc = (GlEglImageTargetTexture2DOES) eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    }
    return proc;
}

// #ifdef HWC_PLUGIN_HAVE_HWCOMPOSER1_API

// #define QPA_HWC_TIMING

#ifdef QPA_HWC_TIMING
#define QPA_HWC_TIMING_SAMPLE(variable) variable = timer.nsecsElapsed()
static QElapsedTimer timer;
static qint64 presentTime;
static qint64 syncTime;
static qint64 prepareTime;
static qint64 setTime;
#else
#define QPA_HWC_TIMING_SAMPLE(variable)
#endif

struct HwcProcs_v20 : public HWC2EventListener
{
    HwComposerBackend_v20 *backend;
    hwc2_display_t primaryDisplayId;
};

void hwc2_callback_vsync(HWC2EventListener* listener, int32_t /*sequenceId*/,
                         hwc2_display_t /*display*/, int64_t /*timestamp*/)
{
    static int counter = 0;
    ++counter;
    if (counter % 2)
        QSystrace::begin("graphics", "QPA::vsync", "");
    else
        QSystrace::end("graphics", "QPA::vsync", "");

    QCoreApplication::postEvent(static_cast<const HwcProcs_v20 *>(listener)->backend,
                                new QEvent(QEvent::User));
}

void hwc2_callback_hotplug(HWC2EventListener* listener, int32_t sequenceId,
                           hwc2_display_t display, bool connected,
                           bool primaryDisplay)
{
    qDebug("onHotplugReceived(%d, %" PRIu64 ", %s, %s)",
           sequenceId, display,
           connected ? "connected" : "disconnected",
           primaryDisplay ? "primary" : "external");

    if (primaryDisplay) {
        static_cast<HwcProcs_v20 *>(listener)->primaryDisplayId = display;
    }

    static_cast<const HwcProcs_v20 *>(listener)->backend->onHotplugReceived(
        sequenceId, display, connected, primaryDisplay);
}

void hwc2_callback_refresh(HWC2EventListener* /*listener*/, int32_t /*sequenceId*/,
                           hwc2_display_t /*display*/)
{
}

class HWC2Window : public HWComposerNativeWindow
{
    private:
        hwc2_compat_layer_t *layer;
        hwc2_compat_display_t *hwcDisplay;
        bool m_syncBeforeSet;
        HWComposerNativeWindowBuffer *m_lastBuffer = nullptr;
        std::vector<HWComposerNativeWindowBuffer*> m_slotCache;
        int m_nextSlot = 0;

        /* ---------- Screen capture (Surface input) ---------- */
        bool m_captureEnabled = false;
        void *m_captureTarget = nullptr;
        void *m_captureNativeWindow = nullptr;
        uint64_t m_captureGeneration = 0;
        EGLSurface m_captureSurface = EGL_NO_SURFACE;
        EGLContext m_captureContext = EGL_NO_CONTEXT;
        int m_captureFrameSkip = 1;   /* diagnostic multiplier only */
        int m_captureFrameCounter = 0;
        int m_captureWidth = 0;
        int m_captureHeight = 0;
        int m_captureFps = 30;
        int64_t m_captureNextFrameNs = 0;
        int64_t m_captureNextSessionCheckNs = 0;
        int m_captureFrameLimit = 0;  /* Gate 2.2: one frame by default */
        int m_captureFramesSubmitted = 0;
        bool m_captureCandidateReady = false;
        HWComposerNativeWindowBuffer *m_captureCandidate = nullptr;
        /* Per-source-buffer EGLImage + GL texture cache. */
        std::map<buffer_handle_t, EGLImageKHR> m_captureEglImages;
        std::map<buffer_handle_t, GLuint> m_captureTextures;
        GLuint m_captureProgram = 0;
        GLuint m_captureBarsProgram = 0;
        bool m_captureTestBars = false;

        void captureInit(int width, int height);
        void captureShutdown();
        void captureAfterPrimarySwap();
        void captureFrame(HWComposerNativeWindowBuffer *src, int width, int height);
        void blitRgbaToSurface(GLuint srcTex, int dw, int dh);
        void renderTestBars(int width, int height, int frame);
    protected:
        void present(HWComposerNativeWindowBuffer *buffer);

    public:

        HWC2Window(unsigned int width, unsigned int height, unsigned int format,
                hwc2_compat_display_t *display, hwc2_compat_layer_t *layer);
        ~HWC2Window();
        void set();
        void captureAfterPrimarySwapFromBackend();
};

HWC2Window::HWC2Window(unsigned int width, unsigned int height,
                    unsigned int format, hwc2_compat_display_t* display,
                    hwc2_compat_layer_t *layer) :
                    HWComposerNativeWindow(width, height, format),
                    layer(layer), hwcDisplay(display)
{
    int bufferCount = qgetenv("QPA_HWC_BUFFER_COUNT").toInt();
    if (bufferCount)
        bufferCount = qBound(2, bufferCount, 8);
    else
        // default to triple-buffering as on Android
        bufferCount = 3;
    setBufferCount(bufferCount);
    m_slotCache.resize(bufferCount, nullptr);
    m_syncBeforeSet = qEnvironmentVariableIsSet("QPA_HWC_SYNC_BEFORE_SET");

    /* Capture is deliberately limited to post-primary-swap test bars for
     * Gate 2.2. Do not enable source EGLImage import until this scheduling
     * point has produced a decodable stream without disturbing lipstick. */
    m_captureWidth  = width;
    m_captureHeight = height;
    const char *capEnv = getenv("QPA_HWC_SCREENCAP");
    if (capEnv && (strcmp(capEnv, "1") == 0 || strcmp(capEnv, "true") == 0)) {
        const char *barsEnv = getenv("QPA_HWC_SCREENCAP_TEST_BARS");
        m_captureTestBars = barsEnv &&
            (strcmp(barsEnv, "1") == 0 || strcmp(barsEnv, "true") == 0);
        if (!m_captureTestBars) {
            qWarning("screencap: Gate 2.2 requires QPA_HWC_SCREENCAP_TEST_BARS=1; capture disabled");
            return;
        }

        const char *fpsEnv = getenv("QPA_HWC_SCREENCAP_FPS");
        if (fpsEnv) m_captureFps = atoi(fpsEnv);
        m_captureFps = qBound(1, m_captureFps, 120);

        const char *limitEnv = getenv("QPA_HWC_SCREENCAP_FRAME_LIMIT");
        if (limitEnv) m_captureFrameLimit = atoi(limitEnv);
        if (m_captureFrameLimit < 1) m_captureFrameLimit = 1;

        const char *skipEnv = getenv("QPA_HWC_SCREENCAP_FRAME_SKIP");
        if (skipEnv) m_captureFrameSkip = atoi(skipEnv);
        if (m_captureFrameSkip < 1) m_captureFrameSkip = 1;

        m_captureEnabled = true;
        qDebug("screencap: Gate 2.2 enabled: test-bars only, %d fps, limit=%d",
               m_captureFps, m_captureFrameLimit);
    }
}

HWC2Window::~HWC2Window()
{
    captureShutdown();  /* Phase 2 — release capture resources */

    if (m_lastBuffer != nullptr) {
        int fenceFd = getFenceBufferFd(m_lastBuffer);
        if (fenceFd != -1)
            close(fenceFd);
        setFenceBufferFd(m_lastBuffer, -1);
        m_lastBuffer->common.decRef(&m_lastBuffer->common);
    }
}

void HWC2Window::present(HWComposerNativeWindowBuffer *buffer)
{
    uint32_t numTypes = 0;
    uint32_t numRequests = 0;
    int displayId = 0;
    hwc2_error_t error = HWC2_ERROR_NONE;

    QSystraceEvent trace("graphics", "QPA::present");

    QPA_HWC_TIMING_SAMPLE(presentTime);

    int acquireFenceFd = getFenceBufferFd(buffer);

    if (m_syncBeforeSet && acquireFenceFd >= 0) {
        sync_wait(acquireFenceFd, -1);
        close(acquireFenceFd);
        acquireFenceFd = -1;
    }

    error = hwc2_compat_display_validate(hwcDisplay, &numTypes,
                                                    &numRequests);
    if (error != HWC2_ERROR_NONE && error != HWC2_ERROR_HAS_CHANGES) {
        qDebug("prepare: validate failed for display %d: %d", displayId, error);
        return;
    }

    if (numTypes || numRequests) {
        qDebug("prepare: validate required changes for display %d: %d",
               displayId, error);
        return;
    }

    error = hwc2_compat_display_accept_changes(hwcDisplay);
    if (error != HWC2_ERROR_NONE) {
        qDebug("prepare: acceptChanges failed: %d", error);
        return;
    }

    QPA_HWC_TIMING_SAMPLE(prepareTime);



    QSystrace::begin("graphics", "QPA::set_client_target", "");

    int slot = -1;
    HWComposerNativeWindowBuffer *target = buffer;

    // Check if the current buffer is cached in any slots
    for (size_t i = 0; i < m_slotCache.size(); i++) {
        if (m_slotCache[i] == buffer) {
            target = nullptr;
            slot = i;
            break;
        }
    }

    // If not found, use the next slot and update the cache
    if (slot == -1) {
        slot = m_nextSlot;
        m_slotCache[slot] = buffer;
        m_nextSlot = (m_nextSlot + 1) % m_slotCache.size();
    }

    hwc2_compat_display_set_client_target(hwcDisplay, slot, target,
                                          acquireFenceFd,
                                          HAL_DATASPACE_UNKNOWN);

    QSystrace::end("graphics", "QPA::set_client_target", "");

    QSystrace::begin("graphics", "QPA::present", "");
    int presentFence = -1;
    hwc2_compat_display_present(hwcDisplay, &presentFence);
    QSystrace::end("graphics", "QPA::present", "");

    QPA_HWC_TIMING_SAMPLE(setTime);

    setFenceBufferFd(buffer, -1);

    // HWC2 present fences signal when the frame n is displayed on screen
    // and the buffer for the previous frame n-1 is no longer needed.
    if (m_lastBuffer != nullptr) {
        int fenceFd = getFenceBufferFd(m_lastBuffer);
        if (fenceFd != -1)
            close(fenceFd);
        setFenceBufferFd(m_lastBuffer, presentFence);
        m_lastBuffer->common.decRef(&m_lastBuffer->common);
    } else if (presentFence != -1) {
        close(presentFence);
    }

    m_lastBuffer = buffer;
    // Prevent the buffer from being destroyed if reallocation happens
    m_lastBuffer->common.incRef(&m_lastBuffer->common);

    /* queueBuffer() is still inside the primary eglSwapBuffers() call here.
     * Record only the immediate candidate; EGL/Binder/capture work is done by
     * HwComposerBackend_v20::swap() after that outer swap returns. */
    if (m_captureEnabled) {
        m_captureCandidate = buffer;
        m_captureCandidateReady = true;
        if (captureDebugEnabled()) {
            qDebug("screencap: candidate recorded in present, awaiting primary swap return");
        }
    }
}

/* ================================================================ */
/*  Screen capture methods (MediaCodec Surface input)                */
/* ================================================================ */

void HWC2Window::captureInit(int width, int height)
{
    if (m_captureTarget != nullptr || !m_captureEnabled) return;

    const MinisfScreenCaptureApi *api = minisfScreenCaptureApi();
    if (!api->valid()) {
        qWarning("screencap: libminisf Surface-input API unavailable");
        m_captureEnabled = false;
        return;
    }

    uint64_t generation = 0;
    void *target = api->targetAcquire(width, height, &generation);
    if (!target) {
        /* No recorder is active. This is the normal idle state. */
        return;
    }

    void *nativeWindow = api->targetNativeWindow(target);
    if (!nativeWindow) {
        qWarning("screencap: target returned no native window");
        api->targetRelease(target);
        m_captureEnabled = false;
        return;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLConfig config = 0;
    EGLint configAttrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RECORDABLE_ANDROID, EGL_TRUE,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint numConfigs = 0;
    if (dpy == EGL_NO_DISPLAY ||
        !eglChooseConfig(dpy, configAttrs, &config, 1, &numConfigs) ||
        numConfigs == 0) {
        qWarning("screencap: no EGL window config for encoder Surface (0x%x)",
                 eglGetError());
        api->targetRelease(target);
        m_captureEnabled = false;
        return;
    }

    logCaptureConfig(dpy, config);

    /* This function is called only after the primary eglSwapBuffers() has
     * returned. A second context/surface may be created while the primary
     * context remains current; do not unbind it merely for creation. */
    EGLint contextAttrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLContext captureContext = eglCreateContext(
        dpy, config, EGL_NO_CONTEXT, contextAttrs);
    if (captureContext == EGL_NO_CONTEXT) {
        qWarning("screencap: eglCreateContext failed for encoder Surface (0x%x)",
                 eglGetError());
        api->targetRelease(target);
        m_captureEnabled = false;
        return;
    }

    EGLSurface surface = eglCreateWindowSurface(
        dpy, config, (EGLNativeWindowType) nativeWindow, nullptr);
    if (surface == EGL_NO_SURFACE) {
        EGLint error = eglGetError();
        eglDestroyContext(dpy, captureContext);
        qWarning("screencap: eglCreateWindowSurface failed for encoder Surface (0x%x)",
                 error);
        api->targetRelease(target);
        m_captureEnabled = false;
        return;
    }

    EGLSurface oldDraw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface oldRead = eglGetCurrentSurface(EGL_READ);
    EGLContext oldContext = eglGetCurrentContext();
    if (!eglMakeCurrent(dpy, surface, surface, captureContext) ||
        !eglSwapInterval(dpy, 0)) {
        EGLint error = eglGetError();
        eglMakeCurrent(dpy, oldDraw, oldRead, oldContext);
        eglDestroySurface(dpy, surface);
        eglDestroyContext(dpy, captureContext);
        qWarning("screencap: cannot configure encoder Surface (0x%x)", error);
        api->targetRelease(target);
        m_captureEnabled = false;
        return;
    }
    if (!eglMakeCurrent(dpy, oldDraw, oldRead, oldContext)) {
        EGLint error = eglGetError();
        eglDestroySurface(dpy, surface);
        eglDestroyContext(dpy, captureContext);
        qWarning("screencap: failed to restore QPA EGL surfaces (0x%x)",
                 error);
        api->targetRelease(target);
        m_captureEnabled = false;
        return;
    }

    m_captureTarget = target;
    m_captureNativeWindow = nativeWindow;
    m_captureGeneration = generation;
    m_captureSurface = surface;
    m_captureContext = captureContext;
    if (captureDebugEnabled()) {
        qDebug("screencap: encoder Surface target=%p generation=%" PRIu64,
               target, generation);
    }
}

void HWC2Window::captureShutdown()
{
    const MinisfScreenCaptureApi *api = minisfScreenCaptureApi();
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSurface oldDraw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface oldRead = eglGetCurrentSurface(EGL_READ);
    EGLContext oldContext = eglGetCurrentContext();

    if (dpy != EGL_NO_DISPLAY && m_captureContext != EGL_NO_CONTEXT &&
        m_captureSurface != EGL_NO_SURFACE) {
        eglMakeCurrent(dpy, m_captureSurface, m_captureSurface,
                       m_captureContext);
    }

    if (dpy != EGL_NO_DISPLAY) {
        for (auto &p : m_captureEglImages) {
            if (p.second != EGL_NO_IMAGE_KHR) {
                EglDestroyImageKHR destroyImage = eglDestroyImageKhrProc();
                if (destroyImage)
                    destroyImage(dpy, p.second);
            }
        }
        for (auto &p : m_captureTextures) {
            GLuint tex = p.second;
            if (tex) glDeleteTextures(1, &tex);
        }
        if (m_captureProgram) {
            glDeleteProgram(m_captureProgram);
        }
        if (m_captureBarsProgram) {
            glDeleteProgram(m_captureBarsProgram);
        }
    }
    m_captureEglImages.clear();
    m_captureTextures.clear();
    m_captureProgram = 0;
    m_captureBarsProgram = 0;

    if (dpy != EGL_NO_DISPLAY && m_captureContext != EGL_NO_CONTEXT) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_captureSurface != EGL_NO_SURFACE) {
            eglDestroySurface(dpy, m_captureSurface);
        }
        eglDestroyContext(dpy, m_captureContext);
    }
    m_captureSurface = EGL_NO_SURFACE;
    m_captureContext = EGL_NO_CONTEXT;
    m_captureNativeWindow = nullptr;

    if (dpy != EGL_NO_DISPLAY && oldContext != EGL_NO_CONTEXT) {
        eglMakeCurrent(dpy, oldDraw, oldRead, oldContext);
    }

    if (m_captureTarget != nullptr && api->valid()) {
        api->targetRelease(m_captureTarget);
    }
    m_captureTarget = nullptr;
    m_captureGeneration = 0;
}

void HWC2Window::captureAfterPrimarySwapFromBackend()
{
    captureAfterPrimarySwap();
}

void HWC2Window::captureAfterPrimarySwap()
{
    if (!m_captureCandidateReady) return;
    m_captureCandidateReady = false;
    HWComposerNativeWindowBuffer *candidate = m_captureCandidate;
    m_captureCandidate = nullptr;

    if (!m_captureEnabled || candidate == nullptr) return;
    if (m_captureFramesSubmitted >= m_captureFrameLimit) {
        m_captureEnabled = false;
        captureShutdown();
        return;
    }

    const int64_t nowNs = captureMonotonicNs();
    if (m_captureTarget == nullptr) {
        if (nowNs < m_captureNextSessionCheckNs) return;
        m_captureNextSessionCheckNs = nowNs + 250000000LL;
        captureInit(m_captureWidth, m_captureHeight);
        if (m_captureTarget == nullptr) return;
    }

    if (nowNs < m_captureNextFrameNs) return;
    m_captureNextFrameNs = nowNs + 1000000000LL / m_captureFps;

    if (++m_captureFrameCounter % m_captureFrameSkip != 0) return;
    if (captureDebugEnabled()) {
        qDebug("screencap: primary swap returned at %" PRId64
               "; submitting test-bars frame %d", nowNs,
               m_captureFramesSubmitted + 1);
    }
    captureFrame(candidate, m_captureWidth, m_captureHeight);
}

static EGLImageKHR ensureEglImage(EGLDisplay dpy, buffer_handle_t h,
                                  std::map<buffer_handle_t, EGLImageKHR> &cache,
                                  std::map<buffer_handle_t, GLuint> &texCache,
                                  GLuint &outTex)
{
    auto it = cache.find(h);
    if (it != cache.end()) {
        outTex = texCache[h];
        return it->second;
    }

    EglCreateImageKHR createImage = eglCreateImageKhrProc();
    GlEglImageTargetTexture2DOES imageTarget =
        glEglImageTargetTexture2dOesProc();
    if (!createImage || !imageTarget) {
        outTex = 0;
        return EGL_NO_IMAGE_KHR;
    }

    EGLint eglImgAttrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = createImage(dpy, EGL_NO_CONTEXT,
                                  EGL_NATIVE_BUFFER_ANDROID,
                                  (EGLClientBuffer)h, eglImgAttrs);
    if (img == EGL_NO_IMAGE_KHR) {
        outTex = 0;
        return EGL_NO_IMAGE_KHR;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    imageTarget(GL_TEXTURE_2D, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    cache[h] = img;
    texCache[h] = tex;
    outTex = tex;
    return img;
}

struct GlStateGuard {
    GLint oldProgram, oldFbo, oldViewport[4];
    GLboolean scissor, blend, depthTest;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface draw, read;

    GlStateGuard() {
        dpy = eglGetCurrentDisplay();
        ctx = eglGetCurrentContext();
        draw = eglGetCurrentSurface(EGL_DRAW);
        read = eglGetCurrentSurface(EGL_READ);
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
        glGetIntegerv(GL_VIEWPORT, oldViewport);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        blend = glIsEnabled(GL_BLEND);
        depthTest = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
    }
    ~GlStateGuard() {
        glUseProgram(oldProgram);
        glBindFramebuffer(GL_FRAMEBUFFER, oldFbo);
        glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (blend) glEnable(GL_BLEND);
        if (depthTest) glEnable(GL_DEPTH_TEST);
        eglMakeCurrent(dpy, draw, read, ctx);
    }
};

void HWC2Window::captureFrame(HWComposerNativeWindowBuffer *src, int width, int height)
{
    Q_UNUSED(src);

    const MinisfScreenCaptureApi *api = minisfScreenCaptureApi();
    if (!api->valid() || !m_captureTarget ||
        !api->targetIsCurrent(m_captureTarget, m_captureGeneration)) {
        qWarning("screencap: encoder target became stale");
        m_captureEnabled = false;
        captureShutdown();
        return;
    }

    EglPresentationTimeANDROID presentationTime = eglPresentationTimeAndroidProc();
    if (!presentationTime) {
        qWarning("screencap: eglPresentationTimeANDROID is unavailable");
        m_captureEnabled = false;
        captureShutdown();
        return;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSurface oldDraw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface oldRead = eglGetCurrentSurface(EGL_READ);
    EGLContext ctx = eglGetCurrentContext();
    if (dpy == EGL_NO_DISPLAY || m_captureContext == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, m_captureSurface, m_captureSurface,
                        m_captureContext)) {
        qWarning("screencap: cannot make encoder Surface current (0x%x)",
                 eglGetError());
        m_captureEnabled = false;
        captureShutdown();
        return;
    }

    /* Gate 2.2 intentionally proves only the safe scheduling point. Source
     * EGLImage import is forbidden until these test bars have passed. */
    if (!m_captureTestBars) {
        qWarning("screencap: real source capture is not enabled at Gate 2.2");
        eglMakeCurrent(dpy, oldDraw, oldRead, ctx);
        m_captureEnabled = false;
        captureShutdown();
        return;
    }

    renderTestBars(width, height, m_captureFramesSubmitted);
    glFlush();

    const int64_t timestampNs = captureMonotonicNs();
    EGLBoolean timestamped = presentationTime(
        dpy, m_captureSurface, static_cast<EGLnsecsANDROID>(timestampNs));
    EGLint timestampError = timestamped ? EGL_SUCCESS : eglGetError();
    EGLBoolean swapped = timestamped && eglSwapBuffers(dpy, m_captureSurface);
    EGLint swapError = swapped ? EGL_SUCCESS : eglGetError();

    /* Always restore QPA's primary surfaces before capture teardown. */
    if (!eglMakeCurrent(dpy, oldDraw, oldRead, ctx)) {
        qWarning("screencap: failed to restore QPA EGL surfaces (0x%x)",
                 eglGetError());
    }

    if (!timestamped) {
        qWarning("screencap: eglPresentationTimeANDROID failed (0x%x)",
                 timestampError);
        m_captureEnabled = false;
        captureShutdown();
        return;
    }
    if (!swapped) {
        qWarning("screencap: encoder eglSwapBuffers failed (0x%x)", swapError);
        m_captureEnabled = false;
        captureShutdown();
        return;
    }

    ++m_captureFramesSubmitted;
    qDebug("screencap: submitted test-bars frame %d pts=%" PRId64,
           m_captureFramesSubmitted, timestampNs);

    if (m_captureFramesSubmitted >= m_captureFrameLimit) {
        qDebug("screencap: Gate 2.2 frame limit reached; disabling capture");
        m_captureEnabled = false;
        captureShutdown();
    }
}

void HWC2Window::renderTestBars(int dw, int dh, int frame)
{
    GlStateGuard guard;

    static const char *vsSrc =
        "attribute vec2 aPos;\n"
        "varying vec2 vPos;\n"
        "void main() { gl_Position = vec4(aPos, 0.0, 1.0);"
        " vPos = aPos * 0.5 + 0.5; }";
    static const char *fsSrc =
        "precision mediump float;\n"
        "varying vec2 vPos; uniform float uFrame;\n"
        "void main() { float x = vPos.x;"
        " if (x > 0.90 && vPos.y < 0.10) {"
        " float bit = mod(uFrame, 2.0);"
        " gl_FragColor = bit < 1.0 ? vec4(0.0,0.0,0.0,1.0) :"
        " vec4(1.0,1.0,1.0,1.0); }"
        " else if (x < 0.143) gl_FragColor = vec4(1.0,1.0,1.0,1.0);"
        " else if (x < 0.286) gl_FragColor = vec4(1.0,1.0,0.0,1.0);"
        " else if (x < 0.429) gl_FragColor = vec4(0.0,1.0,1.0,1.0);"
        " else if (x < 0.572) gl_FragColor = vec4(0.0,1.0,0.0,1.0);"
        " else if (x < 0.715) gl_FragColor = vec4(1.0,0.0,1.0,1.0);"
        " else if (x < 0.858) gl_FragColor = vec4(1.0,0.0,0.0,1.0);"
        " else gl_FragColor = vec4(0.0,0.0,1.0,1.0); }";

    if (!m_captureBarsProgram) {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vsSrc, NULL);
        glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fsSrc, NULL);
        glCompileShader(fs);
        m_captureBarsProgram = glCreateProgram();
        glAttachShader(m_captureBarsProgram, vs);
        glAttachShader(m_captureBarsProgram, fs);
        glBindAttribLocation(m_captureBarsProgram, 0, "aPos");
        glLinkProgram(m_captureBarsProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);
    }

    static const GLfloat verts[] = {
        -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f
    };
    glUseProgram(m_captureBarsProgram);
    glUniform1f(glGetUniformLocation(m_captureBarsProgram, "uFrame"),
                static_cast<GLfloat>(frame));
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, dw, dh);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
}

void HWC2Window::blitRgbaToSurface(GLuint srcTex, int dw, int dh)
{
    GlStateGuard guard;

    static const char *vsSrc =
        "attribute vec2 aPos;\n"
        "varying vec2 vTex;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "  vTex = aPos * 0.5 + 0.5;\n"
        "}";
    static const char *fsSrc =
        "precision mediump float;\n"
        "varying vec2 vTex;\n"
        "uniform sampler2D uTex;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(uTex, vTex);\n"
        "}";

    GLuint &program = m_captureProgram;
    if (!program) {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vsSrc, NULL);
        glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fsSrc, NULL);
        glCompileShader(fs);
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
    }

    static const GLfloat verts[] = {
        -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f
    };
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glUniform1i(glGetUniformLocation(program, "uTex"), 0);
    GLint aPos = glGetAttribLocation(program, "aPos");
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray(aPos);
    glViewport(0, 0, dw, dh);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(aPos);
}

int HwComposerBackend_v20::composerSequenceId = 0;

HwComposerBackend_v20::HwComposerBackend_v20(hw_module_t *hwc_module, void *libminisf)
    : HwComposerBackend(hwc_module, libminisf)
    , hwc2_device(NULL)
    , hwc2_primary_display(NULL)
    , hwc2_primary_layer(NULL)
    , m_displayOff(true)
    , m_primaryWindow(nullptr)
{
    procs = new HwcProcs_v20();
    procs->on_vsync_received = hwc2_callback_vsync;
    procs->on_hotplug_received = hwc2_callback_hotplug;
    procs->on_refresh_received = hwc2_callback_refresh;
    procs->backend = this;
    // primaryDisplayId is not changed in this constructor
    // but it may be changed by hwc2_callback_hotplug and thus the
    // hwc2_compat_device_get_display_by_id loop will request the
    // correct primary diplay once primaryDisplayId has been set.
    procs->primaryDisplayId = 0;

    hwc2_device = hwc2_compat_device_new(false);
    HWC_PLUGIN_ASSERT_NOT_NULL(hwc2_device);

    hwc2_compat_device_register_callback(hwc2_device, procs,
        HwComposerBackend_v20::composerSequenceId++);

    for (int i = 0; i < 5 * 1000; ++i) {
        // Wait at most 5s for hotplug events
        if ((hwc2_primary_display =
            hwc2_compat_device_get_display_by_id(hwc2_device, procs->primaryDisplayId)))
            break;
        usleep(1000);
    }
    HWC_PLUGIN_ASSERT_NOT_NULL(hwc2_primary_display);

    sleepDisplay(false);
}

HwComposerBackend_v20::~HwComposerBackend_v20()
{
    hwc2_compat_display_set_vsync_enabled(hwc2_primary_display, HWC2_VSYNC_DISABLE);

    hwc2_compat_display_set_power_mode(hwc2_primary_display, HWC2_POWER_MODE_OFF);

    // Close the hwcomposer handle
    if (!qgetenv("QPA_HWC_WORKAROUNDS").split(',').contains("no-close-hwc"))
        free(hwc2_device);

    if (hwc2_primary_display != NULL) {
        free(hwc2_primary_display);
    }

    delete procs;
}

EGLNativeDisplayType
HwComposerBackend_v20::display()
{
    return EGL_DEFAULT_DISPLAY;
}

EGLNativeWindowType
HwComposerBackend_v20::createWindow(int width, int height)
{
    // We expect that we haven't created a window already, if we had, we
    // would leak stuff, and we want to avoid that for obvious reasons.
    HWC_PLUGIN_EXPECT_NULL(hwc2_primary_layer);

    hwc2_compat_layer_t* layer = hwc2_primary_layer =
        hwc2_compat_display_create_layer(hwc2_primary_display);

    hwc2_compat_layer_set_composition_type(layer, HWC2_COMPOSITION_CLIENT);
    hwc2_compat_layer_set_blend_mode(layer, HWC2_BLEND_MODE_NONE);
    hwc2_compat_layer_set_source_crop(layer, 0.0f, 0.0f, width, height);
    hwc2_compat_layer_set_display_frame(layer, 0, 0, width, height);
    hwc2_compat_layer_set_visible_region(layer, 0, 0, width, height);

    HWC2Window *hwc_win = new HWC2Window(width, height,
                                         HAL_PIXEL_FORMAT_RGBA_8888,
                                         hwc2_primary_display, layer);
    m_primaryWindow = hwc_win;

    return (EGLNativeWindowType) static_cast<ANativeWindow *>(hwc_win);
}

void
HwComposerBackend_v20::destroyWindow(EGLNativeWindowType window)
{
    Q_UNUSED(window);
}

void
HwComposerBackend_v20::swap(EGLNativeDisplayType display, EGLSurface surface)
{
#ifdef QPA_HWC_TIMING
    timer.start();
#endif

    /* HWC2Window::present() is synchronously reached from this primary swap's
     * queueBuffer() callback. Do not run capture there. This call happens only
     * after the outer eglSwapBuffers() has returned to QPA. */
    EGLBoolean primarySwapped = eglSwapBuffers(display, surface);
    if (!primarySwapped) {
        qWarning("HWComposerBackend::swap: primary eglSwapBuffers failed (0x%x)",
                 eglGetError());
    } else if (m_primaryWindow != nullptr) {
        m_primaryWindow->captureAfterPrimarySwapFromBackend();
    }

#ifdef QPA_HWC_TIMING
    qDebug("HWComposerBackend::swap(), present=%.3f, sync=%.3f, prepare=%.3f, set=%.3f, total=%.3f",
           presentTime / 1000000.0,
           (syncTime - presentTime) / 1000000.0,
           (prepareTime - syncTime) / 1000000.0,
           (setTime - prepareTime) / 1000000.0,
           timer.nsecsElapsed() / 1000000.0);
#endif
}

void
HwComposerBackend_v20::sleepDisplay(bool sleep)
{
    m_displayOff = sleep;
    if (sleep) {
        // Stop the timer so we don't end up calling into eventControl after the
        // screen has been turned off. Doing so leads to logcat errors being
        // logged.
        m_vsyncTimeout.stop();
        hwc2_compat_display_set_vsync_enabled(hwc2_primary_display, HWC2_VSYNC_DISABLE);

        hwc2_compat_display_set_power_mode(hwc2_primary_display, HWC2_POWER_MODE_OFF);
    } else {
        hwc2_compat_display_set_power_mode(hwc2_primary_display, HWC2_POWER_MODE_ON);

        // If we have pending updates, make sure those start happening now..
        if (m_pendingUpdate.size()) {
            hwc2_compat_display_set_vsync_enabled(hwc2_primary_display, HWC2_VSYNC_ENABLE);
            m_vsyncTimeout.start(50, this);
        }
    }
}

float
HwComposerBackend_v20::refreshRate()
{
    float value = (float)hwc2_compat_display_get_active_config(hwc2_primary_display)->vsyncPeriod;

    value = (1000000000.0 / value);

    // make sure the value is "reasonable", otherwise fallback to 60.0.
    return (value > 0 && value <= 1000.0) ? value : 60.0;
}

bool
HwComposerBackend_v20::getScreenSizes(int *width, int *height, float *physical_width, float *physical_height)
{
    HWC2DisplayConfig *config = hwc2_compat_display_get_active_config(hwc2_primary_display);

    // should not happen
    if (!config) return false;

    int dpi_x = config->dpiX;
    int dpi_y = config->dpiY;

    *width = config->width;
    *height = config->height;

    if (dpi_x == 0 || dpi_y == 0 || *width == 0 || *height == 0) {
        qWarning() << "failed to read screen size from hwc1.x backend";
        return false;
    }

    *physical_width = (((float)*width) * 25.4) / (float)dpi_x;
    *physical_height = (((float)*height) * 25.4) / (float)dpi_y;

    return true;
}

void HwComposerBackend_v20::timerEvent(QTimerEvent *e)
{
    if (e->timerId() == m_vsyncTimeout.timerId()) {
        hwc2_compat_display_set_vsync_enabled(hwc2_primary_display, HWC2_VSYNC_DISABLE);
        m_vsyncTimeout.stop();
        // When waking up, we might get here as a result of requesting vsync events
        // before the hwc is up and running. If we're timing out while still waiting
        // for vsync to occur, trigger the update so we don't block the UI.
        if (!m_pendingUpdate.isEmpty())
            handleVSyncEvent();
    } else if (e->timerId() == m_deliverUpdateTimeout.timerId()) {
        m_deliverUpdateTimeout.stop();
        handleVSyncEvent();
    }
}

bool HwComposerBackend_v20::event(QEvent *e)
{
    if (e->type() == QEvent::User) {
        static int idleTime = qBound(5, qgetenv("QPA_HWC_IDLE_TIME").toInt(), 100);
        if (!m_deliverUpdateTimeout.isActive())
            m_deliverUpdateTimeout.start(idleTime, this);
        return true;
    }
    return QObject::event(e);
}

void HwComposerBackend_v20::handleVSyncEvent()
{
    QSystraceEvent trace("graphics", "QPA::handleVsync");
    QSet<QWindow *> pendingWindows = m_pendingUpdate;
    m_pendingUpdate.clear();
    foreach (QWindow *w, pendingWindows) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 12, 0))
        QPlatformWindow *platformWindow = w->handle();
        if (!platformWindow)
            continue;

        platformWindow->deliverUpdateRequest();
#else
        QWindowPrivate *wp = (QWindowPrivate *) QWindowPrivate::get(w);
        wp->deliverUpdateRequest();
#endif
    }
}

bool HwComposerBackend_v20::requestUpdate(QEglFSWindow *window)
{
    // If the display is off, do updates via the normal Qt-based timer.
    if (m_displayOff)
        return false;

    if (m_vsyncTimeout.isActive()) {
        m_vsyncTimeout.stop();
    } else {
        hwc2_compat_display_set_vsync_enabled(hwc2_primary_display, HWC2_VSYNC_ENABLE);
    }
    m_vsyncTimeout.start(50, this);
    m_pendingUpdate.insert(window->window());
    return true;
}

void HwComposerBackend_v20::onHotplugReceived(int32_t /*sequenceId*/,
                                        hwc2_display_t display, bool connected,
                                        bool /*primaryDisplay*/)
{
    hwc2_compat_device_on_hotplug(hwc2_device, display, connected);
}

// #endif /* HWC_PLUGIN_HAVE_HWCOMPOSER1_API */
