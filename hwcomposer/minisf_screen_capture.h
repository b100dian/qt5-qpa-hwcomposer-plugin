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
struct MinisfScreenCaptureApi {
    typedef void *(*TargetAcquire)(int width, int height, uint64_t *generation);
    typedef void *(*TargetNativeWindow)(void *target);
    typedef void (*TargetRelease)(void *target);
    typedef int (*TargetIsCurrent)(void *target, uint64_t generation);

    TargetAcquire targetAcquire;
    TargetNativeWindow targetNativeWindow;
    TargetRelease targetRelease;
    TargetIsCurrent targetIsCurrent;

    bool valid() const {
        return targetAcquire && targetNativeWindow && targetRelease &&
               targetIsCurrent;
    }
};

const MinisfScreenCaptureApi *minisfScreenCaptureApi();

#endif /* QPA_MINISF_SCREEN_CAPTURE_H */
