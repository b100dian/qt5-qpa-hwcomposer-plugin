/*
 * Runtime-resolved screen-capture bridge exported by libminisf.
 *
 * Keep this declaration header independent from droidmedia so the QPA plugin
 * does not acquire a build-time media-stack dependency.
 */
#ifndef QPA_MINISF_SCREEN_CAPTURE_H
#define QPA_MINISF_SCREEN_CAPTURE_H

#include <stdint.h>

/*
 * The concrete Android Surface, BufferQueue producer, and all reference
 * counting remain inside libminisf/libdroidmedia. QPA may only retain these
 * handles and pass the native-window value to EGL.
 */
struct MinisfScreenCaptureSessionInfo {
    uint64_t generation;
    int width;
    int height;
    int fps;
};

struct MinisfScreenCaptureApi {
    typedef int (*SessionQuery)(MinisfScreenCaptureSessionInfo *info);
    typedef void *(*TargetAcquireGeneration)(uint64_t expectedGeneration);
    typedef void *(*TargetNativeWindow)(void *target);
    typedef void (*TargetRelease)(void *target);
    typedef int (*TargetIsCurrent)(void *target, uint64_t generation);

    SessionQuery sessionQuery;
    TargetAcquireGeneration targetAcquireGeneration;
    TargetNativeWindow targetNativeWindow;
    TargetRelease targetRelease;
    /* Retained for standalone diagnostics; QPA uses bounded sessionQuery(). */
    TargetIsCurrent targetIsCurrent;

    bool valid() const {
        return sessionQuery && targetAcquireGeneration && targetNativeWindow &&
               targetRelease;
    }
};

const MinisfScreenCaptureApi *minisfScreenCaptureApi();

#endif /* QPA_MINISF_SCREEN_CAPTURE_H */
