/*
 * xbased — the disc scanned for textures (automatic texture upscaling): a game's textures written out for the
 * upscaler without the game having to load each of them once. It finds the textures kept in the XDK's own
 * container, XPR0 ("Xbox packed resource": a header of resource descriptions, each texture's the same five
 * words Direct3D registers it with, then the data they point into), wherever one lies in the disc image or the
 * game's folder, alone or inside another file, as long as it isn't compressed. A game that keeps its textures
 * in a format of its own, as many of the large ones do, gives nothing here: those are still learned as it
 * plays.
 *
 * A texture found is written to the title's dump/ under the hash the game's own definition of it gets (the
 * same bytes: the game hands Direct3D the container's data as it is), so the upscaler's picture of it is used
 * when the game comes to load it. Palettized textures are left out (their palette is bound at run time).
 * Once a disc: dump/scan.done.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw/xbox/hle/pb/pbres.h"
#include "hw/xbox/hle/pb/texpack.h"
#include "native/app/app.h"
#include "native/core/nc.h"

typedef struct Scan {
    char disc[1100], dir[1100];
    unsigned containers, found, written;
    uint64_t bytes;
} Scan;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int seek64(FILE *f, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

/* The container at `at` of f: its textures written out. */
static void container(Scan *s, FILE *f, uint64_t at)
{
    uint8_t h[12];
    if (seek64(f, at) || fread(h, 1, 12, f) != 12) {
        return;
    }
    const uint32_t total = rd32(h + 4), hdr = rd32(h + 8);
    /* a header of descriptions, then data: sizes a real one has */
    if (hdr < 32 || (hdr & 3) || hdr > (4u << 20) || total <= hdr || total > (512u << 20)) {
        return;
    }
    uint8_t *head = malloc(hdr);
    if (!head || seek64(f, at) || fread(head, 1, hdr, f) != hdr) {
        free(head);
        return;
    }
    uint8_t *data = NULL;
    uint32_t dlen = total - hdr;
    unsigned here = 0;
    for (uint32_t o = 12; o + 20 <= hdr; o += 4) {
        /* a texture's description: Direct3D's "common" word of a texture with one reference, where its data
         * starts, a lock word of 0, its format and its size */
        const uint32_t common = rd32(head + o), off = rd32(head + o + 4), lock = rd32(head + o + 8);
        if (common == 0xFFFFFFFFu) {
            break;              /* the descriptions' end */
        }
        if ((common & 0x0007FFFFu) != 0x00040001u || lock || off >= dlen) {
            continue;
        }
        if (!data) {
            data = malloc(dlen);
            if (!data || seek64(f, at + hdr)) {
                break;
            }
            dlen = (uint32_t)fread(data, 1, dlen, f);   /* a container cut short gives what is there */
            if (off >= dlen) {
                continue;
            }
        }
        uint64_t hash;
        uint8_t *rgba = NULL;
        uint32_t w, hgt;
        if (hle_draw_tex_scan(common, rd32(head + o + 12), rd32(head + o + 16), data + off, dlen - off, &hash, &rgba,
                              &w, &hgt)) {
            s->found++;
            here++;
            if (hle_texpack_scan_dump(s->dir, hash, rgba, w, hgt)) {
                s->written++;
            }
            free(rgba);
            o += 16;            /* past this description */
        }
    }
    s->containers += here > 0;
    free(data);
    free(head);
}

/* One file: every "XPR0" in it tried as a container. */
static void scan_file(Scan *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    enum { CHUNK = 8 << 20 };
    uint8_t *buf = malloc(CHUNK + 4);
    uint64_t base = 0;
    size_t keep = 0;
    /* the places first (the file read through once), then the containers */
    uint64_t *hits = NULL;
    size_t nhits = 0, cap = 0;
    while (buf) {
        const size_t n = fread(buf + keep, 1, CHUNK, f);
        const size_t len = keep + n;
        for (size_t i = 0; i + 4 <= len;) {
            const uint8_t *p = memchr(buf + i, 'X', len - 3 - i);
            if (!p) {
                break;
            }
            i = (size_t)(p - buf);
            if (p[1] == 'P' && p[2] == 'R' && p[3] == '0') {
                if (nhits == cap) {
                    cap = cap ? cap * 2 : 256;
                    hits = realloc(hits, cap * sizeof(*hits));
                }
                if (hits && nhits < 200000) {
                    hits[nhits++] = base + i;
                }
            }
            i++;
        }
        s->bytes += n;
        if (n < CHUNK) {
            break;
        }
        /* the last three bytes again with the next chunk: a tag across the join */
        memmove(buf, buf + len - 3, 3);
        base += len - 3;
        keep = 3;
    }
    for (size_t i = 0; hits && i < nhits; i++) {
        container(s, f, hits[i]);
    }
    free(hits);
    free(buf);
    fclose(f);
}

static void scan_tree(Scan *s, const char *dir, int depth)
{
    int n = 0;
    char **names = SDL_GlobDirectory(dir, NULL, 0, &n);
    for (int i = 0; names && i < n; i++) {
        char p[1400];
        SDL_PathInfo pi;
        SDL_snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        if (!SDL_GetPathInfo(p, &pi)) {
            continue;
        }
        if (pi.type == SDL_PATHTYPE_DIRECTORY) {
            if (depth < 8) {
                scan_tree(s, p, depth + 1);
            }
        } else {
            scan_file(s, p);
        }
    }
    SDL_free(names);
}

static int SDLCALL scan_thread(void *arg)
{
    Scan *s = arg;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    const Uint64 t0 = SDL_GetTicks();
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(s->disc, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
        scan_tree(s, s->disc, 0);
    } else {
        scan_file(s, s->disc);
    }
    char mark[1200];
    SDL_snprintf(mark, sizeof(mark), "%sdump/scan.done", s->dir);
    FILE *m = fopen(mark, "wb");
    if (m) {
        fprintf(m, "%u textures in %u containers, %u written\n", s->found, s->containers, s->written);
        fclose(m);
    }
    nc_log("hakuX-app", "texture scan: %s read (%.0f MiB in %.1f s): %u textures in %u XPR containers, %u written "
           "out for the upscaler%s", s->disc, (double)s->bytes / 1048576.0, (double)(SDL_GetTicks() - t0) / 1000.0,
           s->found, s->containers, s->written,
           s->found ? "" : " (this game keeps its textures another way: they are learned as it plays)");
    free(s);
    return 0;
}

bool nc_texscan_start(const char *disc, const char *dir)
{
    if (!disc || !*disc || !dir || !*dir) {
        return false;
    }
    char mark[1200];
    SDL_snprintf(mark, sizeof(mark), "%sdump/scan.done", dir);
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(mark, &pi)) {
        return false;           /* scanned before */
    }
    Scan *s = calloc(1, sizeof(*s));
    if (!s) {
        return false;
    }
    SDL_snprintf(s->disc, sizeof(s->disc), "%s", disc);
    SDL_snprintf(s->dir, sizeof(s->dir), "%s", dir);
    SDL_Thread *t = SDL_CreateThread(scan_thread, "texscan", s);
    if (!t) {
        free(s);
        return false;
    }
    SDL_DetachThread(t);
    return true;
}
