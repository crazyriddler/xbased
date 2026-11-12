/*
 * xbased front end — the Insignia screen (dash_int.h, scr_insignia): the
 * Xbox Live revival's status, the steps to get online and the library's
 * games that play on it. The status is Insignia's DNS asked, from this PC,
 * for an Xbox Live name only it answers (a thread; the way the Xbox would
 * ask through xbased's NAT).
 */
#include "native/app/ui/dash_int.h"
#include "native/app/ui/insignia_titles.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSE_SOCK closesocket
#define BAD_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSE_SOCK close
#define BAD_SOCK (-1)
#endif

#define INSIGNIA_DNS "46.101.64.175"
#define TEST_NAME "macs.xboxlive.com"   /* Xbox Live's machine account service: only Insignia's DNS answers */

enum { DNS_ERROR, DNS_ANSWER, DNS_NOANSWER, DNS_NOREPLY };

static bool skip_name(const uint8_t *m, size_t n, size_t *p)
{
    for (int guard = 0; guard < 128; guard++) {
        if (*p >= n) {
            return false;
        }
        const uint8_t len = m[*p];
        if (len == 0) {
            (*p)++;
            return true;
        }
        if ((len & 0xC0) == 0xC0) {
            *p += 2;
            return *p <= n;
        }
        *p += 1 + len;
    }
    return false;
}

static unsigned get16(const uint8_t *m, size_t p)
{
    return (unsigned)(m[p] << 8 | m[p + 1]);
}

/* One A-record lookup of name at server:53 (blocking, up to timeout_ms): the
 * first address in ip on DNS_ANSWER. */
static int dns_query_a(const char *server, const char *name, int timeout_ms, char *ip, size_t ipn)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    uint8_t q[300];
    const unsigned id = (unsigned)(SDL_GetPerformanceCounter() & 0xFFFF);
    size_t qn = 0;
    const uint8_t head[12] = {(uint8_t)(id >> 8), (uint8_t)id, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    memcpy(q, head, 12);
    qn = 12;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        const size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || qn + len + 6 > sizeof(q)) {
            return DNS_ERROR;
        }
        q[qn++] = (uint8_t)len;
        memcpy(q + qn, p, len);
        qn += len;
        p += len + (dot ? 1 : 0);
    }
    const uint8_t tail[5] = {0, 0, 1, 0, 1};
    memcpy(q + qn, tail, 5);
    qn += 5;
    const sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) {
        return DNS_ERROR;
    }
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
#else
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    if (inet_pton(AF_INET, server, &to.sin_addr) != 1) {
        CLOSE_SOCK(s);
        return DNS_ERROR;
    }
    int result = DNS_NOREPLY;
    for (int attempt = 0; attempt < 2 && result == DNS_NOREPLY; attempt++) {
        if (sendto(s, (const char *)q, (int)qn, 0, (const struct sockaddr *)&to, sizeof(to)) < 0) {
            result = DNS_ERROR;
            break;
        }
        for (;;) {
            uint8_t m[1500];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const int n = (int)recvfrom(s, (char *)m, sizeof(m), 0, (struct sockaddr *)&from, &fl);
            if (n < 12) {
                break;
            }
            if (get16(m, 0) != id || !(m[2] & 0x80)) {
                continue;
            }
            const int qd = (int)get16(m, 4), an = (int)get16(m, 6);
            size_t p = 12;
            bool ok = true;
            for (int i = 0; i < qd && ok; i++) {
                ok = skip_name(m, (size_t)n, &p) && (p += 4) <= (size_t)n;
            }
            result = DNS_NOANSWER;
            for (int i = 0; i < an && ok; i++) {
                ok = skip_name(m, (size_t)n, &p) && p + 10 <= (size_t)n;
                if (!ok) {
                    break;
                }
                const unsigned type = get16(m, p), rdlen = get16(m, p + 8);
                p += 10;
                if (p + rdlen > (size_t)n) {
                    break;
                }
                if (type == 1 && rdlen == 4) {
                    snprintf(ip, ipn, "%u.%u.%u.%u", m[p], m[p + 1], m[p + 2], m[p + 3]);
                    result = DNS_ANSWER;
                    break;
                }
                p += rdlen;
            }
            break;
        }
    }
    CLOSE_SOCK(s);
    return result;
}

/* the check, on a thread: 0 not run, 1 running, 2 passed, 3 failed */
static SDL_AtomicInt g_state;
static char g_server[64], g_msg[400];
static time_t g_checked;

static int SDLCALL dns_thread(void *ud)
{
    (void)ud;
    char ip[32] = "";
    const int r = dns_query_a(g_server, TEST_NAME, 2500, ip, sizeof(ip));
    if (r == DNS_ANSWER) {
        snprintf(g_msg, sizeof(g_msg), T(S_INS_NOTE_PASSED), ip);
    } else if (r == DNS_NOANSWER) {
        snprintf(g_msg, sizeof(g_msg), "%s", T(S_INS_NOTE_REDIRECT));
    } else {
        snprintf(g_msg, sizeof(g_msg), "%s", T(S_INS_NOTE_NOREPLY));
    }
    g_checked = time(NULL);
    SDL_SetAtomicInt(&g_state, r == DNS_ANSWER ? 2 : 3);
    return 0;
}

static void test_start(void)
{
    if (SDL_GetAtomicInt(&g_state) == 1) {
        return;
    }
    const char *dns = cfg_get(&D.global, "dns");
    snprintf(g_server, sizeof(g_server), "%s", dns && dns[0] ? dns : INSIGNIA_DNS);
    SDL_SetAtomicInt(&g_state, 1);
    SDL_Thread *t = SDL_CreateThread(dns_thread, "insignia-dns", NULL);
    if (t) {
        SDL_DetachThread(t);
    } else {
        SDL_SetAtomicInt(&g_state, 0);
    }
}

static bool supported(uint32_t id)
{
    int lo = 0, hi = (int)(sizeof(k_insignia_ids) / sizeof(k_insignia_ids[0])) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (k_insignia_ids[mid] == id) {
            return true;
        }
        if (k_insignia_ids[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return false;
}

static void wrap(float x, float y, float w, const char *s, float size, UiColor c, int max_lines)
{
    int lines = 0;
    while (*s && lines < max_lines) {
        char line[400];
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

void scr_insignia(void)
{
    const float W = ui_w(), H = ui_h();
    draw_header(T(S_INSIGNIA), NULL);
    D.editing_game = false;
    const char *net = cfg_get(&D.global, "net");
    const bool nat = net && !strcmp(net, "nat");
    const char *dns = cfg_get(&D.global, "dns");
    const bool dns_ok = !dns || !dns[0] || !strcmp(dns, INSIGNIA_DNS);
    if (SDL_GetAtomicInt(&g_state) == 0) {
        test_start();       /* once, when the screen first opens */
    }
    const int st = SDL_GetAtomicInt(&g_state);

    /* the status card */
    const float cx = 60, cy = 130, cw = W - 120, ch = 112;
    ui_round(cx, cy, cw, ch, 16, UI_PANEL);
    ui_round_outline(cx, cy, cw, ch, 16, 1.5f, ui_alpha(UI_GREEN_DIM, 0.8f));
    const UiColor sc = st == 2 ? UI_GREEN : st == 3 ? UI_RED : UI_GREY;
    ui_glow(cx + 56, cy + ch / 2, 50, ui_alpha(sc, 0.35f));
    ui_circle(cx + 56, cy + ch / 2, 26, ui_alpha(sc, 0.9f));
    ui_text(cx + 56, cy + ch / 2 - 17, st == 2 ? "+" : st == 3 ? "!" : "...", 34, UI_FONT_BOLD, UI_BLACK,
            UI_ALIGN_CENTER);
    ui_text(cx + 104, cy + 16, st == 2 ? T(S_INS_ONLINE) : st == 3 ? T(S_INS_DOWN) : T(S_INS_CHECKING), 34,
            UI_FONT_BOLD, UI_WHITE, UI_ALIGN_LEFT);
    int owned = 0, total = 0;
    LibGame *games[512];
    const int ng = lib_list(games, 512);
    for (int i = 0; i < ng; i++) {
        if (supported(games[i]->g.title_id)) {
            owned++;
        }
    }
    total = (int)(sizeof(k_insignia_ids) / sizeof(k_insignia_ids[0]));
    char stat[160];
    snprintf(stat, sizeof(stat), T(S_INS_STAT_OWNED), owned, total);
    ui_text(cx + 106, cy + 64, stat, 20, UI_FONT_REGULAR, UI_GREY, UI_ALIGN_LEFT);
    if (st >= 2 && g_msg[0]) {
        wrap(cx + cw * 0.52f, cy + 18, cw * 0.46f - 20, g_msg, 17, UI_GREY, 4);
    }

    /* the steps, then the library's games */
    List l;
    list_begin(&l, 74, cy + ch + 18, W - 148, H - (cy + ch + 18) - 150, 54);
    const char *help = "";
    char val[96];
    if (list_row(&l, T(S_INS_STEP_NET), nat ? T(S_INS_NET_ON) : NULL, NULL)) {
        cfg_set(&D.global, "net", "nat");
        dash_save_cfg();
        dash_toast(T(S_INS_NET_ON));
    }
    if (list_selected(&l)) {
        help = T(S_INS_HELP_NET);
    }
    snprintf(val, sizeof(val), "%s", dns_ok ? INSIGNIA_DNS : dns);
    if (list_row(&l, T(S_INS_STEP_DNS), val, NULL)) {
        cfg_set(&D.global, "dns", INSIGNIA_DNS);
        cfg_set(&D.global, "net", "nat");
        dash_save_cfg();
        test_start();
    }
    if (list_selected(&l)) {
        help = T(S_INS_HELP_DNS);
    }
    if (list_row(&l, T(S_INS_TEST), st == 1 ? T(S_INS_CHECKING) : st == 2 ? T(S_INS_ONLINE) : st == 3 ? T(S_INS_DOWN) : NULL,
                 NULL)) {
        test_start();
    }
    if (list_row(&l, T(S_INS_SIGNUP), ">", NULL)) {
        SDL_OpenURL("https://insignia.live/");
    }
    if (list_selected(&l)) {
        help = T(S_INS_HELP_SIGNUP);
    }
    if (list_row(&l, T(S_INS_ACCOUNT), dash_xbox_dashboard_installed() ? ">" : T(S_NOT_INSTALLED), NULL)) {
        dash_open_xbox_dashboard();     /* the Xbox dashboard's Xbox Live pages */
    }
    if (list_selected(&l)) {
        help = T(S_INS_HELP_ACCOUNT);
    }
    if (list_row(&l, T(S_INS_GUIDE), ">", NULL)) {
        SDL_OpenURL("https://insignia.live/connect");
    }
    if (list_selected(&l)) {
        help = T(S_INS_HELP_GUIDE);
    }
    if (list_row(&l, T(S_INS_WHO), ">", NULL)) {
        SDL_OpenURL("https://insignia.live/games");
    }
    if (list_selected(&l)) {
        help = T(S_INS_HELP_WHO);
    }
    list_row(&l, T(S_INS_READY), NULL, NULL);
    if (!owned) {
        list_row(&l, T(S_INS_NONE), NULL, NULL);
    }
    for (int i = 0; i < ng; i++) {
        if (!supported(games[i]->g.title_id)) {
            continue;
        }
        char sub[32];
        snprintf(sub, sizeof(sub), "%08X", games[i]->g.title_id);
        if (list_row_icon(&l, lib_icon(games[i]), games[i]->g.title, sub, T(S_INS_PLAY_ONLINE), NULL)) {
            if (!nat) {
                cfg_set(&D.global, "net", "nat");
                dash_save_cfg();
            }
            launch_game(games[i], NULL);
        }
        if (list_selected(&l)) {
            help = T(S_INS_HELP_GAME);
        }
    }
    list_end(&l);
    if (help[0]) {
        const float hy = H - 142;
        ui_round(60, hy, W - 120, 74, 12, UI_PANEL);
        wrap(80, hy + 10, W - 160, help, 19, UI_WHITE, 2);
    }
    if (D.in.back) {
        dash_pop();
    }
    draw_hints(T(S_SELECT), T(S_BACK), NULL, NULL);
}
