/*
 * xbased front end — the Homebrew tab and the store's screens (homebrew.h).
 */
#include "native/app/ui/homebrew.h"

#include "native/app/ui/dash_int.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void wrap(float x, float y, float w, const char *s, float size, UiColor c, int max_lines)
{
    int lines = 0;
    while (*s && lines < max_lines) {
        char line[320];
        size_t n = 0, sp = 0;
        while (s[n] && n < sizeof(line) - 1) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (s[n] == '\n') {
                break;
            }
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
        while (*s == ' ' || *s == '\n') {
            s++;
        }
        y += size * 1.35f;
        lines++;
    }
}

static void help_panel(const char *text)
{
    const float W = ui_w(), H = ui_h(), hy = H - 176;
    ui_round(60, hy, W - 120, 88, 12, UI_PANEL);
    wrap(80, hy + 12, W - 160, text, 19, UI_WHITE, 3);
}

/* ------------------------------------------------------------ installed */

static int g_del;   /* the app asked about */

static void delete_yes(Dash *d)
{
    (void)d;
    hb_delete(g_del);
}

void scr_homebrew(void)
{
    const float W = ui_w(), H = ui_h();
    char sub[64];
    if (D.anim < 0.03f) {
        hb_scan();
    }
    snprintf(sub, sizeof(sub), "%d", hb_count());
    draw_header(T(S_HOMEBREW), hb_count() ? sub : NULL);
    D.editing_game = false;
    const bool busy = hb_busy();

    List l;
    list_begin(&l, 74, 130, W - 148, H - 130 - 190, 58);
    const char *help = T(S_HB_HELP);
    if (list_row(&l, T(S_HB_STORE), ">", NULL) && !busy) {
        st_start();
        D.sel[SCR_STORE] = D.scroll[SCR_STORE] = 0;
        dash_push(SCR_STORE);
    }
    if (list_selected(&l)) {
        help = T(S_HB_STORE_HELP);
    }
    for (int i = 0; i < hb_count(); i++) {
        HbApp *a = hb_get(i);
        if (list_row_icon(&l, hb_icon(a), a->name, a->group, NULL, NULL) && !busy) {
            hb_launch(i);
        }
        if (list_selected(&l)) {
            help = T(S_HB_PLAY_HELP);
            if (D.in.x && !busy) {
                g_del = i;
                dash_ask(T(S_HB_REMOVE_Q), delete_yes);
            }
        }
    }
    if (!hb_count()) {
        ui_text(W / 2, 130 + 58 * 2 + 20, T(S_HB_NONE), 22, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_CENTER);
    }
    list_end(&l);

    if (busy) {
        const float hy = H - 176;
        ui_round(60, hy, W - 120, 88, 12, UI_PANEL);
        ui_text(80, hy + 14, hb_status()[0] ? hb_status() : T(S_HB_PREPARING), 22, UI_FONT_BOLD, UI_GREEN_HI, UI_ALIGN_LEFT);
        ui_round(80, hy + 54, W - 200, 12, 6, (UiColor){0x10, 0x2C, 0x07, 0xFF});
        ui_round(80, hy + 54, fmaxf(12, (W - 200) * hb_progress() / 100.0f), 12, 6, UI_GREEN);
    } else {
        help_panel(help);
    }
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_SELECT), T(S_BACK), hb_count() ? T(S_HB_REMOVE) : NULL, NULL);
}

/* ------------------------------------------------------------ the store */

static int g_app;   /* the store app shown */

void scr_store(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_HB_STORE), "XboxHomebrew.store");
    D.editing_game = false;
    st_start();

    List l;
    list_begin(&l, 74, 130, W - 148, H - 130 - 190, 64);
    /* the category: left and right change it */
    {
        int delta = 0;
        const int c = st_category();
        const char *name = c < 0 || !st_cat(c) ? T(S_HB_ALL) : st_cat(c)->name;
        char val[96];
        snprintf(val, sizeof(val), "< %s >", name);
        list_row(&l, T(S_HB_CATEGORY), val, &delta);
        if (delta && st_ncat() > 0) {
            int n = c + delta;
            if (n < -1) {
                n = st_ncat() - 1;
            }
            if (n >= st_ncat()) {
                n = -1;
            }
            st_set_category(n);
        }
    }
    const char *help = T(S_HB_STORE_HELP2);
    for (int i = 0; i < st_napps(); i++) {
        StApp *a = st_app(i);
        if (!a) {
            break;
        }
        char sub[160];
        snprintf(sub, sizeof(sub), "%s%s%s", a->author, a->author[0] && a->category[0] ? "  -  " : "", a->category);
        if (list_row_icon(&l, st_cover(a), a->name, sub, NULL, NULL)) {
            g_app = i;
            st_open_app(a);
            D.sel[SCR_STORE_APP] = 0;
            dash_push(SCR_STORE_APP);
        }
        if (list_selected(&l) && a->desc[0]) {
            help = a->desc;
        }
    }
    if (st_error()[0] && !st_loading()) {
        if (list_row(&l, st_error(), T(S_HB_RETRY), NULL)) {
            st_retry();
        }
    } else if (st_loading()) {
        list_row(&l, T(S_HB_LOADING), NULL, NULL);
    } else if (st_has_more()) {
        if (list_row(&l, T(S_HB_MORE), NULL, NULL)) {
            st_load_more();
        }
    } else if (!st_napps()) {
        list_row(&l, T(S_HB_STORE_EMPTY), NULL, NULL);
    }
    list_end(&l);
    help_panel(help);
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_SELECT), T(S_BACK), NULL, NULL);
}

void scr_store_app(void)
{
    const float W = ui_w(), H = ui_h();
    StApp *a = st_app(g_app);
    if (!a) {
        dash_pop();
        return;
    }
    draw_header(a->name, a->author[0] ? a->author : NULL);
    D.editing_game = false;
    const int is = st_install_state();
    if (is == 2 || is == 3) {
        dash_toast(st_install_msg());
        st_install_ack();
    }

    const float cx = 60, cy = 130, cw = W - 120, ch = 250;
    ui_round(cx, cy, cw, ch, 16, UI_PANEL);
    ui_round_outline(cx, cy, cw, ch, 16, 1.5f, ui_alpha(UI_GREEN_DIM, 0.8f));
    UiTex *cv = st_cover(a);
    const float is_ = 210;
    if (cv) {
        ui_image(cv, cx + 20, cy + 20, is_, is_, 1);
    } else {
        ui_rect_grad(cx + 20, cy + 20, is_, is_, (UiColor){0x1E, 0x52, 0x0A, 0xFF}, (UiColor){0x06, 0x14, 0x03, 0xFF});
        char ini[2] = {a->name[0], 0};
        ui_text(cx + 20 + is_ / 2, cy + 20 + is_ / 2 - 40, ini, 80, UI_FONT_BOLD, UI_GREEN, UI_ALIGN_CENTER);
    }
    const float tx = cx + 20 + is_ + 24, tw = cw - is_ - 64;
    ui_text(tx, cy + 18, a->category, 18, UI_FONT_BOLD, UI_GREEN_HI, UI_ALIGN_LEFT);
    wrap(tx, cy + 46, tw, a->desc[0] ? a->desc : "", 20, UI_WHITE, 6);
    const StVer *v = NULL;
    for (int i = 0; i < st_nver(); i++) {
        if (a->latest[0] && !strcmp(st_ver(i)->id, a->latest)) {
            v = st_ver(i);
        }
    }
    if (!v) {
        v = st_ver(0);
    }
    char info[200] = "";
    if (st_ver_loading()) {
        snprintf(info, sizeof(info), "%s", T(S_HB_LOADING));
    } else if (v) {
        char size[40] = "";
        if (v->size) {
            snprintf(size, sizeof(size), "  -  %.1f MB", (double)v->size / (1024.0 * 1024.0));
        }
        snprintf(info, sizeof(info), "%s %s%s%s%s", T(S_HB_VERSION), v->version, size, v->date[0] ? "  -  " : "", v->date);
    }
    ui_text(tx, cy + ch - 40, info, 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);

    List l;
    list_begin(&l, 74, cy + ch + 22, W - 148, H - (cy + ch + 22) - 190, 58);
    const bool working = is == 1;
    const bool can = !working && !st_ver_loading() && st_nver() > 0;
    if (list_row(&l, working ? st_install_msg() : T(S_HB_INSTALL), can ? ">" : NULL, NULL) && can) {
        st_install(a);
    }
    list_end(&l);
    if (working) {
        /* the download's bar, so it never looks stuck: filled as it goes, or sliding when the
         * size isn't known (unpacking and installing have no figures) */
        int pct, phase;
        uint64_t bytes, total;
        st_install_progress(&pct, &bytes, &total, &phase);
        const float hy = H - 176, bx = 80, bw = W - 200, bh = 16;
        ui_round(60, hy, W - 120, 88, 12, UI_PANEL);
        char line[160];
        if (phase == 1 && pct >= 0) {
            snprintf(line, sizeof(line), "%s  %d%%  (%.1f / %.1f MB)", st_install_msg(), pct,
                     (double)bytes / 1048576.0, (double)total / 1048576.0);
        } else if (phase == 1 && bytes > 0) {
            snprintf(line, sizeof(line), "%s  %.1f MB", st_install_msg(), (double)bytes / 1048576.0);
        } else {
            snprintf(line, sizeof(line), "%s", st_install_msg());
        }
        ui_text_fit(bx, hy + 12, line, 21, UI_FONT_BOLD, UI_GREEN_HI, UI_ALIGN_LEFT, bw);
        ui_round(bx, hy + 52, bw, bh, bh / 2, (UiColor){0x10, 0x2C, 0x07, 0xFF});
        if (pct >= 0) {
            ui_round(bx, hy + 52, fmaxf(bh, bw * pct / 100.0f), bh, bh / 2, UI_GREEN);
        } else {
            /* a block sliding to and fro */
            const float seg = bw * 0.28f, ph = (float)fmod(ui_time() * 0.9, 2.0);
            const float k = ph < 1 ? ph : 2 - ph;
            ui_round(bx + (bw - seg) * k, hy + 52, seg, bh, bh / 2, UI_GREEN);
        }
    } else {
        help_panel(v && v->changelog[0] ? v->changelog : T(S_HB_INSTALL_HELP));
    }
    if (D.in.back && !working) {
        dash_pop();
    }
    draw_hints(T(S_SELECT), T(S_BACK), NULL, NULL);
}
