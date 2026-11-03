/*
 * hakuX native core, app front end — the panel's vsync, for the emulated
 * display. A Choreographer thread hands each vsync's time to the NV2A's
 * vblank clock (native/dev/nv2a.c), which locks to the panel: a frame then
 * becomes the front image just before the presenter picks one up, instead of
 * two now and then falling in one refresh of a panel ticking beside it.
 */
#include "native/app/app.h"
#include "native/dev/dev.h"
#include "native/core/irq.h"

#include <pthread.h>

#ifndef __ANDROID__
/* Linux: no compositor's vblanks to follow; the emulated display's clock
 * runs free (nv2a.c, as with no panel) */
static void *vsync_thread(void *arg)
{
    return arg;
}
#else
#include <android/choreographer.h>
#include <android/looper.h>

static void on_frame(long frame_ns, void *data)
{
    nc_dev_nv2a_display_vsync((uint64_t)frame_ns);
    if (nc_paused()) {
        /* the emulation paused: a look now and then, not each vsync */
        AChoreographer_postFrameCallbackDelayed(AChoreographer_getInstance(), on_frame, data, 200);
        return;
    }
    AChoreographer_postFrameCallback(AChoreographer_getInstance(), on_frame, data);
}

static void *vsync_thread(void *arg)
{
    ALooper_prepare(0);
    AChoreographer *c = AChoreographer_getInstance();
    if (!c) {
        nc_log("hakuX-app", "vsync: no Choreographer; the vblank runs free");
        return NULL;
    }
    AChoreographer_postFrameCallback(c, on_frame, NULL);
    for (;;) {
        ALooper_pollOnce(-1, NULL, NULL, NULL);
    }
    return NULL;
}

#endif

void nc_vsync_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, vsync_thread, NULL) == 0) {
        pthread_detach(t);
    }
}
