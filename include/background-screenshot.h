#ifndef _SWAY_BACKGROUND_SCREENSHOT_H
#define _SWAY_BACKGROUND_SCREENSHOT_H

#include "cairo.h"
#include "swaylock.h"

cairo_surface_t *load_background_screenshot(struct swaylock_state *state,
                                            struct swaylock_surface *surface);

#endif