/*
 * xbased front end — homebrew: the installed apps and XboxHomebrew.store (homebrew.h).
 *
 * The store's public API is the one its Xbox app uses (categories, a paged app
 * list, versions with their files, covers, downloads). Downloads go through curl,
 * zips are unpacked by tar (Windows' own bsdtar) or unzip, as the updater does.
 * Only homebrew is offered: firmware and Microsoft's own dashboards are left out.
 */
#include "native/app/ui/homebrew.h"

#include "native/app/ui/dash_int.h"
#include "native/app/ui/library.h"
#include "native/app/ui/xiso_build.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API "https://api.xboxhomebrew.store"
#define MAX_APPS 600
#define PAGE 48
#define MAX_CATS 40
#define MAX_VERS 24
#define MAX_HB 512

/* ------------------------------------------------------------ small things */

static void join(char *out, size_t n, const char *a, const char *b)
{
    const size_t l = strlen(a);
    snprintf(out, n, "%s%s%s", a, l && a[l - 1] != '/' && a[l - 1] != '\\' ? "/" : "", b);
}

static bool exists(const char *p)
{
    return SDL_GetPathInfo(p, NULL);
}

static void mkdirs(const char *path)
{
    SDL_CreateDirectory(path);  /* creates the parents too */
}

/* a folder and everything in it removed */
typedef struct {
    char dir[1100];
} RmCtx;

static SDL_EnumerationResult SDLCALL rm_entry(void *ud, const char *dirname, const char *fname)
{
    (void)ud;
    char p[2200];
    join(p, sizeof(p), dirname, fname);
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_EnumerateDirectory(p, rm_entry, NULL);
    }
    SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}

static void remove_tree(const char *dir)
{
    SDL_EnumerateDirectory(dir, rm_entry, NULL);
    SDL_RemovePath(dir);
}

static SDL_EnumerationResult SDLCALL cp_entry(void *ud, const char *dirname, const char *fname)
{
    const char *to = ud;
    char from[2200], dest[2200];
    join(from, sizeof(from), dirname, fname);
    join(dest, sizeof(dest), to, fname);
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(from, &pi)) {
        return SDL_ENUM_CONTINUE;
    }
    if (pi.type == SDL_PATHTYPE_DIRECTORY) {
        mkdirs(dest);
        SDL_EnumerateDirectory(from, cp_entry, dest);
    } else {
        SDL_CopyFile(from, dest);
    }
    return SDL_ENUM_CONTINUE;
}

/* from's contents under to: renamed when both are on one volume, else copied */
static bool move_tree(const char *from, const char *to)
{
    char parent[1100];
    snprintf(parent, sizeof(parent), "%s", to);
    char *s = strrchr(parent, '/');
    if (s) {
        *s = 0;
        mkdirs(parent);
    }
    if (SDL_RenamePath(from, to)) {
        return true;
    }
    mkdirs(to);
    SDL_EnumerateDirectory(from, cp_entry, (void *)to);
    return exists(to);
}

static void data_sub(char *out, size_t n, const char *sub)
{
    snprintf(out, n, "%s%s", cfg_data_dir(), sub);
}

void hb_e_dir(char *out, size_t n)
{
    const char *hdd = cfg_get(&D.global, "hdd");
    if (hdd && hdd[0]) {
        snprintf(out, n, "%s/Partition1", hdd);
    } else {
        snprintf(out, n, "%shdd/Partition1", cfg_data_dir());
    }
}

/* ------------------------------------------------------------ a small JSON reader */

typedef struct J {
    char t;                 /* o object, a array, s string, n number, b true/false, z null */
    char *key, *str;
    double num;
    struct J *child, *next;
} J;

static void j_free(J *j)
{
    while (j) {
        J *n = j->next;
        j_free(j->child);
        free(j->key);
        free(j->str);
        free(j);
        j = n;
    }
}

static void ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
        (*p)++;
    }
}

static void put_utf8(char **o, unsigned c)
{
    if (c < 0x80) {
        *(*o)++ = (char)c;
    } else if (c < 0x800) {
        *(*o)++ = (char)(0xC0 | (c >> 6));
        *(*o)++ = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        *(*o)++ = (char)(0xE0 | (c >> 12));
        *(*o)++ = (char)(0x80 | ((c >> 6) & 0x3F));
        *(*o)++ = (char)(0x80 | (c & 0x3F));
    } else {
        *(*o)++ = (char)(0xF0 | (c >> 18));
        *(*o)++ = (char)(0x80 | ((c >> 12) & 0x3F));
        *(*o)++ = (char)(0x80 | ((c >> 6) & 0x3F));
        *(*o)++ = (char)(0x80 | (c & 0x3F));
    }
}

static char *j_string(const char **p)
{
    (*p)++;     /* the opening quote */
    const char *s = *p;
    size_t cap = 16;
    for (const char *q = s; *q && *q != '"'; q++) {
        cap++;
        if (*q == '\\' && q[1]) {
            q++;
        }
    }
    char *out = malloc(cap * 4 / 3 + 8), *o = out;
    while (**p && **p != '"') {
        if (**p == '\\' && (*p)[1]) {
            (*p)++;
            switch (**p) {
            case 'n': *o++ = '\n'; break;
            case 't': *o++ = '\t'; break;
            case 'r': break;
            case 'b': case 'f': break;
            case 'u': {
                unsigned c = 0;
                for (int i = 1; i <= 4 && isxdigit((unsigned char)(*p)[i]); i++) {
                    c = c * 16 + (unsigned)(isdigit((unsigned char)(*p)[i]) ? (*p)[i] - '0' : (tolower((*p)[i]) - 'a' + 10));
                }
                *p += 4;
                if (c >= 0xD800 && c < 0xDC00 && (*p)[1] == '\\' && (*p)[2] == 'u') {
                    unsigned lo = 0;
                    for (int i = 3; i <= 6 && isxdigit((unsigned char)(*p)[i]); i++) {
                        lo = lo * 16 + (unsigned)(isdigit((unsigned char)(*p)[i]) ? (*p)[i] - '0' : (tolower((*p)[i]) - 'a' + 10));
                    }
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    *p += 6;
                }
                put_utf8(&o, c);
                break;
            }
            default: *o++ = **p; break;
            }
            (*p)++;
        } else {
            *o++ = *(*p)++;
        }
    }
    if (**p == '"') {
        (*p)++;
    }
    *o = 0;
    return out;
}

static J *j_value(const char **p, int depth)
{
    ws(p);
    if (depth > 24 || !**p) {
        return NULL;
    }
    J *j = calloc(1, sizeof(J));
    if (**p == '{' || **p == '[') {
        const char close = **p == '{' ? '}' : ']';
        j->t = **p == '{' ? 'o' : 'a';
        (*p)++;
        J **tail = &j->child;
        for (;;) {
            ws(p);
            if (**p == close) {
                (*p)++;
                break;
            }
            if (!**p) {
                break;
            }
            char *key = NULL;
            if (j->t == 'o') {
                if (**p != '"') {
                    break;
                }
                key = j_string(p);
                ws(p);
                if (**p != ':') {
                    free(key);
                    break;
                }
                (*p)++;
            }
            J *c = j_value(p, depth + 1);
            if (!c) {
                free(key);
                break;
            }
            c->key = key;
            *tail = c;
            tail = &c->next;
            ws(p);
            if (**p == ',') {
                (*p)++;
            }
        }
    } else if (**p == '"') {
        j->t = 's';
        j->str = j_string(p);
    } else if (!strncmp(*p, "true", 4) || !strncmp(*p, "false", 5)) {
        j->t = 'b';
        j->num = **p == 't';
        *p += **p == 't' ? 4 : 5;
    } else if (!strncmp(*p, "null", 4)) {
        j->t = 'z';
        *p += 4;
    } else {
        char *e;
        j->num = strtod(*p, &e);
        if (e == *p) {
            free(j);
            return NULL;
        }
        j->t = 'n';
        *p = e;
    }
    return j;
}

static J *j_parse(const char *text)
{
    const char *p = text;
    return j_value(&p, 0);
}

static const J *j_get(const J *o, const char *key)
{
    if (!o || o->t != 'o') {
        return NULL;
    }
    for (const J *c = o->child; c; c = c->next) {
        if (c->key && !strcmp(c->key, key)) {
            return c;
        }
    }
    return NULL;
}

/* a string field, or an integer as text; out "" when it isn't there */
static void j_str(const J *o, const char *key, char *out, size_t n)
{
    const J *v = j_get(o, key);
    out[0] = 0;
    if (!v) {
        return;
    }
    if (v->t == 's' && v->str) {
        snprintf(out, n, "%s", v->str);
    } else if (v->t == 'n') {
        snprintf(out, n, "%lld", (long long)v->num);
    }
}

static uint64_t j_u64(const J *o, const char *key)
{
    const J *v = j_get(o, key);
    if (!v) {
        return 0;
    }
    if (v->t == 'n') {
        return v->num > 0 ? (uint64_t)v->num : 0;
    }
    if (v->t == 's' && v->str) {
        return strtoull(v->str, NULL, 10);
    }
    return 0;
}

/* the array itself, or the first array under a common wrapper key */
static const J *j_array(const J *j)
{
    if (!j) {
        return NULL;
    }
    if (j->t == 'a') {
        return j;
    }
    if (j->t == 'o') {
        static const char *const keys[] = {"items", "apps", "versions", "data", "results"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const J *a = j_get(j, keys[i]);
            if (a && a->t == 'a') {
                return a;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ the installed apps */

static struct {
    HbApp apps[MAX_HB];
    int n;
    SDL_Thread *th;
    SDL_AtomicInt busy, pct, ready;
    char status[200];
    char iso[1100];
    char dir[1100], name[64], group[16];
} H;

typedef struct {
    int group;
} ScanCtx;

static const char *const k_groups[] = {"Games", "Apps", "Emulators"};

static int cmp_apps(const void *a, const void *b)
{
    const HbApp *x = a, *y = b;
    const int g = strcmp(x->group, y->group);
    if (g) {
        /* Games, Apps, Emulators in that order, not alphabetically */
        return strcmp(x->group, "Games") == 0 ? -1 : strcmp(y->group, "Games") == 0 ? 1
               : strcmp(x->group, "Apps") == 0 ? -1 : 1;
    }
    return SDL_strcasecmp(x->name, y->name);
}

static SDL_EnumerationResult SDLCALL scan_entry(void *ud, const char *dirname, const char *fname)
{
    ScanCtx *c = ud;
    if (H.n >= MAX_HB) {
        return SDL_ENUM_SUCCESS;
    }
    char dir[1100], xbe[1200];
    join(dir, sizeof(dir), dirname, fname);
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(dir, &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) {
        return SDL_ENUM_CONTINUE;
    }
    join(xbe, sizeof(xbe), dir, "default.xbe");
    if (!exists(xbe)) {
        join(xbe, sizeof(xbe), dir, "Default.xbe");
        if (!exists(xbe)) {
            return SDL_ENUM_CONTINUE;
        }
    }
    HbApp *a = &H.apps[H.n++];
    memset(a, 0, sizeof(*a));
    snprintf(a->name, sizeof(a->name), "%s", fname);
    snprintf(a->group, sizeof(a->group), "%s", k_groups[c->group]);
    snprintf(a->dir, sizeof(a->dir), "%s", dir);
    char mk[1200];
    join(mk, sizeof(mk), dir, "xbased-store.txt");
    size_t len = 0;
    char *txt = exists(mk) ? SDL_LoadFile(mk, &len) : NULL;
    if (txt) {
        char *nl = strpbrk(txt, "\r\n");
        if (nl) {
            *nl = 0;
        }
        snprintf(a->store_id, sizeof(a->store_id), "%s", txt);
        SDL_free(txt);
    }
    return SDL_ENUM_CONTINUE;
}

void hb_scan(void)
{
    for (int i = 0; i < H.n; i++) {
        if (H.apps[i].icon) {
            ui_texture_free(H.apps[i].icon);
        }
    }
    H.n = 0;
    char e[1100];
    hb_e_dir(e, sizeof(e));
    for (int g = 0; g < 3; g++) {
        char d[1200];
        join(d, sizeof(d), e, k_groups[g]);
        ScanCtx c = {g};
        SDL_EnumerateDirectory(d, scan_entry, &c);
    }
    qsort(H.apps, (size_t)H.n, sizeof(HbApp), cmp_apps);
}

int hb_count(void)
{
    return H.n;
}

HbApp *hb_get(int i)
{
    return i >= 0 && i < H.n ? &H.apps[i] : NULL;
}

bool hb_busy(void)
{
    return SDL_GetAtomicInt(&H.busy) != 0;
}

int hb_progress(void)
{
    return SDL_GetAtomicInt(&H.pct);
}

const char *hb_status(void)
{
    return H.status;
}

static int image_thread(void *arg)
{
    (void)arg;
    char err[200] = "";
    char dir[64];
    snprintf(dir, sizeof(dir), "homebrew");
    char base[1100];
    data_sub(base, sizeof(base), dir);
    mkdirs(base);
    char safe[200];
    snprintf(safe, sizeof(safe), "%s-%s", H.group, H.name);
    for (char *c = safe; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') {
            *c = '_';
        }
    }
    char iso[1200], sig[1210];
    snprintf(iso, sizeof(iso), "%s/%s.xiso", base, safe);
    snprintf(sig, sizeof(sig), "%s.sig", iso);
    snprintf(H.status, sizeof(H.status), "Reading the folder...");
    XisoTree *t = xiso_tree_open(H.dir, err, sizeof(err));
    bool ok = false;
    if (t) {
        char want[40];
        snprintf(want, sizeof(want), "%016llx", (unsigned long long)xiso_tree_signature(t));
        size_t len = 0;
        char *have = exists(iso) ? SDL_LoadFile(sig, &len) : NULL;
        if (have && len >= 16 && !strncmp(have, want, 16)) {
            ok = true;      /* the image is current */
        } else {
            snprintf(H.status, sizeof(H.status), "Preparing %s...", H.name);
            ok = xiso_tree_write(t, iso, &H.pct, err, sizeof(err));
            if (ok) {
                SDL_IOStream *io = SDL_IOFromFile(sig, "wb");
                if (io) {
                    SDL_WriteIO(io, want, strlen(want));
                    SDL_CloseIO(io);
                }
            }
        }
        SDL_free(have);
        xiso_tree_free(t);
    }
    if (ok) {
        snprintf(H.iso, sizeof(H.iso), "%s", iso);
        H.status[0] = 0;
        SDL_SetAtomicInt(&H.ready, 1);
    } else {
        snprintf(H.status, sizeof(H.status), "Couldn't prepare it: %s", err[0] ? err : "unknown error");
        SDL_SetAtomicInt(&H.ready, -1);
    }
    SDL_SetAtomicInt(&H.busy, 0);
    return 0;
}

void hb_launch(int i)
{
    HbApp *a = hb_get(i);
    if (!a || !SDL_CompareAndSwapAtomicInt(&H.busy, 0, 1)) {
        return;
    }
    if (H.th) {
        SDL_WaitThread(H.th, NULL);
    }
    snprintf(H.dir, sizeof(H.dir), "%s", a->dir);
    snprintf(H.name, sizeof(H.name), "%s", a->name);
    snprintf(H.group, sizeof(H.group), "%s", a->group);
    SDL_SetAtomicInt(&H.pct, 0);
    SDL_SetAtomicInt(&H.ready, 0);
    H.th = SDL_CreateThread(image_thread, "hb-image", NULL);
    if (!H.th) {
        SDL_SetAtomicInt(&H.busy, 0);
    }
}

void hb_poll(void)
{
    const int r = SDL_GetAtomicInt(&H.ready);
    if (r == 1) {
        SDL_SetAtomicInt(&H.ready, 0);
        dash_play_file_later(H.iso);
    } else if (r == -1) {
        SDL_SetAtomicInt(&H.ready, 0);
        dash_toast(H.status);
    }
}

static void cover_path(const char *id, char *out, size_t n);

UiTex *hb_icon(HbApp *a)
{
    if (!a->icon && !a->icon_tried && a->store_id[0]) {
        char p[1300];
        cover_path(a->store_id, p, sizeof(p));
        if (exists(p)) {
            a->icon = lib_load_png(p);
            a->icon_tried = true;
        }
    }
    return a->icon;
}

void hb_delete(int i)
{
    HbApp *a = hb_get(i);
    if (a && !hb_busy()) {
        remove_tree(a->dir);
        hb_scan();
    }
}

/* ------------------------------------------------------------ the store */

static struct {
    SDL_Mutex *mu;
    StCat cats[MAX_CATS];
    SDL_AtomicInt ncat, cats_state;     /* 0 not asked, 1 loading, 2 done */
    int cat;                            /* shown: -1 all */
    StApp apps[MAX_APPS];
    SDL_AtomicInt napps, raw, loading, more, gen, covering;   /* raw: entries the store returned (some are left out) */
    char err[160];
    StVer vers[MAX_VERS];
    SDL_AtomicInt nver, ver_loading;
    char ver_app[48];
    SDL_AtomicInt inst_state;
    char inst_msg[200];
    bool started;
} S;

static void lower(char *s)
{
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

static bool category_allowed(const char *category)
{
    char c[64];
    snprintf(c, sizeof(c), "%s", category);
    lower(c);
    return strcmp(c, "firmware") && strcmp(c, "bios") && strcmp(c, "dlc") && strcmp(c, "saves");
}

static bool app_allowed(const StApp *a)
{
    if (!category_allowed(a->category)) {
        return false;
    }
    char c[64], who[160];
    snprintf(c, sizeof(c), "%s", a->category);
    lower(c);
    if (!strcmp(c, "dashboards")) {
        snprintf(who, sizeof(who), "%s %s", a->author, a->name);
        lower(who);
        if (strstr(who, "microsoft")) {
            return false;     /* Microsoft's own dashboards are its copyrighted software */
        }
    }
    return true;
}

static void url_encode(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
        }
    }
    out[o] = 0;
}

static bool http_get(const char *url, const char *to, const char *max_time)
{
    const char *args[] = {"curl", "-sfL", "--max-time", max_time, "-A", "xbased", "-o", to, url, NULL};
    return cfg_run(args);
}

static char *http_text(const char *url)
{
    char tmp[1100];
    data_sub(tmp, sizeof(tmp), "store");
    mkdirs(tmp);
    data_sub(tmp, sizeof(tmp), "store/page.tmp");
    /* one file a thread: the thread's address in the name */
    char name[1200];
    snprintf(name, sizeof(name), "%s.%u", tmp, (unsigned)SDL_GetCurrentThreadID());
    remove(name);
    char *text = NULL;
    if (http_get(url, name, "25")) {
        size_t len;
        text = SDL_LoadFile(name, &len);
    }
    remove(name);
    return text;
}

static int cats_thread(void *arg)
{
    (void)arg;
    char url[200];
    snprintf(url, sizeof(url), "%s/api/categories", API);
    char *text = http_text(url);
    int n = 0;
    if (text) {
        J *j = j_parse(text);
        const J *a = j_array(j);
        for (const J *c = a ? a->child : NULL; c && n < MAX_CATS; c = c->next) {
            StCat cat;
            memset(&cat, 0, sizeof(cat));
            j_str(c, "name", cat.name, sizeof(cat.name));
            cat.count = (int)j_u64(c, "count");
            if (!cat.name[0] || !category_allowed(cat.name)) {
                continue;
            }
            S.cats[n++] = cat;
        }
        j_free(j);
        SDL_free(text);
    }
    SDL_SetAtomicInt(&S.ncat, n);
    SDL_SetAtomicInt(&S.cats_state, 2);
    return 0;
}

static void cover_path(const char *id, char *out, size_t n)
{
    char enc[160], dir[1100];
    url_encode(id, enc, sizeof(enc));
    data_sub(dir, sizeof(dir), "store/covers");
    snprintf(out, n, "%s/%s.png", dir, enc);
}

static int covers_thread(void *arg)
{
    (void)arg;
    char dir[1100];
    data_sub(dir, sizeof(dir), "store/covers");
    mkdirs(dir);
    const int gen = SDL_GetAtomicInt(&S.gen);
    for (int i = 0; i < SDL_GetAtomicInt(&S.napps) && gen == SDL_GetAtomicInt(&S.gen); i++) {
        char enc[160], p[1300], url[400];
        url_encode(S.apps[i].id, enc, sizeof(enc));
        snprintf(p, sizeof(p), "%s/%s.png", dir, enc);
        if (!exists(p)) {
            snprintf(url, sizeof(url), "%s/api/Cover/%s?width=256&height=256", API, enc);
            char part[1320];
            snprintf(part, sizeof(part), "%s.part", p);
            if (http_get(url, part, "20")) {
                SDL_RenamePath(part, p);
            } else {
                remove(part);
            }
        }
        if (i + 1 == SDL_GetAtomicInt(&S.napps)) {
            SDL_Delay(100);     /* a page still arriving */
        }
    }
    SDL_SetAtomicInt(&S.covering, 0);
    return 0;
}

typedef struct {
    int offset, gen;
    char category[48];
} PageArgs;

static int apps_thread(void *arg)
{
    PageArgs *pa = arg;
    char url[400], enc[160];
    url_encode(pa->category, enc, sizeof(enc));
    snprintf(url, sizeof(url), "%s/api/apps?offset=%d&count=%d%s%s", API, pa->offset, PAGE,
             pa->category[0] ? "&category=" : "", pa->category[0] ? enc : "");
    char *text = http_text(url);
    bool more = false;
    if (!text) {
        if (pa->gen == SDL_GetAtomicInt(&S.gen)) {
            snprintf(S.err, sizeof(S.err), "Couldn't reach XboxHomebrew.store (check your connection; curl is needed).");
        }
    } else if (pa->gen == SDL_GetAtomicInt(&S.gen)) {
        J *j = j_parse(text);
        const J *a = j_array(j);
        int got = 0;
        if (!a) {
            snprintf(S.err, sizeof(S.err), "The store's answer wasn't understood.");
        }
        for (const J *o = a ? a->child : NULL; o; o = o->next) {
            got++;
            const int n = SDL_GetAtomicInt(&S.napps);
            if (n >= MAX_APPS) {
                break;
            }
            StApp *ap = &S.apps[n];
            memset(ap, 0, sizeof(*ap));
            j_str(o, "id", ap->id, sizeof(ap->id));
            if (!ap->id[0]) {
                continue;
            }
            j_str(o, "name", ap->name, sizeof(ap->name));
            j_str(o, "author", ap->author, sizeof(ap->author));
            j_str(o, "category", ap->category, sizeof(ap->category));
            j_str(o, "description", ap->desc, sizeof(ap->desc));
            j_str(o, "latest_version", ap->latest, sizeof(ap->latest));
            if (!app_allowed(ap)) {
                continue;
            }
            SDL_SetAtomicInt(&S.napps, n + 1);
        }
        SDL_AddAtomicInt(&S.raw, got);
        more = got >= PAGE && SDL_GetAtomicInt(&S.napps) < MAX_APPS;
        j_free(j);
        SDL_free(text);
        if (SDL_CompareAndSwapAtomicInt(&S.covering, 0, 1)) {
            SDL_Thread *t = SDL_CreateThread(covers_thread, "st-covers", NULL);
            if (t) {
                SDL_DetachThread(t);
            } else {
                SDL_SetAtomicInt(&S.covering, 0);
            }
        }
    }
    if (pa->gen == SDL_GetAtomicInt(&S.gen)) {
        SDL_SetAtomicInt(&S.more, more);
        SDL_SetAtomicInt(&S.loading, 0);
    }
    free(pa);
    return 0;
}

static void fetch_page(int offset)
{
    if (!SDL_CompareAndSwapAtomicInt(&S.loading, 0, 1)) {
        return;
    }
    PageArgs *pa = calloc(1, sizeof(*pa));
    pa->offset = offset;
    pa->gen = SDL_GetAtomicInt(&S.gen);
    if (S.cat >= 0 && S.cat < SDL_GetAtomicInt(&S.ncat)) {
        snprintf(pa->category, sizeof(pa->category), "%s", S.cats[S.cat].name);
    }
    S.err[0] = 0;
    SDL_Thread *t = SDL_CreateThread(apps_thread, "st-apps", pa);
    if (t) {
        SDL_DetachThread(t);
    } else {
        free(pa);
        SDL_SetAtomicInt(&S.loading, 0);
    }
}

void st_start(void)
{
    if (S.started) {
        return;
    }
    S.started = true;
    S.cat = -1;
    SDL_SetAtomicInt(&S.cats_state, 1);
    SDL_Thread *t = SDL_CreateThread(cats_thread, "st-cats", NULL);
    if (t) {
        SDL_DetachThread(t);
    }
    fetch_page(0);
}

void st_retry(void)
{
    S.started = false;
    SDL_AddAtomicInt(&S.gen, 1);
    SDL_SetAtomicInt(&S.loading, 0);
    SDL_SetAtomicInt(&S.napps, 0);
    SDL_SetAtomicInt(&S.raw, 0);
    SDL_SetAtomicInt(&S.more, 0);
    S.err[0] = 0;
    st_start();
}

int st_ncat(void)
{
    return SDL_GetAtomicInt(&S.ncat);
}

const StCat *st_cat(int i)
{
    return i >= 0 && i < SDL_GetAtomicInt(&S.ncat) ? &S.cats[i] : NULL;
}

int st_category(void)
{
    return S.cat;
}

void st_set_category(int i)
{
    if (i < -1 || i >= SDL_GetAtomicInt(&S.ncat) || i == S.cat) {
        return;
    }
    SDL_AddAtomicInt(&S.gen, 1);      /* what is still arriving is for the old one */
    SDL_SetAtomicInt(&S.loading, 0);
    SDL_SetAtomicInt(&S.napps, 0);
    SDL_SetAtomicInt(&S.raw, 0);
    SDL_SetAtomicInt(&S.more, 0);
    S.cat = i;
    for (int k = 0; k < MAX_APPS; k++) {
        if (S.apps[k].cover) {
            ui_texture_free(S.apps[k].cover);
            S.apps[k].cover = NULL;
        }
    }
    fetch_page(0);
}

int st_napps(void)
{
    return SDL_GetAtomicInt(&S.napps);
}

StApp *st_app(int i)
{
    return i >= 0 && i < SDL_GetAtomicInt(&S.napps) ? &S.apps[i] : NULL;
}

bool st_loading(void)
{
    return SDL_GetAtomicInt(&S.loading) != 0;
}

bool st_has_more(void)
{
    return SDL_GetAtomicInt(&S.more) != 0;
}

void st_load_more(void)
{
    if (!st_loading() && st_has_more()) {
        fetch_page(SDL_GetAtomicInt(&S.raw));   /* the offset counts what the store returned */
    }
}

const char *st_error(void)
{
    return S.err;
}

UiTex *st_cover(StApp *a)
{
    if (a->cover || a->cover_tried) {
        return a->cover;
    }
    char enc[160], p[1300], dir[1100];
    url_encode(a->id, enc, sizeof(enc));
    data_sub(dir, sizeof(dir), "store/covers");
    snprintf(p, sizeof(p), "%s/%s.png", dir, enc);
    if (exists(p)) {
        a->cover = lib_load_png(p);
        a->cover_tried = true;
    }
    return a->cover;
}

/* ---- versions ---- */

static int vers_thread(void *arg)
{
    char *id = arg;
    char url[400], enc[160];
    url_encode(id, enc, sizeof(enc));
    snprintf(url, sizeof(url), "%s/api/apps/%s/versions", API, enc);
    char *text = http_text(url);
    int n = 0;
    if (text && !strcmp(id, S.ver_app)) {
        J *j = j_parse(text);
        const J *a = j_array(j);
        for (const J *o = a ? a->child : NULL; o && n < MAX_VERS; o = o->next) {
            StVer *v = &S.vers[n];
            memset(v, 0, sizeof(*v));
            j_str(o, "id", v->id, sizeof(v->id));
            if (!v->id[0]) {
                continue;
            }
            j_str(o, "version", v->version, sizeof(v->version));
            j_str(o, "release_date", v->date, sizeof(v->date));
            j_str(o, "changelog", v->changelog, sizeof(v->changelog));
            j_str(o, "folder_name", v->folder, sizeof(v->folder));
            j_str(o, "region", v->region, sizeof(v->region));
            v->size = j_u64(o, "size");
            const J *f = j_get(o, "download_files");
            if (f && f->t == 'a') {
                for (const J *e = f->child; e; e = e->next) {
                    v->nfiles++;
                }
            }
            n++;
        }
        j_free(j);
    }
    SDL_free(text);
    if (!strcmp(id, S.ver_app)) {
        SDL_SetAtomicInt(&S.nver, n);
        SDL_SetAtomicInt(&S.ver_loading, 0);
    }
    free(id);
    return 0;
}

void st_open_app(StApp *a)
{
    snprintf(S.ver_app, sizeof(S.ver_app), "%s", a->id);
    SDL_SetAtomicInt(&S.nver, 0);
    SDL_SetAtomicInt(&S.ver_loading, 1);
    SDL_Thread *t = SDL_CreateThread(vers_thread, "st-vers", strdup(a->id));
    if (t) {
        SDL_DetachThread(t);
    } else {
        SDL_SetAtomicInt(&S.ver_loading, 0);
    }
}

bool st_ver_loading(void)
{
    return SDL_GetAtomicInt(&S.ver_loading) != 0;
}

int st_nver(void)
{
    return SDL_GetAtomicInt(&S.nver);
}

const StVer *st_ver(int i)
{
    return i >= 0 && i < SDL_GetAtomicInt(&S.nver) ? &S.vers[i] : NULL;
}

/* ---- install ---- */

typedef struct {
    StApp app;
    StVer ver;
} InstArgs;

/* The download's progress for the screen: curl runs to its end in cfg_run, so a thread
 * looks at the growing file (bytes of this install so far; the store's size for the
 * version, 0 when it doesn't say) */
static SDL_AtomicInt g_dl_bytes, g_dl_stop;
static uint64_t g_dl_total;
static int g_dl_phase;      /* 1 downloading, 2 unpacking, 3 installing: the bar's kind */

typedef struct {
    const char *path;
    uint64_t base;
} Watch;

static int watch_thread(void *arg)
{
    Watch *w = arg;
    while (!SDL_GetAtomicInt(&g_dl_stop)) {
        SDL_PathInfo pi;
        if (SDL_GetPathInfo(w->path, &pi)) {
            SDL_SetAtomicInt(&g_dl_bytes, (int)((w->base + pi.size) >> 10));   /* KiB */
        }
        SDL_Delay(150);
    }
    return 0;
}

/* http_get with the file's growth watched; base: what earlier files of the install were */
static bool download_watched(const char *url, const char *to, uint64_t base)
{
    Watch w = {to, base};
    SDL_SetAtomicInt(&g_dl_stop, 0);
    SDL_Thread *t = SDL_CreateThread(watch_thread, "st-watch", &w);
    const bool ok = http_get(url, to, "1800");
    SDL_SetAtomicInt(&g_dl_stop, 1);
    if (t) {
        SDL_WaitThread(t, NULL);
    }
    return ok;
}

static void inst_msg(const char *m)
{
    snprintf(S.inst_msg, sizeof(S.inst_msg), "%s", m);
}

static void safe_folder(const char *name, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)name; *c && o + 1 < n && o < 42; c++) {
        if (*c < 0x20 || *c > 0x7E || strchr("<>:\"/\\|?*", *c)) {
            continue;
        }
        out[o++] = (char)*c;
    }
    out[o] = 0;
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) {
        out[--o] = 0;
    }
    char *s = out;
    while (*s == ' ') {
        s++;
    }
    if (s != out) {
        memmove(out, s, strlen(s) + 1);
    }
    if (!out[0]) {
        snprintf(out, n, "App");
    }
}

static const char *install_group(const char *category)
{
    char c[64];
    snprintf(c, sizeof(c), "%s", category);
    lower(c);
    if (!strcmp(c, "games") || !strcmp(c, "homebrew games")) {
        return "Games";
    }
    if (!strcmp(c, "emulators")) {
        return "Emulators";
    }
    return "Apps";
}

/* Large Google Drive files answer with a "can't scan for viruses" page: the URL of the
 * form that confirms and downloads, or "" when the text isn't that page */
static void drive_confirm_url(const char *html, char *out, size_t n)
{
    out[0] = 0;
    const char *form = strstr(html, "download-form");
    if (!form || !strstr(html, "drive.usercontent.google.com")) {
        return;
    }
    const char *start = form;
    while (start > html && strncmp(start, "<form", 5)) {
        start--;
    }
    const char *end = strstr(form, "</form>");
    if (!end || strncmp(start, "<form", 5)) {
        return;
    }
    const char *act = strstr(start, "action=\"");
    if (!act || act > end) {
        return;
    }
    act += 8;
    const char *ae = strchr(act, '"');
    if (!ae || strncmp(act, "https://drive.usercontent.google.com/", 37)) {
        return;
    }
    size_t o = (size_t)snprintf(out, n, "%.*s", (int)(ae - act), act);
    char sep = '?';
    for (const char *in = strstr(ae, "<input"); in && in < end; in = strstr(in + 6, "<input")) {
        const char *te = strchr(in, '>');
        const char *nm = strstr(in, "name=\""), *vl = strstr(in, "value=\"");
        if (!te || !nm || nm > te) {
            continue;
        }
        nm += 6;
        char name[64] = "", val[256] = "", encn[200], encv[600];
        const char *ne = strchr(nm, '"');
        snprintf(name, sizeof(name), "%.*s", ne ? (int)(ne - nm) : 0, nm);
        if (vl && vl < te) {
            vl += 7;
            const char *ve = strchr(vl, '"');
            snprintf(val, sizeof(val), "%.*s", ve ? (int)(ve - vl) : 0, vl);
        }
        url_encode(name, encn, sizeof(encn));
        url_encode(val, encv, sizeof(encv));
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%c%s=%s", sep, encn, encv);
        sep = '&';
    }
}

static bool is_zip(const char *path)
{
    size_t len = 0;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        return false;
    }
    char m[4] = {0};
    len = SDL_ReadIO(io, m, 4);
    SDL_CloseIO(io);
    return len == 4 && m[0] == 'P' && m[1] == 'K';
}

static bool unzip_to(const char *zip, const char *dest)
{
#ifdef _WIN32
    const char *root = SDL_getenv("SystemRoot");
    char tar[300];
    snprintf(tar, sizeof(tar), "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
    const char *args[] = {tar, "-xf", zip, "-C", dest, NULL};
    return cfg_run(args);
#else
    const char *args[] = {"unzip", "-qo", zip, "-d", dest, NULL};
    return cfg_run(args);
#endif
}

typedef struct {
    int count;
    char only[1200];
    bool only_dir;
} Kids;

static SDL_EnumerationResult SDLCALL kid_entry(void *ud, const char *dirname, const char *fname)
{
    Kids *k = ud;
    k->count++;
    join(k->only, sizeof(k->only), dirname, fname);
    SDL_PathInfo pi;
    k->only_dir = SDL_GetPathInfo(k->only, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
    return SDL_ENUM_CONTINUE;
}

static int install_thread(void *arg)
{
    InstArgs *ia = arg;
    char work[1100], dl[1200], un[1200];
    data_sub(work, sizeof(work), "store/install");
    remove_tree(work);
    mkdirs(work);
    join(un, sizeof(un), work, "files");
    mkdirs(un);
    const int nfiles = ia->ver.nfiles > 0 ? ia->ver.nfiles : 1;
    bool ok = true;
    uint64_t done_files = 0;
    SDL_SetAtomicInt(&g_dl_bytes, 0);
    g_dl_total = ia->ver.size;
    g_dl_phase = 1;
    char enc[160];
    url_encode(ia->ver.id, enc, sizeof(enc));
    for (int f = 0; f < nfiles && ok; f++) {
        char msg[160], url[600];
        snprintf(msg, sizeof(msg), nfiles > 1 ? "Downloading %s (file %d of %d)..." : "Downloading %s...", ia->app.name, f + 1,
                 nfiles);
        inst_msg(msg);
        join(dl, sizeof(dl), work, "download.zip");
        remove(dl);
        snprintf(url, sizeof(url), "%s/api/Download/%s?fileIndex=%d", API, enc, f);
        if (!download_watched(url, dl, done_files)) {
            inst_msg("The download failed.");
            ok = false;
            break;
        }
        if (!is_zip(dl)) {
            /* a Google Drive page asking to confirm: its form's URL, once */
            size_t len = 0;
            char *html = SDL_LoadFile(dl, &len);
            char again[1400] = "";
            if (html && len < (4u << 20)) {
                drive_confirm_url(html, again, sizeof(again));
            }
            SDL_free(html);
            if (again[0]) {
                remove(dl);
                ok = download_watched(again, dl, done_files);
            }
            if (!ok || !is_zip(dl)) {
                inst_msg("The download isn't a zip file.");
                ok = false;
                break;
            }
        }
        {
            SDL_PathInfo pi;
            if (SDL_GetPathInfo(dl, &pi)) {
                done_files += pi.size;
            }
        }
        g_dl_phase = 2;
        inst_msg("Unpacking...");
        if (!unzip_to(dl, un)) {
            inst_msg("The zip file couldn't be unpacked.");
            ok = false;
            break;
        }
    }
    if (ok) {
        g_dl_phase = 3;
        inst_msg("Installing...");
        Kids k = {0};
        SDL_EnumerateDirectory(un, kid_entry, &k);
        const char *root = k.count == 1 && k.only_dir ? k.only : un;
        char e[1100], folder[64], dest[1400];
        hb_e_dir(e, sizeof(e));
        safe_folder(ia->ver.folder[0] ? ia->ver.folder : ia->app.name, folder, sizeof(folder));
        char gdir[1200];
        join(gdir, sizeof(gdir), e, install_group(ia->app.category));
        join(dest, sizeof(dest), gdir, folder);
        if (exists(dest)) {
            remove_tree(dest);
        }
        ok = move_tree(root, dest);
        if (ok) {
            char mk[1500];
            join(mk, sizeof(mk), dest, "xbased-store.txt");
            SDL_IOStream *io = SDL_IOFromFile(mk, "wb");
            if (io) {
                char body[200];
                const int l = snprintf(body, sizeof(body), "%s\n%s\n", ia->app.id, ia->ver.version);
                SDL_WriteIO(io, body, (size_t)l);
                SDL_CloseIO(io);
            }
            char done[200];
            snprintf(done, sizeof(done), "Installed in %s/%s.", install_group(ia->app.category), folder);
            inst_msg(done);
        } else {
            inst_msg("It couldn't be put on the hard disk.");
        }
    }
    remove_tree(work);
    free(ia);
    SDL_SetAtomicInt(&S.inst_state, ok ? 2 : 3);
    return 0;
}

void st_install(const StApp *a)
{
    if (!SDL_CompareAndSwapAtomicInt(&S.inst_state, 0, 1)) {
        return;
    }
    if (st_ver_loading() || st_nver() <= 0) {
        inst_msg("The versions haven't arrived yet.");
        SDL_SetAtomicInt(&S.inst_state, 3);
        return;
    }
    int pick = 0;
    for (int i = 0; i < st_nver(); i++) {
        if (a->latest[0] && !strcmp(S.vers[i].id, a->latest)) {
            pick = i;
            break;
        }
    }
    InstArgs *ia = calloc(1, sizeof(*ia));
    ia->app = *a;
    ia->ver = S.vers[pick];
    inst_msg("Starting...");
    SDL_Thread *t = SDL_CreateThread(install_thread, "st-install", ia);
    if (t) {
        SDL_DetachThread(t);
    } else {
        free(ia);
        SDL_SetAtomicInt(&S.inst_state, 0);
    }
}

/* 0-99 while the size is known, -1 when it isn't (or after the download: the bar just moves) */
void st_install_progress(int *pct, uint64_t *bytes, uint64_t *total, int *phase)
{
    const uint64_t b = (uint64_t)SDL_GetAtomicInt(&g_dl_bytes) << 10;
    *bytes = b;
    *total = g_dl_total;
    *phase = g_dl_phase;
    if (g_dl_phase == 1 && g_dl_total > 0 && b <= g_dl_total + (g_dl_total >> 3)) {
        const uint64_t p = b * 100 / g_dl_total;
        *pct = p > 99 ? 99 : (int)p;
    } else {
        *pct = -1;
    }
}

int st_install_state(void)
{
    return SDL_GetAtomicInt(&S.inst_state);
}

const char *st_install_msg(void)
{
    return S.inst_msg;
}

void st_install_ack(void)
{
    const int s = SDL_GetAtomicInt(&S.inst_state);
    if (s == 2 || s == 3) {
        SDL_SetAtomicInt(&S.inst_state, 0);
    }
}
