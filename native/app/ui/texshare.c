/*
 * xbased — HD texture packs shared between PCs.
 *
 * A pack is a zip of what a game's textures folder holds: <title id>/auto/<hash>.png (an upscaler's pictures:
 * checked against the game's texture and given its alpha when drawn, as on the PC that made them) and
 * <title id>/<hash>.png (hand-made ones: drawn as they are), with a pack.txt that says which game. Exporting
 * writes one (stored, not compressed: PNGs don't shrink) into "texture packs/exported/"; a zip put into
 * "texture packs/" is installed when the dashboard next starts and then moved to its installed/ folder. A pack
 * zipped again by another program (deflate) is read too. Only entries with those names are taken out of a zip,
 * and only into the textures folder.
 */
#include "native/app/ui/texshare.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_image's inflate, for packs another program compressed */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "native/third_party/stb/stb_image.h"

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static void w16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t v)
{
    w16(p, v);
    w16(p + 2, v >> 16);
}

static uint32_t r16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t r32(const uint8_t *p)
{
    return r16(p) | r16(p + 2) << 16;
}

/* ---- writing ---- */

typedef struct Zip {
    FILE *f;
    uint8_t *dir;               /* the central directory so far */
    size_t dir_n, dir_cap;
    uint32_t entries, at;
} Zip;

static bool zip_add(Zip *z, const char *name, const uint8_t *data, uint32_t n)
{
    const uint32_t nl = (uint32_t)strlen(name), crc = crc32_of(data, n);
    uint8_t h[30] = { 0 };
    w32(h, 0x04034B50u);
    w16(h + 4, 20);
    w16(h + 6, 0x0800);         /* names in UTF-8 */
    w16(h + 12, 0x0021);        /* 1 January 1980: a date every program takes */
    w32(h + 14, crc);
    w32(h + 18, n);
    w32(h + 22, n);
    w16(h + 26, nl);
    if (fwrite(h, 1, 30, z->f) != 30 || fwrite(name, 1, nl, z->f) != nl || fwrite(data, 1, n, z->f) != n) {
        return false;
    }
    if (z->dir_n + 46 + nl > z->dir_cap) {
        z->dir_cap = (z->dir_cap + 46 + nl) * 2;
        z->dir = realloc(z->dir, z->dir_cap);
        if (!z->dir) {
            return false;
        }
    }
    uint8_t *c = z->dir + z->dir_n;
    memset(c, 0, 46);
    w32(c, 0x02014B50u);
    w16(c + 4, 20);
    w16(c + 6, 20);
    w16(c + 8, 0x0800);
    w16(c + 14, 0x0021);
    w32(c + 16, crc);
    w32(c + 20, n);
    w32(c + 24, n);
    w16(c + 28, nl);
    w32(c + 42, z->at);
    memcpy(c + 46, name, nl);
    z->dir_n += 46 + nl;
    z->at += 30 + nl + n;
    z->entries++;
    return true;
}

static bool zip_close(Zip *z)
{
    bool ok = z->f != NULL;
    if (ok) {
        uint8_t e[22] = { 0 };
        w32(e, 0x06054B50u);
        w16(e + 8, z->entries);
        w16(e + 10, z->entries);
        w32(e + 12, (uint32_t)z->dir_n);
        w32(e + 16, z->at);
        ok = fwrite(z->dir, 1, z->dir_n, z->f) == z->dir_n && fwrite(e, 1, 22, z->f) == 22;
        ok = fclose(z->f) == 0 && ok;
    }
    free(z->dir);
    memset(z, 0, sizeof(*z));
    return ok;
}

/* "<16 hex digits>.png" */
static bool hash_name(const char *n, size_t len)
{
    if (len != 20 || SDL_strncasecmp(n + 16, ".png", 4)) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (!SDL_isxdigit((unsigned char)n[i])) {
            return false;
        }
    }
    return true;
}

static void safe_title(const char *title, char *out, size_t n)
{
    size_t k = 0;
    for (const char *p = title ? title : ""; *p && k + 1 < n && k < 80; p++) {
        const unsigned char c = (unsigned char)*p;
        out[k++] = c < 32 || strchr("<>:\"/\\|?*", c) ? '_' : (char)c;
    }
    while (k && (out[k - 1] == ' ' || out[k - 1] == '.')) {
        k--;
    }
    out[k] = 0;
    if (!k) {
        SDL_snprintf(out, n, "game");
    }
}

int texshare_export(const char *data_dir, uint32_t title_id, const char *title, char *out, size_t out_n)
{
    char base[1200], dir[1300], name[100], path[1500];
    out[0] = 0;
    SDL_snprintf(base, sizeof(base), "%shle/textures/%08X", data_dir, title_id);
    safe_title(title, name, sizeof(name));
    SDL_snprintf(dir, sizeof(dir), "%stexture packs", data_dir);
    SDL_CreateDirectory(dir);
    SDL_snprintf(dir, sizeof(dir), "%stexture packs/exported", data_dir);
    SDL_CreateDirectory(dir);
    Zip z = { 0 };
    int total = 0, part = 0;
    bool ok = true;
    for (int sub = 0; sub < 2 && ok; sub++) {
        char from[1400];
        SDL_snprintf(from, sizeof(from), "%s%s", base, sub ? "/auto" : "");
        int n = 0;
        char **names = SDL_GlobDirectory(from, "*.png", 0, &n);
        for (int i = 0; names && i < n && ok; i++) {
            if (!hash_name(names[i], strlen(names[i]))) {
                continue;
            }
            SDL_snprintf(path, sizeof(path), "%s/%s", from, names[i]);
            size_t len = 0;
            uint8_t *data = SDL_LoadFile(path, &len);
            if (!data || len < 8 || len > (512u << 20)) {
                SDL_free(data);
                continue;
            }
            /* a part ends before it passes what a plain zip holds safely */
            if (z.f && ((uint64_t)z.at + len > 1900ull << 20 || z.entries >= 60000)) {
                ok = zip_close(&z);
            }
            if (ok && !z.f) {
                part++;
                if (part == 1) {
                    SDL_snprintf(path, sizeof(path), "%s/%s [%08X] HD textures.zip", dir, name, title_id);
                    SDL_snprintf(out, out_n, "%s", path);
                } else {
                    SDL_snprintf(path, sizeof(path), "%s/%s [%08X] HD textures part %d.zip", dir, name, title_id, part);
                }
                z.f = fopen(path, "wb");
                ok = z.f != NULL;
                if (ok) {
                    char txt[400];
                    const int tl = SDL_snprintf(txt, sizeof(txt), "xbased HD texture pack\r\ngame: %s\r\ntitle id: %08X\r\n"
                                                "part: %d\r\nTo install: put this zip into xbased's \"texture packs\" "
                                                "folder and start xbased.\r\n", title ? title : "", title_id, part);
                    ok = zip_add(&z, "pack.txt", (const uint8_t *)txt, (uint32_t)tl);
                }
            }
            if (ok) {
                char en[64];
                SDL_snprintf(en, sizeof(en), "%08X/%s%s", title_id, sub ? "auto/" : "", names[i]);
                ok = zip_add(&z, en, data, (uint32_t)len);
                total += ok;
            }
            SDL_free(data);
        }
        SDL_free(names);
    }
    if (z.f) {
        ok = zip_close(&z) && ok;
    }
    return ok ? total : -1;
}

/* ---- reading ---- */

/* "XXXXXXXX/<hash>.png" or "XXXXXXXX/auto/<hash>.png": nothing else leaves a zip */
static bool entry_ok(const char *n, size_t len)
{
    if (len < 29 || n[8] != '/') {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (!SDL_isxdigit((unsigned char)n[i])) {
            return false;
        }
    }
    const char *r = n + 9;
    size_t rl = len - 9;
    if (rl == 25 && !SDL_strncmp(r, "auto/", 5)) {
        r += 5;
        rl -= 5;
    }
    return hash_name(r, rl);
}

static int import_zip(const char *zip, const char *data_dir)
{
    FILE *f = fopen(zip, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (size < 22) {
        fclose(f);
        return -1;              /* not a zip (or one of 2 GB and more: a part of an export never is) */
    }
    const long tail = size < 65536 + 22 ? size : 65536 + 22;
    uint8_t *t = malloc((size_t)tail);
    int done = -1;
    if (t && !fseek(f, size - tail, SEEK_SET) && fread(t, 1, (size_t)tail, f) == (size_t)tail) {
        long e = tail - 22;
        while (e >= 0 && r32(t + e) != 0x06054B50u) {
            e--;
        }
        if (e >= 0) {
            const uint32_t entries = r16(t + e + 10), dn = r32(t + e + 12), da = r32(t + e + 16);
            uint8_t *d = dn && dn < (64u << 20) ? malloc(dn) : NULL;
            if (d && !fseek(f, (long)da, SEEK_SET) && fread(d, 1, dn, f) == dn) {
                done = 0;
                uint32_t at = 0;
                for (uint32_t i = 0; i < entries && at + 46 <= dn && r32(d + at) == 0x02014B50u; i++) {
                    const uint32_t method = r16(d + at + 10), crc = r32(d + at + 16), cs = r32(d + at + 20);
                    const uint32_t us = r32(d + at + 24), nl = r16(d + at + 28), lo = r32(d + at + 42);
                    const uint32_t skip = 46 + nl + r16(d + at + 30) + r16(d + at + 32);
                    const char *name = (const char *)d + at + 46;
                    if (at + skip > dn) {
                        break;
                    }
                    at += skip;
                    uint8_t lh[30];
                    if (!entry_ok(name, nl) || (method != 0 && method != 8) || !cs || cs > (512u << 20) ||
                        us > (512u << 20) || fseek(f, (long)lo, SEEK_SET) || fread(lh, 1, 30, f) != 30 ||
                        r32(lh) != 0x04034B50u || fseek(f, (long)(r16(lh + 26) + r16(lh + 28)), SEEK_CUR)) {
                        continue;
                    }
                    uint8_t *c = malloc(cs), *u = NULL;
                    uint32_t un = 0;
                    if (c && fread(c, 1, cs, f) == cs) {
                        if (method == 0) {
                            u = c;
                            un = cs;
                        } else {
                            int ol = 0;
                            u = (uint8_t *)stbi_zlib_decode_noheader_malloc((const char *)c, (int)cs, &ol);
                            un = u && ol > 0 ? (uint32_t)ol : 0;
                        }
                    }
                    if (u && un == us && crc32_of(u, un) == crc) {
                        char dir[1300], path[1400];
                        SDL_snprintf(dir, sizeof(dir), "%shle", data_dir);
                        SDL_CreateDirectory(dir);
                        SDL_snprintf(dir, sizeof(dir), "%shle/textures", data_dir);
                        SDL_CreateDirectory(dir);
                        SDL_snprintf(dir, sizeof(dir), "%shle/textures/%.8s", data_dir, name);
                        SDL_CreateDirectory(dir);
                        if (nl > 29) {
                            SDL_snprintf(dir, sizeof(dir), "%shle/textures/%.8s/auto", data_dir, name);
                            SDL_CreateDirectory(dir);
                        }
                        SDL_snprintf(path, sizeof(path), "%shle/textures/%.*s", data_dir, (int)nl, name);
                        if (SDL_SaveFile(path, u, un)) {
                            done++;
                        }
                    }
                    if (u != c) {
                        free(u);
                    }
                    free(c);
                }
            }
            free(d);
        }
    }
    free(t);
    fclose(f);
    return done;
}

struct Zl {
    char **v;
    int n;
};

static SDL_EnumerationResult SDLCALL zip_name_cb(void *ud, const char *dirname, const char *fname)
{
    (void)dirname;
    struct Zl *z = ud;
    const size_t l = SDL_strlen(fname);
    if (l > 4 && !SDL_strcasecmp(fname + l - 4, ".zip")) {
        char **nv = SDL_realloc(z->v, (z->n + 2) * sizeof(char *));
        if (nv) {
            z->v = nv;
            z->v[z->n++] = SDL_strdup(fname);
            z->v[z->n] = NULL;
        }
    }
    return SDL_ENUM_CONTINUE;
}

int texshare_import_all(const char *data_dir, char *note, size_t note_n)
{
    char dir[1200], inst[1300];
    note[0] = 0;
    SDL_snprintf(dir, sizeof(dir), "%stexture packs", data_dir);
    int n = 0, zips = 0, pictures = 0, bad = 0;
    /* only the zips lying directly in the folder: SDL_GlobDirectory walks every folder below too, and the
     * exported and installed packs there hold thousands of pictures, read at each start of the dashboard */
    char **names = NULL;
    {
        struct Zl zl = { NULL, 0 };
        SDL_EnumerateDirectory(dir, zip_name_cb, &zl);
        names = zl.v;
        n = zl.n;
    }
    for (int i = 0; names && i < n; i++) {
        char from[1500], to[1500];
        SDL_snprintf(from, sizeof(from), "%s/%s", dir, names[i]);
        const int got = import_zip(from, data_dir);
        if (got < 0) {
            bad++;
            continue;           /* left where it is */
        }
        zips++;
        pictures += got;
        SDL_snprintf(inst, sizeof(inst), "%s/installed", dir);
        SDL_CreateDirectory(inst);
        SDL_snprintf(to, sizeof(to), "%s/%s", inst, names[i]);
        SDL_RemovePath(to);
        SDL_RenamePath(from, to);
    }
    for (int i = 0; names && i < n; i++) {
        SDL_free(names[i]);
    }
    SDL_free(names);
    if (zips || bad) {
        SDL_snprintf(note, note_n, "Texture packs: %d pictures installed from %d zip%s%s", pictures, zips,
                     zips == 1 ? "" : "s", bad ? " (a zip couldn't be read)" : "");
    }
    return pictures;
}
