/* xbased — picture settings chosen for the PC it runs on (hwtune.c). */
#ifndef NATIVE_APP_UI_HWTUNE_H
#define NATIVE_APP_UI_HWTUNE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "native/app/ui/config.h"

struct SDL_Window;

typedef struct HwTune {
    int scale, aniso, texcap;
    bool smaa, fxaa, fsr;
    bool texauto;               /* automatic texture upscaling: a discrete GPU of 6 GB or more */
} HwTune;

/* The settings for a GPU of this kind (2 discrete, 1 integrated, 0 none) and memory, on a display this high. */
void hwtune_pick(int kind, uint64_t vram, int display_h, HwTune *t);
/* Looks at the PC and writes its settings into c: the first time (no "hw.tuned") those not there yet, with
 * "hw.auto" set all of them. True when c changed (the caller saves it); note says what was found and set. */
bool hwtune_apply(Config *c, struct SDL_Window *win, char *note, size_t note_n);

#endif
