/*
 * xbased front end — the skins (skin.h): each dashboard style's backdrop and
 * main screen, in the colours of the current theme. Everything is drawn with
 * the interface's own shapes (ui.h); the layouts follow the xemu Modern GUI's.
 */
#include "native/app/ui/skin.h"
#include "native/app/ui/dash_int.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* "42h 5m", "5m", "" under a minute: the play time under a game's title */
static const char *pt_text(int64_t s)
{
    static char b[4][32];
    static int k;
    k = (k + 1) & 3;
    if (s < 60) {
        b[k][0] = 0;
    } else if (s < 3600) {
        snprintf(b[k], sizeof(b[k]), "%dm", (int)(s / 60));
    } else {
        snprintf(b[k], sizeof(b[k]), "%dh %dm", (int)(s / 3600), (int)(s % 3600 / 60));
    }
    return b[k];
}

#define MAXI 8

static int g_style = SK_ORB;
static struct { float x, y, w, h; } R[MAXI];
static float g_sp;          /* the selection as a position (eased) */
static float g_open[MAXI];  /* each item's "selected" (eased 0..1) */

/* ------------------------------------------------------------ names */

static const struct {
    const char *name, *title;
} k_styles[SK_COUNT] = {
    {"orb", "xbased (Orb)"},       {"tabs", "Tabs"},
    {"classic", "Classic"},        {"blades", "Blades"},
    {"metro", "Metro"},            {"original", "Original Xbox"},
    {"oblades", "Original Blades"}, {"glossy", "Glossy Blades"},
    {"modern", "Modern"},          {"holo", "Holo"},
    {"aurora", "Aurora"},
};

const char *skin_name(int id)
{
    return k_styles[id >= 0 && id < SK_COUNT ? id : 0].name;
}

const char *skin_title(int id)
{
    return k_styles[id >= 0 && id < SK_COUNT ? id : 0].title;
}

int skin_from_name(const char *name)
{
    for (int i = 0; name && i < SK_COUNT; i++) {
        if (!strcmp(name, k_styles[i].name)) {
            return i;
        }
    }
    return SK_ORB;          /* xbased's own orb unless another is chosen */
}

void skin_set(const char *name)
{
    g_style = skin_from_name(name);
}

int skin_id(void)
{
    return g_style;
}

/* ------------------------------------------------------------ helpers */

static UiColor A(UiColor c, float a)
{
    return ui_alpha(c, a);
}

static bool over(float x, float y, float w, float h)
{
    return D.in.mx >= x && D.in.mx < x + w && D.in.my >= y && D.in.my < y + h;
}

/* mode 0 lower, 1 UPPER, 2 Title (the first letter capital, the rest as is: "Games") */
static const char *case_of(const char *s, int mode)
{
    static char b[8][96];
    static int k;
    k = (k + 1) & 7;
    size_t i = 0;
    for (; s[i] && i < sizeof(b[0]) - 1; i++) {
        const char c = s[i];
        b[k][i] = mode == 2 ? (i == 0 && c >= 'a' && c <= 'z' ? c - 32 : c)
                  : mode == 1 ? (c >= 'a' && c <= 'z' ? c - 32 : c)
                              : (c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    b[k][i] = 0;
    return b[k];
}
#define UP(s) case_of(s, 1)
#define LOW(s) case_of(s, 0)
#define TITLE(s) case_of(s, 2)

static float approach(float cur, float target, float k)
{
    return fabsf(target - cur) < 0.002f ? target : cur + (target - cur) * k;
}

/* The section colours (Blades, Metro, Modern, Glossy...): one per item. */
static UiColor sect(int id)
{
    static const uint8_t c[UI_THEME_COUNT][8][3] = {
        {{86, 178, 22}, {32, 132, 72}, {52, 120, 200}, {212, 132, 24}, {0, 150, 136}, {110, 120, 112}, {196, 64, 96}, {160, 90, 200}},
        {{0, 200, 230}, {40, 110, 255}, {150, 70, 255}, {255, 176, 40}, {57, 210, 130}, {110, 116, 150}, {255, 46, 136}, {255, 120, 60}},
        {{255, 52, 40}, {255, 96, 56}, {230, 40, 80}, {255, 176, 64}, {255, 140, 40}, {150, 112, 108}, {255, 214, 80}, {200, 80, 230}},
    };
    const uint8_t *p = c[ui_theme_id()][id >= 0 && id < 8 ? id : 0];
    return (UiColor){p[0], p[1], p[2], 255};
}

static UiColor dark_base(float mixk)
{
    return ui_mix(UI_GREEN_DARK, UI_BLACK, mixk);
}

static void tick(int n, int sel)
{
    memset(R, 0, sizeof(R));
    for (int i = 0; i < n && i < MAXI; i++) {
        g_open[i] = approach(g_open[i], i == sel ? 1.0f : 0.0f, 0.2f);
    }
    g_sp = approach(g_sp, (float)sel, 0.16f);
}

static void nav(int *sel, int n, bool vert, bool horiz)
{
    int d = 0;
    if (vert) {
        d = D.in.up ? -1 : D.in.down ? 1 : d;
    }
    if (horiz) {
        d = (D.in.left || D.in.lb) ? -1 : (D.in.right || D.in.rb) ? 1 : d;
    }
    if (d) {
        *sel = (*sel + d + n) % n;
    }
}

static void rect_set(int i, float x, float y, float w, float h)
{
    R[i].x = x;
    R[i].y = y;
    R[i].w = w;
    R[i].h = h;
}

static int finish(int n, int *sel)
{
    int chosen = -1;
    for (int i = 0; i < n; i++) {
        if (R[i].w > 0 && over(R[i].x, R[i].y, R[i].w, R[i].h)) {
            if (D.in.moved) {
                *sel = i;
            }
            if (D.in.click) {
                *sel = i;
                chosen = i;
            }
        }
    }
    if (D.in.ok) {
        chosen = *sel;
    }
    return chosen;
}

/* Games played, most recent first (else the first of the library). */
typedef struct {
    LibGame *g[12];
    int n;
    bool played;
} Rec;

static Rec recents_scan(void);

/* Several skins ask more than once a frame: the library is scanned once per frame. */
static Rec recents(void)
{
    static Rec cached;
    static double stamp = -1.0;
    const double now = ui_time();
    if (now != stamp) {
        cached = recents_scan();
        stamp = now;
    }
    return cached;
}

static Rec recents_scan(void)
{
    Rec r = {.n = 0, .played = false};
    static LibGame *all[1024];
    const int n = lib_list(all, 1024);
    for (int i = 0; i < n; i++) {
        if (all[i]->played > 0 && !all[i]->hidden) {
            int k = r.n < 12 ? r.n++ : 11;
            if (k == 11 && r.n == 12 && r.g[11] && r.g[11]->played >= all[i]->played) {
                continue;
            }
            r.g[k] = all[i];
            while (k > 0 && r.g[k - 1]->played < r.g[k]->played) {
                LibGame *t = r.g[k];
                r.g[k] = r.g[k - 1];
                r.g[k - 1] = t;
                k--;
            }
            r.played = true;
        }
    }
    if (!r.n) {
        for (int i = 0; i < n && r.n < 12; i++) {
            if (!all[i]->hidden) {
                r.g[r.n++] = all[i];
            }
        }
    }
    return r;
}

static void cover(LibGame *g, float x, float y, float w, float h, float a)
{
    UiTex *t = lib_cover(g);
    if (!t) {
        t = lib_icon(g);
    }
    ui_round(x, y, w, h, 6, A(UI_GREEN_DARK, a));
    if (t) {
        ui_image(t, x, y, w, h, a);
    }
}

/* A row of covers (box art), the first count of r: a click opens the game. */
static void strip(const Rec *r, float x, float y, float cw, float ch, float gap, int count, bool titles, UiColor ac)
{
    for (int i = 0; i < count && i < r->n; i++) {
        const float gx = x + i * (cw + gap);
        const bool hov = over(gx, y, cw, ch + (titles ? 40 : 0));
        cover(r->g[i], gx, y, cw, ch, 1.0f);
        if (hov) {
            ui_round_outline(gx - 2, y - 2, cw + 4, ch + 4, 8, 2.5f, ac);
            if (D.in.click) {
                dash_open_game(r->g[i]);
            }
        }
        if (titles) {
            ui_text_fit(gx, y + ch + 8, r->g[i]->g.title, 16, UI_FONT_REGULAR, hov ? UI_WHITE : UI_GREY, UI_ALIGN_LEFT,
                        cw);
        }
    }
}

static void wrap_text(float x, float y, float w, const char *s, float size, UiColor c, int max_lines)
{
    int lines = 0;
    while (*s && lines < max_lines) {
        char line[256];
        size_t n = 0, sp = 0;
        while (s[n] && n < sizeof(line) - 1) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (s[n] == ' ') {
                sp = n;
            }
            if (ui_text_width(line, size, UI_FONT_REGULAR) > w) {
                n = sp ? sp : n;
                break;
            }
            n++;
        }
        snprintf(line, sizeof(line), "%.*s", (int)n, s);
        ui_text(x, y, line, size, UI_FONT_REGULAR, c, UI_ALIGN_LEFT);
        s += n;
        while (*s == ' ') {
            s++;
        }
        y += size * 1.4f;
        lines++;
    }
}

static void clock_text(float right, float y, float size, UiColor c)
{
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    ui_text(right, y, dash_hm(tmv.tm_hour, tmv.tm_min), size, UI_FONT_BOLD, c, UI_ALIGN_RIGHT);
}

static void pool(float cx, float cy, float r, UiColor c, float a)
{
    ui_glow(cx, cy, r, A(c, a));
}

/* ------------------------------------------------------------ backdrops */

static void backdrop_flat(float k, UiColor pa, UiColor pb)
{
    const float W = ui_w(), H = ui_h();
    const double t = ui_time();
    ui_rect_grad(0, 0, W, H, dark_base(0.78f), dark_base(0.93f));
    const float m = k > 0 ? 1.0f : 0.0f;
    pool(W * (0.05f + 0.03f * sinf((float)t * 0.2f) * m), H * 0.02f, H * 1.0f, pa, 0.20f);
    pool(W * (0.02f + 0.02f * cosf((float)t * 0.17f) * m), H * 1.0f, H * 0.9f, pb, 0.22f);
    ui_rect_grad(0, H * 0.78f, W, H * 0.22f, (UiColor){0, 0, 0, 0}, (UiColor){0, 0, 0, 120});
}

static void backdrop_original(float k)
{
    const float W = ui_w(), H = ui_h();
    const double t = ui_time() * (k > 0 ? 1 : 0);
    ui_rect_grad(0, 0, W, H, ui_th(0, 2, 0, 0xFF), ui_th(6, 28, 6, 0xFF));
    pool(W * 0.72f, H * 0.5f, H * 0.95f, UI_GREEN, 0.14f);
    for (int l = 0; l < 3; l++) {
        const float base = H * (0.86f + l * 0.035f), amp = H * (0.035f + 0.008f * l);
        const float kk = (float)(2 * M_PI) / (W * (0.55f + 0.2f * l)), sp = 0.22f + 0.07f * l;
        float px = 0, py = 0;
        for (int i = 0; i <= 48; i++) {
            const float x = W * i / 48.0f;
            const float y = base + amp * sinf(x * kk + (float)t * sp + l * 1.7f) +
                            amp * 0.35f * sinf(x * kk * 2.3f - (float)t * sp * 1.4f);
            if (i) {
                ui_line(px, py, x, y, 7, A(UI_GREEN, 0.045f));
                ui_line(px, py, x, y, 2.5f, A(UI_GREEN, 0.20f));
            }
            px = x;
            py = y;
        }
    }
    for (int i = 0; i < 40; i++) {
        const float fx = fmodf(i * 0.6180339f, 1.0f), fz = fmodf(i * 0.3719f, 1.0f);
        const float drift = fmodf((float)(t * (0.004f + 0.006f * fz)) + fmodf(i * 0.2713f, 1.0f), 1.0f);
        ui_glow(W * fx + sinf((float)t * 0.3f + i) * 8, H * (1 - drift), 3 + 3 * fz,
                A(UI_GREEN_HI, 0.5f * sinf(drift * (float)M_PI)));
    }
}

static void backdrop_oblades(float k)
{
    const float W = ui_w(), H = ui_h();
    const double t = ui_time() * (k > 0 ? 1 : 0);
    ui_rect_grad(0, 0, W, H, ui_th(2, 40, 10, 0xFF), ui_th(0, 18, 4, 0xFF));
    pool(W * 0.3f, H * 0.5f, H * 1.1f, UI_GREEN, 0.18f);
    /* a web of curves over the field */
    for (int l = 0; l < 9; l++) {
        float px = 0, py = 0;
        for (int i = 0; i <= 40; i++) {
            const float x = W * i / 40.0f;
            const float y = H * (0.1f + 0.1f * l) + H * 0.07f * sinf(x * 0.006f + (float)t * 0.15f + l * 0.9f) +
                            H * 0.03f * sinf(x * 0.013f - (float)t * 0.1f + l);
            if (i) {
                ui_line(px, py, x, y, 1.2f, A(UI_GREEN_HI, 0.10f));
            }
            px = x;
            py = y;
        }
    }
}

static void backdrop_glossy(float k)
{
    const float W = ui_w(), H = ui_h();
    const float t = (float)ui_time() * (k > 0 ? 1 : 0);
    ui_rect_grad(0, 0, W, H, ui_th(120, 204, 66, 0xFF), ui_th(40, 128, 26, 0xFF));
    for (int i = 0; i < 6; i++) {
        pool(W * (0.5f + 0.42f * sinf(t * 0.06f + i * 1.9f)), H * (0.45f + 0.3f * cosf(t * 0.045f + i * 1.3f)), H * 0.7f,
             (UiColor){225, 255, 190, 255}, 0.16f);
    }
    ui_rect_grad(0, H * 0.7f, W, H * 0.3f, (UiColor){0, 0, 0, 0}, (UiColor){0, 0, 0, 90});
}

static void backdrop_holo(float k)
{
    const float W = ui_w(), H = ui_h();
    const float t = (float)ui_time() * (k > 0 ? 1 : 0);
    ui_rect_grad(0, 0, W, H, dark_base(0.88f), dark_base(0.97f));
    const float step = 30;
    for (int j = 1; j * step < H; j++) {
        for (int i = 1; i * step < W; i++) {
            ui_rect(i * step, j * step, 1.5f, 1.5f, A(UI_GREEN, 0.18f));
        }
    }
    if (k > 0) {
        const float y = fmodf(t * 36, H + 60) - 30;
        ui_rect_grad(0, y - 40, W, 40, A(UI_GREEN, 0), A(UI_GREEN, 0.07f));
    }
    ui_round_outline(14, 14, W - 28, H - 28, 18, 1.5f, A(UI_GREEN, 0.5f));
    ui_round_outline(22, 22, W - 44, H - 44, 14, 1.0f, A(UI_GREEN, 0.18f));
}

static void backdrop_aurora(float k)
{
    const float W = ui_w(), H = ui_h();
    const float t = (float)ui_time() * (k > 0 ? 1 : 0);
    ui_rect_grad(0, 0, W, H, ui_mix(UI_GREEN_DARK, UI_GREEN_DIM, 0.35f), ui_mix(UI_GREEN_DARK, UI_BLACK, 0.55f));
    pool(W * (0.86f + 0.03f * sinf(t * 0.1f)), H * 0.18f, H * 0.9f, UI_GREEN, 0.18f);
    pool(W * (0.18f + 0.04f * cosf(t * 0.08f)), H * 0.92f, H * 0.9f, UI_GREEN, 0.14f);
}

/* The library's covers drifting slowly up behind everything, each in a lane
 * of its own, nearer ones larger and brighter (Settings > Interface). */
static void floating_covers(void)
{
    if (!cfg_bool(&D.global, "ui.float", true)) {
        return;
    }
    static LibGame *all[256];
    const int total = lib_list(all, 256);
    int pool[256], np = 0;
    int budget = 1;     /* one cover read from disk a frame, not a stall */
    for (int i = 0; i < total && np < 9; i++) {
        LibGame *g = all[i];
        if (!g->hidden && !g->cover_tex && !g->cover_tried && budget-- > 0) {
            lib_cover(g);
        }
        if (!g->hidden && g->cover_tex) {
            pool[np++] = i;
        }
    }
    if (!np) {
        return;
    }
    const float W = ui_w(), H = ui_h(), t = (float)ui_time();
    const int count = np < 9 ? np : 9;
    for (int i = 0; i < count; i++) {
        const float depth = 0.55f + 0.45f * (float)((i * 5) % 9) / 8.0f;     /* 0.55..1 */
        const float h = (110.0f + 90.0f * depth) * (H / 720.0f), w = h * 0.78f;
        const float speed = 9.0f + 16.0f * depth;
        const float span = H + h * 2.0f;
        const float y = H + h - fmodf(t * speed + (float)i * span / count, span);
        const float x = ((float)i + 0.5f) / count * (W - w) + sinf(t * 0.25f + (float)i * 1.7f) * 26.0f;
        const float a = (0.05f + 0.07f * depth) * fminf(1.0f, fminf(y + h, H - y + h * 0.5f) / (H * 0.25f));
        UiTex *tx = all[pool[i % np]]->cover_tex;
        if (tx && a > 0.01f) {
            ui_image(tx, x, y, w, h, a);
        }
    }
}

void skin_backdrop(float k)
{
    switch (g_style) {
    case SK_ORB: ui_backdrop(k); break;
    case SK_ORIGINAL: backdrop_original(k); break;
    case SK_OBLADES: backdrop_oblades(k); break;
    case SK_GLOSSY: backdrop_glossy(k); break;
    case SK_HOLO: backdrop_holo(k); break;
    case SK_AURORA: backdrop_aurora(k); break;
    case SK_TABS: backdrop_flat(k, UI_GREEN, UI_ACCENT2); break;
    default: backdrop_flat(k, UI_GREEN, UI_GREEN_DIM); break;
    }
    if (k > 0.0f) {
        floating_covers();
    }
}

/* ------------------------------------------------------------ the styles */

/* xbased's own: green capsules fanned on the orb's right. */
static void st_orb(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    const bool tall = W < H * 1.1f;
    const float ox = tall ? W * 0.5f : W * 0.32f, oy = tall ? H * 0.27f : H * 0.5f;
    const float orb_r = tall ? fminf(H * 0.11f, W * 0.18f) : H * 0.13f;
    nav(sel, n, true, false);
    ui_orb(ox, oy, orb_r, 0);
    ui_text(ox, oy - orb_r * 0.36f, "xbased", orb_r * 0.56f, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_CENTER);
    const float bh = n > 5 ? 50 : 56;
    for (int i = 0; i < n; i++) {
        const float t = ease(D.anim * 3 - i * 0.12f);
        const float ang = (i - (n - 1) / 2.0f) * (n > 5 ? 0.27f : 0.32f);
        const float bw = tall ? W * 0.7f : W * 0.30f;
        const float bx = tall ? (W - bw) / 2 + (1 - t) * 60 : ox + orb_r * 1.5f + cosf(ang) * 40 - 30 + (1 - t) * 60;
        const float by = tall ? oy + orb_r * 1.9f + i * (bh + 14) : oy + sinf(ang) * orb_r * 2.9f - bh / 2;
        const bool s = *sel == i;
        rect_set(i, bx, by, bw, bh);
        if (!tall) {
            ui_line(ox + orb_r * 0.9f * cosf(ang), oy + orb_r * 0.9f * sinf(ang), bx, by + bh / 2, s ? 3 : 1.5f,
                    A(UI_GREEN, (s ? 0.8f : 0.35f) * t));
        }
        if (s) {
            ui_glow(bx + bw / 2, by + bh / 2, bw * 0.7f, A(UI_GREEN, 0.25f * t));
        }
        ui_round(bx, by, bw, bh, bh / 2, A(s ? ui_th(0x3C, 0x9A, 0x12, 0xFF) : ui_th(0x14, 0x3A, 0x08, 0xFF), 0.92f * t));
        ui_round_outline(bx, by, bw, bh, bh / 2, s ? 2.5f : 1.5f, A(s ? UI_GREEN_HI : UI_GREEN_DIM, t));
        ui_text(bx + 34, by + (bh - 32) / 2 - 2, it[i].label, 32, UI_FONT_BOLD, A(s ? UI_WHITE : UI_GREY, t),
                UI_ALIGN_LEFT);
    }
}

/* Tabs: the xemu dashboard's top bar of numbered tabs over a hero panel,
 * the recent games and the library's covers. */
static void st_tabs(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, false, true);
    if (D.in.up) {
        *sel = (*sel + n - 1) % n;
    } else if (D.in.down) {
        *sel = (*sel + 1) % n;
    }
    const UiColor ac = UI_GREEN, ac2 = UI_ACCENT2;
    ui_rect(0, 0, W, 84, (UiColor){0, 0, 0, 90});
    ui_line(0, 84, W, 84, 1, A(ac, 0.35f));
    /* the mark and name */
    ui_line(52, 44, 66, 30, 2, ac);
    ui_line(66, 30, 80, 44, 2, ac);
    ui_line(80, 44, 66, 58, 2, ac);
    ui_line(66, 58, 52, 44, 2, ac);
    ui_circle(66, 44, 4, ac2);
    ui_text(98, 28, "X B A S E D", 26, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    /* the tabs */
    float size = 24, total;
    do {
        total = 0;
        for (int i = 0; i < n; i++) {
            total += ui_text_width(TITLE(it[i].label), size, UI_FONT_REGULAR) + 70;
        }
        size -= 1;
    } while (total > W - 420 && size > 13);
    size += 1;
    float x = 290;
    for (int i = 0; i < n; i++) {
        const char *lab = TITLE(it[i].label);
        const float tw = ui_text_width(lab, size, UI_FONT_REGULAR), w = tw + 46;
        const bool s = *sel == i;
        rect_set(i, x - 8, 20, w + 16, 52);
        char num[8];
        snprintf(num, sizeof(num), "%02d", i + 1);
        ui_text(x, 34, num, size * 0.6f, UI_FONT_REGULAR, s ? ac2 : A(UI_GREY, 0.8f), UI_ALIGN_LEFT);
        ui_text(x + 28, 28, lab, size, UI_FONT_REGULAR, s ? UI_WHITE : UI_GREY, UI_ALIGN_LEFT);
        if (s) {
            ui_rect(x, 66, w - 6, 3, ac);
            ui_glow(x + w / 2, 66, w * 0.6f, A(ac, 0.25f));
        }
        x += w + 24;
    }
    clock_text(W - 40, 30, 22, ac);
    /* the hero */
    const Rec rec = recents();
    const float hx = 28, hy = 108, hw = W * 0.62f, hh = H * 0.43f;
    ui_round(hx, hy, hw, hh, 18, UI_PANEL);
    ui_round_outline(hx, hy, hw, hh, 18, 1.5f, A(ac, 0.35f));
    ui_rect(hx, hy + 18, 3, hh - 36, ac);
    const bool games = it[*sel].id == 0;
    if (games && rec.n) {
        LibGame *g = rec.g[0];
        ui_text(hx + 34, hy + hh - 150, rec.played ? "// CONTINUE PLAYING" : "// YOUR LIBRARY", 15, UI_FONT_REGULAR,
                ac, UI_ALIGN_LEFT);
        ui_text_fit(hx + 34, hy + hh - 128, g->g.title, 40, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT, hw * 0.6f);
        char sub[96];
        const char *ptx = pt_text(g->playtime);
        snprintf(sub, sizeof(sub), "%08X%s%s", g->g.title_id, ptx[0] ? "    " : "", ptx);
        ui_text(hx + 36, hy + hh - 78, sub, 17, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);
        const float ch = hh - 56, cw = ch * 0.72f;
        cover(g, hx + hw - cw - 50, hy + 28, cw, ch, 1.0f);
        if (over(hx + hw - cw - 50, hy + 28, cw, ch) && D.in.click) {
            dash_open_game(g);
        }
    } else {
        ui_text(hx + 34, hy + hh - 150, UP(it[*sel].label), 15, UI_FONT_REGULAR, ac, UI_ALIGN_LEFT);
        ui_text(hx + 34, hy + hh - 128, TITLE(it[*sel].label), 44, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
        wrap_text(hx + 36, hy + hh - 70, hw - 90, it[*sel].desc, 20, UI_GREY, 2);
    }
    /* the activity */
    const float ax = hx + hw + 18, aw = W - ax - 28;
    ui_round(ax, hy, aw, hh, 18, UI_PANEL);
    ui_round_outline(ax, hy, aw, hh, 18, 1.5f, A(ac, 0.2f));
    ui_text(ax + 24, hy + 18, "// ACTIVITY", 14, UI_FONT_REGULAR, ac, UI_ALIGN_LEFT);
    ui_text(ax + 24, hy + 38, T(S_RECENT), 26, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    for (int i = 0; i < rec.n && i < 4; i++) {
        const float ry = hy + 84 + i * 56;
        if (ry + 48 > hy + hh) {
            break;
        }
        cover(rec.g[i], ax + 24, ry, 40, 40, 1.0f);
        ui_text_fit(ax + 78, ry + 2, rec.g[i]->g.title, 20, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT, aw - 110);
        char sub[32];
        snprintf(sub, sizeof(sub), "%08X", rec.g[i]->g.title_id);
        ui_text(ax + 78, ry + 24, sub, 14, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);
        if (over(ax + 24, ry, aw - 48, 44) && D.in.click) {
            dash_open_game(rec.g[i]);
        }
    }
    /* the library's covers */
    const float ly = hy + hh + 26;
    char head[64];
    snprintf(head, sizeof(head), "// YOUR LIBRARY  -  %d", lib_count());
    ui_text(30, ly, head, 14, UI_FONT_REGULAR, ac, UI_ALIGN_LEFT);
    const float cw = fminf(128, (W - 56) / 7.0f - 14), ch = cw * 1.25f;
    const int cnt = (int)((W - 56) / (cw + 14));
    strip(&rec, 30, ly + 26, cw, ch, 14, cnt, true, ac);
}

/* Classic: a plain list on the left, the last game's cover and details. */
static void st_classic(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, true, false);
    const UiColor ac = UI_GREEN;
    ui_text(60, 40, "xbased", 56, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    ui_rect(62, 110, 160, 3, ac);
    const float rh = fminf(62, (H - 330) / n), y0 = 150;
    for (int i = 0; i < n; i++) {
        const bool s = *sel == i;
        const float y = y0 + i * rh;
        rect_set(i, 50, y, W * 0.38f, rh);
        if (s) {
            ui_rect_hgrad(50, y, W * 0.38f, rh - 6, A(ac, 0.30f), A(ac, 0.0f));
            ui_rect(50, y, 4, rh - 6, ac);
        }
        ui_text(76, y + (rh - 6 - 34) / 2, it[i].label, 34, s ? UI_FONT_BOLD : UI_FONT_REGULAR, s ? UI_WHITE : UI_GREY,
                UI_ALIGN_LEFT);
    }
    const Rec rec = recents();
    const float px = W * 0.46f, pw = W - px - 50;
    ui_text(px, 40, UP(it[*sel].label), 15, UI_FONT_REGULAR, ac, UI_ALIGN_LEFT);
    ui_text(px, 60, it[*sel].label, 44, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    wrap_text(px, 120, pw * 0.55f, it[*sel].desc, 21, UI_GREY, 3);
    if (rec.n) {
        const float ch = H * 0.46f, cw = ch * 0.72f;
        cover(rec.g[0], W - 50 - cw, 60, cw, ch, 1.0f);
        if (over(W - 50 - cw, 60, cw, ch) && D.in.click) {
            dash_open_game(rec.g[0]);
        }
        ui_text(px, H - 250, T(S_RECENT), 18, UI_FONT_BOLD, UI_GREY, UI_ALIGN_LEFT);
        strip(&rec, px, H - 220, 96, 120, 14, (int)((W - px - 50) / 110), true, ac);
    }
}

/* Blades: full-height panels side by side; the current one is wide, the
 * others slim strips with their name turned upward. */
static void st_blades(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, false, true);
    if (D.in.up) {
        *sel = (*sel + n - 1) % n;
    } else if (D.in.down) {
        *sel = (*sel + 1) % n;
    }
    ui_text(30, 14, "xbased", 26, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    clock_text(W - 30, 16, 22, UI_GREY);
    const float top = 62, bot = H - 82, sw = fminf(70, W * 0.055f), gap = 8;
    static float wd[MAXI];
    float x = 24;
    const float wide = W - 48 - (n - 1) * (sw + gap);
    const Rec rec = recents();
    for (int i = 0; i < n; i++) {
        wd[i] = wd[i] < 1 ? (i == *sel ? wide : sw) : approach(wd[i], i == *sel ? wide : sw, 0.2f);
        const float w = wd[i];
        const UiColor c = sect(it[i].id);
        const bool s = *sel == i;
        rect_set(i, x, top, w, bot - top);
        ui_round(x, top, w, bot - top, 14, ui_mix(dark_base(0.5f), c, s ? 0.20f : 0.10f));
        ui_rect_grad(x + 2, top + 2, w - 4, 90, A(c, s ? 0.45f : 0.22f), A(c, 0.0f));
        ui_round_outline(x, top, w, bot - top, 14, s ? 2 : 1, A(c, s ? 0.9f : 0.45f));
        ui_rect(x + 18, top + 18, 4, 28, s ? c : A(c, 0.8f));
        if (w < sw + 40) {
            ui_text_up(x + (w - 24) / 2, bot - 22, TITLE(it[i].label), 24, UI_FONT_REGULAR, A(UI_WHITE, 0.82f));
        } else {
            const float a = fminf(1.0f, (w - sw - 40) / 120.0f);
            ui_text(x + 40, top + 14, TITLE(it[i].label), 48, UI_FONT_BOLD, A(UI_WHITE, a), UI_ALIGN_LEFT);
            wrap_text(x + 42, top + 80, w - 90, it[i].desc, 21, A(UI_GREY, a), 2);
            if (it[i].id == 0 && rec.n && a > 0.8f) {
                const float cw = fminf(150, (w - 90) / 5.0f - 14), ch = cw * 1.28f;
                ui_text(x + 42, top + 150, T(S_RECENT), 17, UI_FONT_BOLD, c, UI_ALIGN_LEFT);
                strip(&rec, x + 42, top + 180, cw, ch, 14, (int)((w - 90) / (cw + 14)), true, c);
            }
        }
        x += w + gap;
    }
}

/* Metro: big lowercase section names across the top, the page under. */
static void st_metro(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, false, true);
    if (D.in.up) {
        *sel = (*sel + n - 1) % n;
    } else if (D.in.down) {
        *sel = (*sel + 1) % n;
    }
    clock_text(W - 40, 22, 24, UI_GREY);
    float size = 60, total;
    do {
        total = 0;
        for (int i = 0; i < n; i++) {
            total += ui_text_width(TITLE(it[i].label), size, UI_FONT_REGULAR) + 30;
        }
        size -= 2;
    } while (total > W - 80 && size > 22);
    float x = 40;
    for (int i = 0; i < n; i++) {
        const bool s = *sel == i;
        const float w = ui_text_width(TITLE(it[i].label), size, UI_FONT_REGULAR);
        rect_set(i, x - 6, 50, w + 12, size + 24);
        ui_text(x, 56, TITLE(it[i].label), size, UI_FONT_REGULAR, s ? UI_WHITE : A(UI_GREY, 0.6f), UI_ALIGN_LEFT);
        x += w + 30;
    }
    const UiColor ac = sect(it[*sel].id);
    const Rec rec = recents();
    const float ty = 56 + size + 54;
    if (it[*sel].id == 0 && rec.n) {
        ui_text(40, ty, UP(T(S_RECENT)), 15, UI_FONT_BOLD, UI_GREY, UI_ALIGN_LEFT);
        const float rh = fminf(40, (H - ty - 130) / (float)(rec.n > 9 ? 9 : rec.n));
        for (int i = 0; i < rec.n && i < 9; i++) {
            const float y = ty + 28 + i * rh;
            const bool hov = over(36, y, W * 0.38f, rh);
            if (i == 0 || hov) {
                ui_rect_hgrad(36, y, W * 0.38f, rh - 2, A(ac, i == 0 ? 0.35f : 0.2f), A(ac, 0));
                ui_rect(36, y, 3, rh - 2, ac);
            }
            ui_text_fit(52, y + (rh - 26) / 2 - 1, rec.g[i]->g.title, 24, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT,
                        W * 0.36f);
            if (hov && D.in.click) {
                dash_open_game(rec.g[i]);
            }
        }
        const float ch = H * 0.52f, cw = ch * 0.72f;
        cover(rec.g[0], W - 70 - cw, ty - 10, cw, ch, 1.0f);
        ui_text(W * 0.45f, ty + 6, rec.g[0]->g.title, 38, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT);
        char sub[32];
        const char *ptx = pt_text(rec.g[0]->playtime);
        snprintf(sub, sizeof(sub), "%s  %08X%s%s", T(S_TITLE_ID), rec.g[0]->g.title_id, ptx[0] ? "    " : "", ptx);
        ui_text(W * 0.45f, ty + 56, sub, 18, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);
    } else {
        ui_text(40, ty, UP(it[*sel].label), 15, UI_FONT_BOLD, UI_GREY, UI_ALIGN_LEFT);
        ui_text(40, ty + 26, it[*sel].label, 54, UI_FONT_REGULAR, UI_WHITE, UI_ALIGN_LEFT);
        wrap_text(42, ty + 100, W * 0.5f, it[*sel].desc, 24, UI_GREY, 3);
    }
    ui_rect(40, ty - 14, 70, 3, ac);
}

/* Original Xbox: the globe, a list with a bar that slides to the row. */
static void st_original(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, true, false);
    const bool tall = W < H * 1.1f;
    const float gcx = tall ? W * 0.5f : W * 0.72f, gcy = tall ? H * 0.2f : H * 0.5f;
    ui_orb(gcx, gcy, tall ? fminf(H * 0.1f, W * 0.2f) : fminf(H * 0.25f, W * 0.17f), 0);
    ui_text(W * 0.08f, 26, "xbased", 30, UI_FONT_BOLD, UI_GREEN, UI_ALIGN_LEFT);
    const float rh = fminf(70, H * 0.55f / n), size = rh * 0.56f, x0 = W * 0.08f;
    const float y0 = tall ? H * 0.38f : (H - rh * n) / 2 - 10, rw = W * (tall ? 0.8f : 0.40f);
    const float by = y0 + g_sp * rh;
    ui_rect_hgrad(x0 - 14, by, rw, rh, A(UI_GREEN, 0.38f), A(UI_GREEN, 0));
    ui_rect(x0 - 14, by + 4, 4, rh - 8, UI_GREEN);
    for (int i = 0; i < n; i++) {
        const bool s = *sel == i;
        rect_set(i, x0 - 14, y0 + i * rh, rw, rh);
        ui_text(x0, y0 + i * rh + (rh - size) / 2 - 2, TITLE(it[i].label), size, UI_FONT_REGULAR,
                s ? UI_WHITE : A(UI_WHITE, 0.55f), UI_ALIGN_LEFT);
    }
    wrap_text(x0, H - 138, W * 0.5f, it[*sel].desc, 22, UI_GREY, 2);
}

/* Original Blades: a sphere wrapped in fins, an arc of glass orbs, each with
 * its bar; the orbs turn round the sphere as the selection moves. */
static void st_oblades(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, true, false);
    const double t = ui_time();
    const bool tall = W < H * 1.1f;
    const float R0 = tall ? fminf(H * 0.1f, W * 0.2f) : fminf(H * 0.22f, W * 0.15f);
    const float cx = tall ? W * 0.5f : W * 0.27f, cy = tall ? H * 0.22f : H * 0.5f;
    ui_orb(cx, cy, R0, 0);
    for (int i = 0; i < 5; i++) {
        const float rot = (float)(t * (0.15 + i * 0.05) + i * 1.3);
        ui_ring(cx, cy, R0 * 1.55f, R0 * (0.25f + i * 0.09f), rot, 12, A(UI_GREEN, 0.08f));
        ui_ring(cx, cy, R0 * 1.55f, R0 * (0.25f + i * 0.09f), rot, 2, A(UI_GREEN_HI, 0.45f));
    }
    ui_text(W * 0.04f, 26, "xbased", 28, UI_FONT_BOLD, UI_GREEN_HI, UI_ALIGN_LEFT);
    for (int i = 0; i < n; i++) {
        /* the orbs on an arc round the sphere, the selected one a little
         * nearer the middle; they glide as the selection moves */
        const float step = fminf(76.0f, (H - 200) / n);
        const float Ra = tall ? H * 0.4f : R0 * 2.7f;
        float dy = (i - (n - 1) / 2.0f) * step - (g_sp - (n - 1) / 2.0f) * 10.0f;
        dy = fmaxf(-Ra * 0.92f, fminf(Ra * 0.92f, dy));
        const float px = tall ? cx - Ra * 0.5f + Ra * 0.5f : cx + sqrtf(Ra * Ra - dy * dy);
        const float py = tall ? cy + R0 * 2.0f + 70 + (i - (n - 1) / 2.0f) * step + 100 : cy + dy;
        const bool s = *sel == i;
        const float vis = 1.0f;
        const float r = 22 + 12 * g_open[i];
        ui_glow(px, py, r * 2.4f, A(UI_GREEN, (s ? 0.5f : 0.18f) * vis));
        ui_circle(px, py, r, A(ui_th(0x10, 0x40, 0x08, 0xFF), vis));
        ui_ring(px, py, r, r, 0, 2, A(s ? UI_GREEN_HI : UI_GREEN_DIM, vis));
        ui_glow(px - r * 0.3f, py - r * 0.35f, r * 0.6f, A((UiColor){255, 255, 255, 255}, 0.4f * vis));
        const float bx = px + r + 14, bw = fminf(W * 0.30f, W - bx - 30), bh = 44;
        rect_set(i, px - r, py - bh / 2, bw + r * 2 + 14, bh);
        ui_round(bx, py - bh / 2, bw, bh, 12, A(s ? ui_th(0x3C, 0x9A, 0x12, 0xFF) : ui_th(0x10, 0x30, 0x08, 0xFF), 0.9f * vis));
        ui_round_outline(bx, py - bh / 2, bw, bh, 12, s ? 2 : 1, A(s ? UI_GREEN_HI : UI_GREEN_DIM, vis));
        ui_text(bx + 22, py - 16, TITLE(it[i].label), 30, UI_FONT_BOLD, A(s ? UI_WHITE : UI_GREY, vis), UI_ALIGN_LEFT);
    }
    wrap_text(W * 0.04f, 66, W * 0.34f, it[*sel].desc, 22, UI_GREY, 3);
}

/* One blade, as the Xbox 360's first dashboard had them: a curved strip
 * bowed toward the inside. left: the strip sits in the left stack. */
static void blade_shape(float x, float sw, float bow, float top, float bot, bool left, UiColor body, UiColor edge)
{
    /* one smooth outline (the bow's sine, 48 points) drawn as bands: the body,
     * a soft highlight down its inner half, the lit edge */
    enum { N = 48 };
    float xl[N], xr[N], xe[N], xh[N], y[N];
    for (int k = 0; k < N; k++) {
        const float u = (float)k / (N - 1);
        const float bw = bow * powf(fmaxf(0.0f, sinf(u * (float)M_PI)), 0.8f);
        y[k] = top + (bot - top) * u;
        xl[k] = left ? x : x - bw;
        xr[k] = left ? x + sw + bw : x + sw;
        xe[k] = left ? xr[k] - 3 : xl[k];
        xh[k] = xl[k] + (xr[k] - xl[k]) * 0.55f;
    }
    ui_band(xl, xr, y, N, body, body);
    ui_band(xl, xh, y, N, (UiColor){255, 255, 255, 70}, (UiColor){255, 255, 255, 0});
    float xe2[N];
    for (int k = 0; k < N; k++) {
        xe2[k] = xe[k] + 3;
    }
    ui_band(xe, xe2, y, N, edge, edge);
}

static void st_glossy(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, false, true);
    if (D.in.up) {
        *sel = (*sel + n - 1) % n;
    } else if (D.in.down) {
        *sel = (*sel + 1) % n;
    }
    const float sw = fminf(54, W * 0.036f), bow = 18, top = 0, bot = H - 78;
    static float cx[MAXI];
    static bool init;
    for (int i = 0; i < n; i++) {
        const float target = i <= *sel ? i * sw : W - (n - i) * sw;
        cx[i] = init ? approach(cx[i], target, 0.22f) : target;
    }
    init = true;
    /* the middle: the section's title */
    const float mx0 = (*sel + 1) * sw + bow + 30, mx1 = W - (n - 1 - *sel) * sw - bow - 30;
    const float mw = mx1 - mx0;
    ui_text(mx0 + 10, H * 0.17f + 3, TITLE(it[*sel].label), 78, UI_FONT_BOLD, (UiColor){0, 40, 0, 90}, UI_ALIGN_LEFT);
    ui_text(mx0 + 8, H * 0.17f, TITLE(it[*sel].label), 78, UI_FONT_BOLD, (UiColor){255, 255, 255, 255}, UI_ALIGN_LEFT);
    if (mw > 200) {
        wrap_text(mx0 + 12, H * 0.17f + 100, mw - 30, it[*sel].desc, 25, (UiColor){235, 255, 225, 255}, 3);
        const Rec rec = recents();
        if (it[*sel].id == 0 && rec.n) {
            const float cw = fminf(140, mw / 5.0f - 14), ch = cw * 1.28f;
            strip(&rec, mx0 + 12, H * 0.17f + 190, cw, ch, 14, (int)(mw / (cw + 14)), false, (UiColor){255, 255, 255, 255});
        }
    }
    /* the blades: the left stack from the current one outward, so each
     * covers the one before; the right stack the other way */
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < n; k++) {
            const int i = pass == 0 ? *sel - k : *sel + 1 + k;
            if (i < 0 || i >= n) {
                continue;
            }
            const bool on = i == *sel, left = i <= *sel;
            const UiColor body = on ? ui_th(58, 150, 32, 255) : (UiColor){168, 175, 170, 255};
            const UiColor edge = on ? ui_th(168, 236, 110, 255) : (UiColor){250, 251, 250, 255};
            blade_shape(cx[i], sw, bow, top, bot, left, body, edge);
            ui_text_up(cx[i] + (sw - 22) / 2 + (left ? 0 : 0), bot - 24, TITLE(it[i].label), 22, UI_FONT_BOLD,
                       on ? (UiColor){255, 255, 255, 255} : (UiColor){52, 62, 56, 255});
            rect_set(i, cx[i], top, sw + bow, bot - top);
        }
    }
    ui_rect_grad(0, bot, W, H - bot, ui_th(24, 92, 18, 240), ui_th(8, 52, 10, 248));
    ui_line(0, bot, W, bot, 1.5f, (UiColor){220, 255, 190, 90});
}

/* Modern: a side menu and one calm card, the accent following the section. */
static void st_modern(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, true, false);
    static UiColor acc;
    static bool init;
    const UiColor tgt = sect(it[*sel].id);
    if (!init) {
        acc = tgt;
        init = true;
    }
    acc.r = (uint8_t)(acc.r + (tgt.r - acc.r) * 0.15f);
    acc.g = (uint8_t)(acc.g + (tgt.g - acc.g) * 0.15f);
    acc.b = (uint8_t)(acc.b + (tgt.b - acc.b) * 0.15f);
    pool(W * 0.75f, H * 0.2f, H * 0.9f, acc, 0.16f);
    const float sx = 24, sy = 24, sw = fminf(310, W * 0.26f), sh = H - 110;
    ui_round(sx, sy, sw, sh, 22, A(dark_base(0.55f), 0.92f));
    ui_round_outline(sx, sy, sw, sh, 22, 1, A(UI_WHITE, 0.08f));
    ui_circle(sx + 46, sy + 52, 22, acc);
    ui_text(sx + 46, sy + 36, "x", 32, UI_FONT_BOLD, UI_BLACK, UI_ALIGN_CENTER);
    ui_text(sx + 82, sy + 34, "xbased", 28, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    const float rh = fminf(62, (sh - 120) / n);
    for (int i = 0; i < n; i++) {
        const float y = sy + 100 + i * rh;
        const bool s = *sel == i;
        const UiColor c = sect(it[i].id);
        rect_set(i, sx + 10, y, sw - 20, rh - 6);
        if (s) {
            ui_round(sx + 10, y, sw - 20, rh - 6, 16, A(c, 0.22f));
            ui_round_outline(sx + 10, y, sw - 20, rh - 6, 16, 1.5f, A(c, 0.7f));
        }
        ui_circle(sx + 46, y + (rh - 6) / 2, 17, s ? c : A(c, 0.45f));
        char ini[3] = {it[i].label[0], it[i].label[0] == 'O' && it[i].label[1] == 'G' ? 'G' : 0, 0};
        ui_text(sx + 46, y + (rh - 6) / 2 - 12, ini, 22, UI_FONT_BOLD, UI_BLACK, UI_ALIGN_CENTER);
        ui_text(sx + 78, y + (rh - 6 - 26) / 2, it[i].label, 26, s ? UI_FONT_BOLD : UI_FONT_REGULAR,
                s ? UI_WHITE : UI_GREY, UI_ALIGN_LEFT);
    }
    const float cx0 = sx + sw + 20, cw = W - cx0 - 24;
    ui_round(cx0, sy, cw, sh, 22, A(dark_base(0.6f), 0.9f));
    ui_round_outline(cx0, sy, cw, sh, 22, 1, A(UI_WHITE, 0.08f));
    ui_rect(cx0 + 34, sy + 36, 5, 44, acc);
    ui_text(cx0 + 54, sy + 26, it[*sel].label, 52, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    wrap_text(cx0 + 56, sy + 98, cw - 110, it[*sel].desc, 23, UI_GREY, 2);
    const Rec rec = recents();
    if (it[*sel].id == 0 && rec.n) {
        ui_text(cx0 + 56, sy + 170, T(S_RECENT), 19, UI_FONT_BOLD, acc, UI_ALIGN_LEFT);
        const float cwd = fminf(150, (cw - 110) / 5.0f - 14);
        strip(&rec, cx0 + 56, sy + 202, cwd, cwd * 1.28f, 14, (int)((cw - 110) / (cwd + 14)), true, acc);
    }
}

/* Holo: black glass, HUD cards in a row, a details panel under them. */
static void st_holo(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, false, true);
    if (D.in.up) {
        *sel = (*sel + n - 1) % n;
    } else if (D.in.down) {
        *sel = (*sel + 1) % n;
    }
    const UiColor ac = UI_GREEN, ac2 = UI_ACCENT2;
    ui_text(54, 40, "X B A S E D", 24, UI_FONT_BOLD, ac, UI_ALIGN_LEFT);
    ui_text(54, 68, "// DASHBOARD", 14, UI_FONT_REGULAR, A(ac, 0.7f), UI_ALIGN_LEFT);
    clock_text(W - 54, 42, 24, ac);
    const float cw = fminf(200, (W - 140) / n - 14), ch = 150, gap = 14;
    const float x0 = (W - (cw * n + gap * (n - 1))) / 2, y0 = H * 0.2f;
    for (int i = 0; i < n; i++) {
        const bool s = *sel == i;
        const float lift = -16 * g_open[i];
        const float x = x0 + i * (cw + gap), y = y0 + lift;
        rect_set(i, x, y0 - 16, cw, ch + 16);
        ui_round(x, y, cw, ch, 14, A(ac, s ? 0.14f : 0.05f));
        ui_round_outline(x, y, cw, ch, 14, s ? 2.2f : 1.0f, A(ac, s ? 1.0f : 0.4f));
        if (s) {
            ui_glow(x + cw / 2, y + ch / 2, cw * 0.8f, A(ac, 0.16f));
            for (float ly = y + 6; ly < y + ch - 6; ly += 5) {
                ui_line(x + 10, ly, x + cw - 10, ly, 1, A(ac, 0.07f));
            }
        }
        const float hx = x + cw / 2, hy = y + 58, hr = 30;
        for (int k = 0; k < 6; k++) {
            const float a0 = (float)(k * M_PI / 3 + M_PI / 6), a1 = (float)((k + 1) * M_PI / 3 + M_PI / 6);
            ui_line(hx + cosf(a0) * hr, hy + sinf(a0) * hr, hx + cosf(a1) * hr, hy + sinf(a1) * hr, s ? 2.2f : 1.4f,
                    s ? ac : A(ac, 0.6f));
        }
        char ini[3] = {it[i].label[0], it[i].label[0] == 'O' && it[i].label[1] == 'G' ? 'G' : 0, 0};
        ui_text(hx, hy - 17, ini, 30, UI_FONT_BOLD, s ? ac2 : A(UI_WHITE, 0.8f), UI_ALIGN_CENTER);
        {
            /* a long label (OG Dashboard) is set smaller to fit the tile, not cut short */
            const char *tl = TITLE(it[i].label);
            float fs = 21, lw = ui_text_width(tl, fs, UI_FONT_BOLD);
            if (lw > cw - 16) {
                fs = fmaxf(13, fs * (cw - 16) / lw);
            }
            ui_text_fit(hx, y + ch - 42 + (21 - fs) / 2, tl, fs, UI_FONT_BOLD, s ? UI_WHITE : UI_GREY,
                        UI_ALIGN_CENTER, cw - 16);
        }
    }
    const float py = y0 + ch + 36, ph = H - py - 100, px = x0, pw = W - 2 * x0;
    ui_round(px, py, pw, ph, 16, A(ac, 0.05f));
    ui_round_outline(px, py, pw, ph, 16, 1.2f, A(ac, 0.45f));
    ui_text(px + 28, py + 22, "// SECTION", 14, UI_FONT_REGULAR, ac, UI_ALIGN_LEFT);
    ui_text(px + 28, py + 44, TITLE(it[*sel].label), 44, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    wrap_text(px + 30, py + 104, pw * 0.5f, it[*sel].desc, 21, UI_GREY, 2);
    const Rec rec = recents();
    if (it[*sel].id == 0 && rec.n) {
        const float chh = ph - 70, cwd = chh * 0.74f;
        strip(&rec, px + pw - 28 - 4 * (cwd + 14) + 14, py + 24, cwd, chh - 30, 14, 4, false, ac);
    }
}

/* Aurora: deep teal glass, the last game's art behind, a bar of pills on
 * top (the current one opens into its name), one rounded card. */
static void st_aurora(const SkinItem *it, int n, int *sel)
{
    const float W = ui_w(), H = ui_h();
    nav(sel, n, false, true);
    if (D.in.up) {
        *sel = (*sel + n - 1) % n;
    } else if (D.in.down) {
        *sel = (*sel + 1) % n;
    }
    const UiColor ac = UI_GREEN;
    const Rec rec = recents();
    if (rec.n) {
        UiTex *t = lib_cover(rec.g[0]);
        if (t) {
            ui_image(t, W * 0.3f, -H * 0.1f, W * 0.7f, H * 1.2f, 0.16f);
            ui_rect_grad(0, 0, W, H, A(UI_GREEN_DARK, 0.5f), A(dark_base(0.8f), 0.93f));
        }
    }
    ui_circle(62, 54, 27, A(ac, 0.25f));
    ui_circle(62, 54, 22, ac);
    ui_text(62, 38, "x", 32, UI_FONT_BOLD, UI_BLACK, UI_ALIGN_CENTER);
    ui_text(102, 30, "xbased", 24, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    char sub[48];
    snprintf(sub, sizeof(sub), "%d", lib_count());
    ui_text(103, 58, sub, 17, UI_FONT_REGULAR, A(ac, 0.85f), UI_ALIGN_LEFT);
    float total = 0;
    for (int i = 0; i < n; i++) {
        total += (i == *sel ? ui_text_width(it[i].label, 22, UI_FONT_BOLD) + 70 : 52) + 10;
    }
    float x = W - 40 - total - 80;
    if (x < 230) {
        x = 230;
    }
    for (int i = 0; i < n; i++) {
        const bool s = *sel == i;
        const float w = s ? ui_text_width(it[i].label, 22, UI_FONT_BOLD) + 70 : 52;
        rect_set(i, x, 28, w, 52);
        ui_round(x, 28, w, 52, 26, s ? A(ac, 0.9f) : A(UI_WHITE, 0.09f));
        char ini[3] = {it[i].label[0], it[i].label[0] == 'O' && it[i].label[1] == 'G' ? 'G' : 0, 0};
        ui_text(x + 26, 40, ini, 24, UI_FONT_BOLD, s ? UI_BLACK : UI_GREY, UI_ALIGN_CENTER);
        if (s) {
            ui_text(x + 48, 40, it[i].label, 22, UI_FONT_BOLD, UI_BLACK, UI_ALIGN_LEFT);
        }
        x += w + 10;
    }
    const float cx = 28, cy = 108, cw = W - 56, ch = H - cy - 100;
    ui_round(cx, cy, cw, ch, 30, A(dark_base(0.6f), 0.62f));
    ui_round_outline(cx, cy, cw, ch, 30, 1.2f, A(UI_WHITE, 0.10f));
    ui_text(cx + 40, cy + 28, it[*sel].label, 54, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    wrap_text(cx + 42, cy + 100, cw * 0.6f, it[*sel].desc, 23, UI_GREY, 2);
    if (it[*sel].id == 0 && rec.n) {
        ui_text(cx + 42, cy + 170, T(S_RECENT), 19, UI_FONT_BOLD, ac, UI_ALIGN_LEFT);
        const float chh = ch - 250, cwd = chh * 0.74f;
        strip(&rec, cx + 42, cy + 204, cwd, chh, 16, (int)((cw - 84) / (cwd + 16)), true, ac);
    }
}

int skin_main(const SkinItem *it, int n, int *sel)
{
    if (n > MAXI) {
        n = MAXI;
    }
    *sel = *sel < 0 ? 0 : *sel >= n ? n - 1 : *sel;
    tick(n, *sel);
    switch (g_style) {
    case SK_TABS: st_tabs(it, n, sel); break;
    case SK_CLASSIC: st_classic(it, n, sel); break;
    case SK_BLADES: st_blades(it, n, sel); break;
    case SK_METRO: st_metro(it, n, sel); break;
    case SK_ORIGINAL: st_original(it, n, sel); break;
    case SK_OBLADES: st_oblades(it, n, sel); break;
    case SK_GLOSSY: st_glossy(it, n, sel); break;
    case SK_MODERN: st_modern(it, n, sel); break;
    case SK_HOLO: st_holo(it, n, sel); break;
    case SK_AURORA: st_aurora(it, n, sel); break;
    default: st_orb(it, n, sel); break;
    }
    return finish(n, sel);
}
