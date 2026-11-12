/*
 * xbased front end — the OG Dashboard tab (dash_int.h, scr_ogdash): the
 * original Xbox dashboard from the user's own qcow2 hard disk image. The
 * image is read once (xemu_import.c copies its C: drive's files, the
 * dashboard, into the console's hard disk folder); from then on the tab
 * opens it whenever. The dashboard runs as a game does, in its own window:
 * Guide (or Back+Start, or Esc) then Quit comes back here.
 */
#include "native/app/ui/dash_int.h"

#include <stdio.h>
#include <string.h>

static void wrap(float x, float y, float w, const char *s, float size, UiColor c, int max_lines)
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
        y += size * 1.35f;
        lines++;
    }
}

void scr_ogdash(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_OGDASH), NULL);
    D.editing_game = false;
    const bool have = dash_xbox_dashboard_installed();
    const bool busy = dash_install_busy();

    /* the dashboard's card */
    const float cx = 60, cy = 130, cw = W - 120, ch = 150;
    ui_round(cx, cy, cw, ch, 16, UI_PANEL);
    ui_round_outline(cx, cy, cw, ch, 16, 1.5f, ui_alpha(have ? UI_GREEN : UI_GREEN_DIM, 0.8f));
    const UiColor sc = busy ? UI_GREEN_HI : have ? UI_GREEN : UI_GREY;
    ui_glow(cx + 76, cy + ch / 2, 70, ui_alpha(sc, 0.30f));
    ui_circle(cx + 76, cy + ch / 2, 40, ui_alpha(sc, 0.9f));
    ui_text(cx + 76, cy + ch / 2 - 26, "X", 52, UI_FONT_BOLD, UI_BLACK, UI_ALIGN_CENTER);
    ui_text(cx + 144, cy + 26, "Xbox Dashboard", 40, UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    ui_text(cx + 146, cy + 82, busy ? T(S_OG_READING) : have ? T(S_OG_STATUS_READY) : T(S_OG_STATUS_NONE), 24,
            UI_FONT_REGULAR, busy ? UI_GREEN_HI : have ? UI_GREEN : UI_GREY, UI_ALIGN_LEFT);

    List l;
    list_begin(&l, 74, cy + ch + 22, W - 148, H - (cy + ch + 22) - 190, 58);
    const char *help = have ? T(S_OG_HELP_READY) : T(S_OG_HELP_NONE);
    if (have) {
        if (list_row(&l, T(S_OG_OPEN), busy ? NULL : ">", NULL) && !busy) {
            dash_open_xbox_dashboard();
        }
        if (list_selected(&l)) {
            help = T(S_OG_HELP_INSIGNIA);
        }
    }
    if (list_row(&l, have ? T(S_OG_REPLACE) : T(S_OG_ADD), busy ? T(S_OG_READING) : NULL, NULL) && !busy) {
        dash_install_xbox_dashboard();
    }
    if (list_selected(&l)) {
        help = T(S_OG_HELP_NONE);
    }
    list_end(&l);

    const float hy = H - 176;
    ui_round(60, hy, W - 120, 88, 12, UI_PANEL);
    wrap(80, hy + 12, W - 160, help, 19, UI_WHITE, 3);
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_SELECT), T(S_BACK), NULL, NULL);
}
