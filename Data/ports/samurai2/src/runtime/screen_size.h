#ifndef NFSMW_SCREEN_SIZE_H
#define NFSMW_SCREEN_SIZE_H

#include <stdlib.h>

/* Offscreen GLES size. Shadowgun's HUD was authored for a wide frame; 640x480
   clipped the ammo counter. Default matches the TrimUI panel. */
static inline int nfsmw_screen_width(void)
{
    static int value;
    const char *configured;
    int parsed;

    if (value != 0)
        return value;
    configured = getenv("NFSMW_WIDTH");
    parsed = configured != NULL ? atoi(configured) : 1280;
    if (parsed < 160)
        parsed = 160;
    if (parsed > 1280)
        parsed = 1280;
    value = parsed;
    return value;
}

static inline int nfsmw_screen_height(void)
{
    static int value;
    const char *configured;
    int parsed;

    if (value != 0)
        return value;
    configured = getenv("NFSMW_HEIGHT");
    parsed = configured != NULL ? atoi(configured) : 720;
    if (parsed < 120)
        parsed = 120;
    if (parsed > 720)
        parsed = 720;
    value = parsed;
    return value;
}

#endif
