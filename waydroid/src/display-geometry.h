#ifndef DS_WAYDROID_DISPLAY_GEOMETRY_H
#define DS_WAYDROID_DISPLAY_GEOMETRY_H

#include <stdint.h>

#define DEFAULT_DISPLAY_WIDTH 1080
#define DEFAULT_DISPLAY_HEIGHT 1920

static inline uint32_t scaled_dim(uint32_t pixels, int percent)
{
    uint32_t scaled = (uint32_t)(((uint64_t)pixels * (uint32_t)percent + 50) / 100);
    scaled &= ~1u; /* Keep it even for Android's graphics stack. */
    return scaled < 64 ? 64 : scaled;
}

static inline int display_density(uint32_t width, uint32_t height, int width_dp)
{
    uint32_t shorter = width < height ? width : height;
    int density = (int)((shorter * 160u + (uint32_t)width_dp / 2) / (uint32_t)width_dp);
    if (density < 120)
        return 120;
    return density > 640 ? 640 : density;
}

#endif
