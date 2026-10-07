#ifndef TRIERARCH_ANDROID_PRESENTER_PROBE_H
#define TRIERARCH_ANDROID_PRESENTER_PROBE_H

#include <android/native_window.h>

/*
 * Performs one non-owning dequeue/queue round trip before EGL connects to the
 * window.  It is deliberately only a feasibility probe for the Android-owned
 * presenter design; it never retains or exports a buffer fd.
 */
void trierarch_android_presenter_probe(ANativeWindow *window);

#endif
