/*
 * Runtime-resolved screen-capture bridge exported by libminisf.
 *
 * Keep this declaration header independent from droidmedia so the QPA plugin
 * does not acquire a build-time media-stack dependency.
 */
#ifndef QPA_MINISF_SCREEN_CAPTURE_H
#define QPA_MINISF_SCREEN_CAPTURE_H

#include <stddef.h>

struct MinisfScreenCaptureApi {
    typedef void (*Init)(int width, int height, void **outQueue);
    typedef void *(*Producer)(void *queue);
    typedef void (*Destroy)(void *queue);
    typedef void *(*ConsumerNew)(void);
    typedef int (*GetDimensions)(int *width, int *height);

    Init init;
    Producer producer;
    Destroy destroy;
    ConsumerNew consumerNew;
    GetDimensions getDimensions;

    bool valid() const {
        return init && producer && destroy && consumerNew && getDimensions;
    }
};

const MinisfScreenCaptureApi *minisfScreenCaptureApi();

#endif /* QPA_MINISF_SCREEN_CAPTURE_H */
