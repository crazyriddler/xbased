/*
 * hakuX native core, app front end (libhakuxcore.so): SDL3 window and input,
 * the core on its own thread, and the HLE GPU's frames presented (through
 * its Vulkan swapchain).
 * Independent of xemu: it talks to native/core and the HLE GPU's display only.
 */
#ifndef NATIVE_APP_APP_H
#define NATIVE_APP_APP_H

#include "native/core/nc.h"

struct ANativeWindow;

/* present.c — on the window's thread. */
bool nc_present_init(void);
/* Once per display refresh: the newest frame (or the last one) letterboxed
 * into win, waiting for the panel; false when nothing could be presented
 * (the HLE GPU not up yet, no window). */
bool nc_present_frame(struct ANativeWindow *win);
/* The picture's shape on screen: the frame's own (auto: 4:3 for 640x480),
 * 4:3, 16:9 (a widescreen title's anamorphic 640x480), or the whole window. */
enum { NC_ASPECT_AUTO, NC_ASPECT_4_3, NC_ASPECT_16_9, NC_ASPECT_STRETCH };
void nc_present_set_aspect(int mode);
/* The console is set to widescreen: auto shows a 4:3-sized frame as 16:9
 * (a widescreen title renders anamorphic into it). */
void nc_present_set_widescreen(bool on);
/* Overscan as a TV's: pct percent of the frame hidden past the screen's
 * edges (a CRT hid some 5-10%: 90% was the classic action-safe area), the
 * frame grown about its middle to do it; pct < 0 underscan, the frame
 * shrunk to leave that much border. Any shape (4:3, 16:9, ...). */
void nc_present_set_overscan(int pct);
/* The frame's borders cut off, h console pixels off each side and v off the
 * top and bottom (of 640x480; 0 none): a game's black edges hidden, the
 * rest filling the frame's place. */
void nc_present_set_crop(int h, int v);
/* FXAA over each frame before it's scaled to the window. */
void nc_present_set_fxaa(bool on);
void nc_present_set_smaa(bool on);     /* SMAA in place of FXAA */
void nc_present_set_interp(bool on);   /* an in-between frame for games slower than the refresh */
void nc_present_set_sharpen(int level); /* 0 off, 1 to 3: sharpening when no upscaler does it */
void nc_present_set_integer(bool on);  /* the picture a whole number of times the console's lines tall */
void nc_present_set_refresh(float hz);
/* Automatic texture upscaling: the disc (an image, or the game's folder) read on a thread of its own for the
 * textures it holds in XPR containers, which are written out to `dir` (the title's textures folder, with its
 * ending slash) for the upscaler (texscan.c). Once a disc; false when it was done before (or can't start). */
bool nc_texscan_start(const char *disc, const char *dir);
/* Automatic texture upscaling (texauto.c): the textures a game wrote out into `dir` (the title's folder, with its
 * ending slash) that have no upscaled picture yet, handed to Real-ESRGAN, which runs on by itself. Returns how
 * many (0 none to do, -1 the program missing or not started); note says which, for the log. */
int nc_texauto_run(const char *dir, const char *exe_dir, const char *data_dir, char *note, size_t note_n); /* the display's refresh rate (frame interpolation's limits) */
/* The frame rate in the window's corner. */
void nc_present_set_show_fps(bool on);
/* A frame smaller than its place in the window: scaled up bilinearly, or by
 * AMD FidelityFX Super Resolution 1 (a larger one is always averaged down). */
enum { NC_UPSCALE_BILINEAR, NC_UPSCALE_FSR, NC_UPSCALE_NEAREST, NC_UPSCALE_NIS };    /* swapchain.h HLE_UPSCALE_*'s */
void nc_present_set_upscale(int mode);
/* NVIDIA Image Scaling's sharpness, 0-100 percent (50: its default). */
void nc_present_set_nis_sharpness(int pct);
/* An "upscale=" value: "fsr", and "sgsr" (Snapdragon GSR, which FSR
 * replaced) as FSR; anything else bilinear. */
int nc_upscale_from_name(const char *v);
/* Frames the title presented per second over the last half second, their
 * average interval, and the last one's (ms). */
void nc_present_stats(float *fps, float *avg_ms, float *last_ms);
/* A title frame has been on screen (any thread). */
bool nc_present_shown(void);

/* vklayer.c — debug.hakux.vkvalidation=1: the Khronos validation layer in
 * front of the custom driver, its vkGetInstanceProcAddr returned in place of
 * the driver's (drv_gipa as it is otherwise, or without the layer). */
void *nc_vk_layers(void *drv_gipa);
/* vkdriver.c — "<driver dir>|<library>|<hook library dir>": the custom
 * Vulkan driver (Turnip) loaded through adrenotools and handed to the HLE
 * GPU (the system driver when it fails, or the build has no adrenotools). */
void nc_vk_driver_load(const char *spec);
/* The kgsl priority the loaded driver's draw contexts got (1 the highest,
 * 15 the lowest), 0 when none was set (no custom driver, not Turnip, or the
 * system driver after a failed load: the kernel's default, 8). */
int nc_vk_driver_kgsl_priority(void);

/* vsync.c — the panel's vsyncs to the emulated display's vblank clock. */
void nc_vsync_start(void);

#endif
