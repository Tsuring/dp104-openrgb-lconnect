#include "dp104_usb_stage.h"
#include <string.h>

int dp104_usb_stage_send(dp104_usb_stage *stage, dp104_usb_stage_hooks hooks,
                         uintptr_t port, const uint8_t *payload, size_t length)
{
    uint32_t saved;
    int accepted = 0;
    if(!stage || !payload || length != DP104_REPORT_BYTES || !hooks.enter ||
       !hooks.leave || !hooks.busy || !hooks.queue) return 0;
    saved = hooks.enter(hooks.context);
    if(stage->pending && !hooks.busy(hooks.context, stage->port)) stage->pending = 0;
    if(!stage->pending && !hooks.busy(hooks.context, port))
    {
        memmove(stage->words, payload, DP104_REPORT_BYTES);
        accepted = hooks.queue(hooks.context, port, (const uint8_t *)stage->words,
                               DP104_REPORT_BYTES);
        if(accepted)
        {
            stage->port = port;
            stage->pending = 1;
        }
    }
    hooks.leave(hooks.context, saved);
    return accepted != 0;
}
