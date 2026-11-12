/*
 * hakuX front end — the dashboard's drawing (ui.c): text in the bundled
 * font with the system's CJK fonts behind it, shapes, the console's green
 * theme, over an SDL_Renderer (Direct3D on Windows, OpenGL/Vulkan on Linux),
 * or, in a game's process, as the overlay its own presenter composites over
 * the picture (ui_init_overlay: the window stays the presenter's).
 */
#ifndef NATIVE_APP_UI_UI_H
#define NATIVE_APP_UI_UI_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint8_t r, g, b, a; } UiColor;
/* A texture of the interface's (the renderer's, or the overlay's). */
typedef struct UiTex UiTex;

/* The themes (the skins' colours): Green is the original Xbox dashboard's
 * greens on black, Neon is cyan and magenta on blue-black with cut corners,
 * Tron Red is red and amber on black, cut corners too. The UI_* colours are
 * the current theme's; a colour written out in the code as green (a literal
 * of the green theme) goes through ui_th, which turns it to the theme. */
typedef struct {
    UiColor green, green_hi, green_dim, green_dark, panel, white, grey, red;
    UiColor accent2;    /* the second colour: magenta, amber, a blue for green */
    UiColor ok;         /* status "good": never the accent */
    float hue;          /* degrees a green literal's hue is turned to, <0: not turned */
    bool cut;           /* the corners cut instead of rounded (Neon, Tron Red) */
} UiTheme;
enum { UI_THEME_GREEN, UI_THEME_NEON, UI_THEME_RED, UI_THEME_COUNT };
const UiTheme *ui_theme(void);
int ui_theme_id(void);
void ui_theme_set(int id);
int ui_theme_from_name(const char *name);   /* "green", "neon", "red"; unknown: green */
const char *ui_theme_name(int id);
/* A colour of the green theme turned to the current theme. */
UiColor ui_th(int r, int g, int b, int a);

#define UI_GREEN      (ui_theme()->green)
#define UI_GREEN_HI   (ui_theme()->green_hi)
#define UI_GREEN_DIM  (ui_theme()->green_dim)
#define UI_GREEN_DARK (ui_theme()->green_dark)
#define UI_PANEL      (ui_theme()->panel)
#define UI_WHITE      (ui_theme()->white)
#define UI_GREY       (ui_theme()->grey)
#define UI_RED        (ui_theme()->red)
#define UI_ACCENT2    (ui_theme()->accent2)
#define UI_OK         (ui_theme()->ok)
#define UI_BLACK      ((UiColor){0, 0, 0, 0xFF})

static inline UiColor ui_alpha(UiColor c, float a)
{
    c.a = (uint8_t)(c.a * (a < 0 ? 0 : a > 1 ? 1 : a));
    return c;
}
UiColor ui_mix(UiColor a, UiColor b, float t);

enum { UI_FONT_REGULAR, UI_FONT_BOLD, UI_FONTS };
enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER = 1, UI_ALIGN_RIGHT = 2 };

/* Up and down: the renderer for win, the fonts from font_dir (Play) and the
 * system's CJK fonts. */
bool ui_init(SDL_Window *win, const char *font_dir);
/* Or as a game's overlay: what is drawn becomes the presenter's overlay
 * (hw/xbox/hle/vk/swapchain.h) at ui_end, which then calls present (the
 * window's refresh); size gives the window's pixels. */
bool ui_init_overlay(const char *font_dir, void (*present)(void), void (*size)(int *w, int *h));
bool ui_is_overlay(void);
void ui_shutdown(void);
SDL_Renderer *ui_renderer(void);
/* The frame: its size (pixels) and scale (1 at 720 lines: sizes are given
 * in 720p units and scaled), then drawing, then ui_end presents. */
void ui_begin(void);
void ui_end(void);
/* The overlay's frame drawn so far set as the presenter's, without
 * presenting (the window's loop presents); cover: it draws the whole
 * window. ui_overlay_off: none any more. */
void ui_overlay_submit(bool cover);
void ui_overlay_off(void);
/* The next ui_end's overlay covers the window (each frame's own). */
void ui_cover(bool on);
/* Everything drawn from now on faded to a (1: as given). */
void ui_set_alpha(float a);
/* A picture of the frame being drawn, to path (a BMP), as it is shown. */
void ui_shot(const char *path);
float ui_w(void);   /* in 720p units */
float ui_h(void);
float ui_scale(void);
double ui_time(void);  /* seconds since ui_init */

/* Text in 720p units: size is the font's pixel height. */
float ui_text(float x, float y, const char *utf8, float size, int font, UiColor c, int align);
float ui_text_width(const char *utf8, float size, int font);
/* Text reading upward (a vertical strip's label): x the line's left edge, y
 * its bottom; its length. Not drawn in a game's overlay. */
float ui_text_up(float x, float y, const char *utf8, float size, int font, UiColor c);
/* Text cut to max_w with an ellipsis. */
float ui_text_fit(float x, float y, const char *utf8, float size, int font, UiColor c, int align, float max_w);

void ui_rect(float x, float y, float w, float h, UiColor c);
void ui_rect_grad(float x, float y, float w, float h, UiColor top, UiColor bottom);
/* A smooth curved band: spans xl[k]..xr[k] at y[k], shaded cl to cr across. */
#define UI_BAND_MAX 64
void ui_band(const float *xl, const float *xr, const float *y, int n, UiColor cl, UiColor cr);
void ui_rect_hgrad(float x, float y, float w, float h, UiColor left, UiColor right);
void ui_round(float x, float y, float w, float h, float r, UiColor c);
void ui_round_outline(float x, float y, float w, float h, float r, float t, UiColor c);
void ui_line(float x0, float y0, float x1, float y1, float t, UiColor c);
/* A soft radial glow (additive), radius r. */
void ui_glow(float cx, float cy, float r, UiColor c);
void ui_circle(float cx, float cy, float r, UiColor c);
void ui_ring(float cx, float cy, float rx, float ry, float rot, float t, UiColor c);
/* An image (texture) fitted into the box, keeping its shape. */
void ui_image(UiTex *t, float x, float y, float w, float h, float alpha);
UiTex *ui_texture_rgba(const uint8_t *rgba, int w, int h);
UiTex *ui_texture_surface(SDL_Surface *s);
void ui_texture_free(UiTex *t);
void ui_texture_size(const UiTex *t, float *w, float *h);
/* A texture over the whole window, its colour modulated by c. */
void ui_fill(UiTex *t, UiColor c);
/* The game's picture under the overlay, as the window would show it, or
 * that blurred (overlay only, while covering: NULL otherwise). */
UiTex *ui_scene(bool blurred);
void ui_clip(float x, float y, float w, float h);
void ui_unclip(void);

/* Layers: drawing into a transparent texture the window's size (a screen's
 * foreground), then that texture shown turned about the vertical axis
 * (angle, radians: 0 flat), pushed back (depth, in widths) and faded. */
UiTex *ui_layer_begin(UiTex *layer);
void ui_layer_end(void);
void ui_layer_draw3d(UiTex *layer, float angle, float depth, float alpha);

/* The transitions, over the whole window: t's picture breaking into
 * shards that fly at the viewer, a flash at the break, black behind (k 0
 * whole .. 1 gone); and the other way in, t's picture rushing in out of a
 * flash of colour from (k 0 .. 1 settled). */
void ui_fx_shatter(UiTex *t, float k);
void ui_fx_flash_in(UiTex *t, float k, UiColor from);

/* The console's animated backdrop: a green fog and drifting light. */
void ui_backdrop(float intensity);
/* The dashboard's orb (its centre and size). */
void ui_orb(float cx, float cy, float r, float pulse);

#endif
