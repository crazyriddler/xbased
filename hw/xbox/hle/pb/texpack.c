/*
 * hakuX HLE GPU — texture packs (texpack.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/pb/texpack.h"
#include <sys/stat.h>
#include <time.h>
#define XXH_INLINE_ALL
#include "xxhash.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "native/third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "native/third_party/stb/stb_image_write.h"

/* a set of hashes (open addressing; 0 stands for itself in `zero`) */
typedef struct Set {
    uint64_t *v;
    size_t cap, n;
    bool zero;
} Set;

static bool set_has(const Set *s, uint64_t k)
{
    if (!k) {
        return s->zero;
    }
    if (!s->cap) {
        return false;
    }
    for (size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull >> 32) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
        if (s->v[i] == k) {
            return true;
        }
        if (!s->v[i]) {
            return false;
        }
    }
}

static void set_add(Set *s, uint64_t k)
{
    if (!k) {
        s->zero = true;
        return;
    }
    if (set_has(s, k)) {
        return;
    }
    if ((s->n + 1) * 2 > s->cap) {
        Set o = *s;
        s->cap = o.cap ? o.cap * 2 : 1024;
        s->v = g_malloc0(s->cap * sizeof(uint64_t));
        s->n = 0;
        for (size_t i = 0; i < o.cap; i++) {
            if (o.v[i]) {
                set_add(s, o.v[i]);
            }
        }
        g_free(o.v);
    }
    size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull >> 32) & (s->cap - 1);
    while (s->v[i]) {
        i = (i + 1) & (s->cap - 1);
    }
    s->v[i] = k;
    s->n++;
}

/* g_absent: no picture's file was there (asked once a run, unless pictures are made while the game runs);
 * g_refused: there is one and it isn't to be used; g_present: there is one; g_dumped: written out this run (or
 * found written); g_wait: a texture with a dump, whose picture can still come */
static Set g_absent, g_refused, g_present, g_dumped, g_wait;

/* Pictures are made while the game runs (hle_host_texpack bits 2 and 3): one that isn't there may come. */
static bool live(void)
{
    return (hle_host_texpack() & 12) == 12;
}

/* How long ago a file was last written, in seconds; -1 without one. */
static double file_age(const char *path)
{
    struct stat st;
    if (stat(path, &st)) {
        return -1.0;
    }
    return difftime(time(NULL), st.st_mtime);
}
static unsigned g_nloaded, g_ndumped, g_nrejected, g_nused;

/* <base>textures/<title id>/ (and its dump/), made at first use */
static char *dir;

const char *hle_texpack_dir(void)
{
    return dir;
}

static const char *pack_dir(void)
{
    if (!dir) {
        dir = g_strdup_printf("%stextures/%08X/", hle_host_base_path(), g_hle.title.xbe.title_id);
        g_autofree char *dump = g_strdup_printf("%sdump", dir);
        g_autofree char *aut = g_strdup_printf("%sauto", dir);
        g_mkdir_with_parents(dump, 0775);
        g_mkdir_with_parents(aut, 0775);
        hle_log("hakuX-hle-front", "texture packs: %s (PNG files named by the textures' hashes; dump/ holds the "
                "game's own, written out when that is on; auto/ those an upscaler made of them)", dir);
    }
    return dir;
}

uint64_t hle_texpack_hash(const void *data, size_t len, uint32_t fmt, uint32_t w, uint32_t h, const uint32_t *pal,
                          uint32_t npal)
{
    uint64_t x = XXH3_64bits(data, len);
    if (pal && npal) {
        x ^= XXH3_64bits(pal, (size_t)npal * 4) * 0x9E3779B97F4A7C15ull;
    }
    const uint32_t tail[4] = { fmt, w, h, (uint32_t)len };
    return x ^ (XXH3_64bits(tail, sizeof(tail)) * 0xC2B2AE3D27D4EB4Full);
}

static bool file_there(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
    }
    return f != NULL;
}

bool hle_texpack_has(uint64_t hash)
{
    if (set_has(&g_refused, hash) || (!live() && set_has(&g_absent, hash))) {
        return false;
    }
    if (set_has(&g_present, hash)) {
        return true;
    }
    g_autofree char *own = g_strdup_printf("%s%016llx.png", pack_dir(), (unsigned long long)hash);
    g_autofree char *aut = g_strdup_printf("%sauto/%016llx.png", pack_dir(), (unsigned long long)hash);
    g_autofree char *bad = g_strdup_printf("%sauto/%016llx.bad", pack_dir(), (unsigned long long)hash);
    /* the upscaler's: once it is whole (two seconds since it was written to) */
    const bool there = file_there(own) || (file_age(aut) >= 2.0 && !file_there(bad));
    if (there) {
        set_add(&g_present, hash);
    } else if (!live()) {
        set_add(&g_absent, hash);
    }
    return there;
}

static void touch(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fclose(f);
    }
}

/* An upscaler's picture was refused. The first time, it and the texture's dump are removed, so the texture is
 * written out again (colour alone now) and upscaled again after this run (auto/<hash>.redo remembers); refused
 * a second time, auto/<hash>.bad says to leave it (neither read nor made again). */
void hle_texpack_reject(uint64_t hash, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t fmt, bool decoded,
                        float colour)
{
    const unsigned long long x = (unsigned long long)hash;
    g_autofree char *png = g_strdup_printf("%sauto/%016llx.png", pack_dir(), x);
    g_autofree char *redo = g_strdup_printf("%sauto/%016llx.redo", pack_dir(), x);
    g_autofree char *bad = g_strdup_printf("%sauto/%016llx.bad", pack_dir(), x);
    g_autofree char *dump = g_strdup_printf("%sdump/%016llx.png", pack_dir(), x);
    const bool again = !file_there(redo);
    if (again) {
        touch(redo);
        remove(dump);
    } else {
        touch(bad);
    }
    remove(png);
    set_add(&g_refused, hash);
    if (g_nrejected++ < 24) {
        hle_log("hakuX-hle-front", "texture packs: auto/%016llx.png (%ux%u) not used: the game's texture is %ux%u "
                "format 0x%02X%s, colour off by %.1f of 255 (28 allowed); %s (#%u of 24 logged)", x, w, h, ow, oh,
                fmt, decoded ? "" : " (not decoded)", (double)colour,
                again ? "to be made again after this run" : "the second time: left alone from now on", g_nrejected);
    }
}

void hle_texpack_used(uint64_t hash, uint32_t w, uint32_t h, bool from_auto)
{
    g_nused++;
    if (g_nloaded++ < 16) {
        hle_log("hakuX-hle-front", "texture packs: %s%016llx.png %ux%u in use (#%u of 16 logged)",
                from_auto ? "auto/" : "", (unsigned long long)hash, w, h, g_nloaded);
    }
}

void hle_texpack_counts(unsigned *used, unsigned *rejected)
{
    *used = g_nused;
    *rejected = g_nrejected;
}

uint8_t *hle_texpack_load(uint64_t hash, uint32_t *w, uint32_t *h, bool *from_auto)
{
    *from_auto = false;
    if (set_has(&g_refused, hash) || (!live() && set_has(&g_absent, hash))) {
        return NULL;
    }
    /* your own file first, then the upscaler's */
    g_autofree char *path = g_strdup_printf("%s%016llx.png", pack_dir(), (unsigned long long)hash);
    gchar *file = NULL;
    gsize len = 0;
    bool made = false;
    if (!g_file_get_contents(path, &file, &len, NULL) || !file) {
        g_free(path);
        path = g_strdup_printf("%sauto/%016llx.png", pack_dir(), (unsigned long long)hash);
        made = true;
        if (!g_file_get_contents(path, &file, &len, NULL) || !file) {
            if (!live()) {
                set_add(&g_absent, hash);
            }
            return NULL;
        }
    }
    int iw = 0, ih = 0, comp = 0;
    uint8_t *px = len <= INT32_MAX ? stbi_load_from_memory((const stbi_uc *)file, (int)len, &iw, &ih, &comp, 4) : NULL;
    g_free(file);
    if (!px && made && file_age(path) < 5.0) {
        return NULL;                    /* the upscaler is still writing it: asked again later */
    }
    if (!px || iw < 1 || ih < 1 || iw > 8192 || ih > 8192) {
        hle_log("hakuX-hle-front", "texture packs: %s not used (%s)", path,
                px ? "larger than 8192 pixels a side" : "not a PNG this reads");
        if (px) {
            stbi_image_free(px);
        } else if (made) {
            remove(path);               /* an upscale cut short: made again after the next run */
        }
        set_add(&g_refused, hash);
        return NULL;
    }
    *w = (uint32_t)iw;
    *h = (uint32_t)ih;
    *from_auto = made;
    return px;
}

void hle_texpack_free(uint8_t *rgba)
{
    stbi_image_free(rgba);
}

void hle_texpack_note_dumped(uint64_t hash)
{
    set_add(&g_wait, hash);
}

bool hle_texpack_awaited(uint64_t hash)
{
    return live() && set_has(&g_wait, hash) && !set_has(&g_refused, hash);
}

bool hle_texpack_ready(uint64_t hash)
{
    if (set_has(&g_refused, hash)) {
        return false;
    }
    g_autofree char *aut = g_strdup_printf("%sauto/%016llx.png", pack_dir(), (unsigned long long)hash);
    return file_age(aut) >= 2.0;
}

bool hle_texpack_dump_wanted(uint64_t hash)
{
    if (set_has(&g_dumped, hash)) {
        return false;
    }
    set_add(&g_dumped, hash);
    return true;
}

typedef struct Out {
    uint8_t *p;
    size_t n, cap;
} Out;

static void out_write(void *ctx, void *data, int size)
{
    Out *o = ctx;
    if (size <= 0) {
        return;
    }
    if (o->n + (size_t)size > o->cap) {
        o->cap = MAX(o->cap * 2, o->n + (size_t)size + 4096);
        o->p = g_realloc(o->p, o->cap);
    }
    memcpy(o->p + o->n, data, (size_t)size);
    o->n += (size_t)size;
}

/* A texture's colour as a three-channel PNG at `path`, unless a file is there. (The upscaler's handling of an
 * alpha channel is not to be relied on, and the texture's own alpha is what is drawn in any case: pbres.c
 * texpack_alpha.) False when nothing was written. */
static bool dump_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    FILE *have = fopen(path, "rb");
    if (have) {
        fclose(have);                   /* written by an earlier run */
        return false;
    }
    const size_t n = (size_t)w * h;
    g_autofree uint8_t *rgb = g_malloc(n * 3);
    for (size_t i = 0; i < n; i++) {
        rgb[i * 3] = rgba[i * 4];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    Out o = { 0 };
    const bool ok = stbi_write_png_to_func(out_write, &o, (int)w, (int)h, 3, rgb, (int)w * 3) && o.n &&
                    g_file_set_contents(path, (const gchar *)o.p, (gssize)o.n, NULL);
    g_free(o.p);
    return ok;
}

void hle_texpack_dump(uint64_t hash, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    g_autofree char *path = g_strdup_printf("%sdump/%016llx.png", pack_dir(), (unsigned long long)hash);
    if (dump_write(path, rgba, w, h)) {
        if (g_ndumped++ < 8 || !(g_ndumped & 255)) {
            hle_log("hakuX-hle-front", "texture packs: wrote dump/%016llx.png (%ux%u; %u so far)",
                    (unsigned long long)hash, w, h, g_ndumped);
        }
    }
}

bool hle_texpack_scan_dump(const char *dir, uint64_t hash, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    g_autofree char *aut = g_strdup_printf("%sauto/%016llx.png", dir, (unsigned long long)hash);
    g_autofree char *own = g_strdup_printf("%s%016llx.png", dir, (unsigned long long)hash);
    g_autofree char *path = g_strdup_printf("%sdump/%016llx.png", dir, (unsigned long long)hash);
    if (file_there(aut) || file_there(own)) {
        return false;                   /* has its picture already */
    }
    return dump_write(path, rgba, w, h);
}
