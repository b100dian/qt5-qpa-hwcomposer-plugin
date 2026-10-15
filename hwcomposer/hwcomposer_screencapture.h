/****************************************************************************
**
** Copyright (C) 2026 Jolla Ltd.
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

#ifndef HWCOMPOSER_SCREENCAPTURE_H
#define HWCOMPOSER_SCREENCAPTURE_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cutils/native_handle.h>
#include <stdint.h>

#include <map>

class HWComposerNativeWindowBuffer;
struct MinisfScreenCaptureSessionInfo;

/*
 * Renders the display into the encoder Surface of a screen recorder.
 *
 * A recorder (droidmedia's droid_media_screen_capture_*) registers its
 * encoder's input Surface with the sailfish.screencap service. This class
 * polls the service through libminisf, and after every primary swap draws
 * the buffer that was just presented into that Surface: one textured quad,
 * with no CPU copy. The recorder must ask for the display size, or its
 * transpose for a landscape recording.
 */
class HwComposerScreenCapture
{
public:
    // Returns nullptr unless QPA_HWC_SCREENCAP is 1 or true.
    static HwComposerScreenCapture *create(int displayWidth, int displayHeight);
    ~HwComposerScreenCapture();

    // From present(), inside the primary eglSwapBuffers().
    void bufferPresented(HWComposerNativeWindowBuffer *buffer, int acquireFenceFd);
    // After the primary eglSwapBuffers() has returned.
    void captureFrame();

private:
    HwComposerScreenCapture(int displayWidth, int displayHeight, int landscapeRotation);

    bool resolve(void *libminisf);
    void pollSession();
    void attach(const MinisfScreenCaptureSessionInfo &info, int rotation);
    void detach();
    void fail();
    bool submit(HWComposerNativeWindowBuffer *buffer);
    bool render(HWComposerNativeWindowBuffer *buffer);
    GLuint sourceTexture(HWComposerNativeWindowBuffer *buffer);

    // libminisf
    int (*m_sessionQuery)(MinisfScreenCaptureSessionInfo *info) = nullptr;
    void *(*m_targetAcquireGeneration)(uint64_t generation) = nullptr;
    void *(*m_targetNativeWindow)(void *target) = nullptr;
    void (*m_targetRelease)(void *target) = nullptr;

    // EGL and GL extensions
    PFNEGLCREATEIMAGEKHRPROC m_createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_imageTargetTexture = nullptr;
    PFNEGLPRESENTATIONTIMEANDROIDPROC m_presentationTime = nullptr;

    const int m_displayWidth;
    const int m_displayHeight;
    const int m_landscapeRotation;

    // Last buffer handed to the display, and a copy of its acquire fence.
    HWComposerNativeWindowBuffer *m_buffer = nullptr;
    int m_fenceFd = -1;

    int64_t m_nextPollNs = 0;
    uint64_t m_failedGeneration = 0;

    // The session this object is attached to.
    void *m_target = nullptr;
    uint64_t m_generation = 0;
    int m_width = 0;
    int m_height = 0;
    int m_rotation = 0;

    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
    GLuint m_program = 0;

    struct Source {
        EGLImageKHR image;
        GLuint texture;
    };
    std::map<buffer_handle_t, Source> m_sources;
};

#endif /* HWCOMPOSER_SCREENCAPTURE_H */
