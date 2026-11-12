/*
 * xbased front end — a folder laid out as an Xbox disc image (xiso_build.h).
 *
 * XDVDFS in brief: sector 32 holds the volume descriptor, which points at the root
 * directory table. A directory table is a binary search tree of entries (left and
 * right child offsets in 4-byte units; the first entry is the tree's root) ordered
 * by a case-insensitive name compare; entries are 4-byte aligned, never cross a
 * sector, and unused bytes are 0xFF. Each entry gives the start sector and size of a
 * file, or of a folder's own table.
 */
#include "native/app/ui/xiso_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u
#define VOLUME_SECTOR 32u
#define IMAGE_MODULUS 0x10000ull
#define ATTR_DIR 0x10
#define ATTR_FILE 0x20

typedef struct XNode {
    char name[256];
    bool dir;
    uint64_t size;
    int64_t mtime;
    struct XNode *kids;
    int nkids, cap;
    char *src;          /* the file's path on the host */
    uint32_t sector;
} XNode;

struct XisoTree {
    XNode root;
    uint64_t sig, data_bytes;
    char err[160];
    bool failed;
};

typedef struct {
    XNode *dir;
    XNode **kids;       /* sorted */
    int n, rootidx;
    int *left, *right;
    uint32_t *offset;   /* bytes from the table's start */
    uint32_t bytes;     /* whole sectors */
} Table;

static int cmp_names(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z') {
            x -= 32;
        }
        if (y >= 'a' && y <= 'z') {
            y -= 32;
        }
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (!x) {
            return 0;
        }
    }
}

static int cmp_nodes(const void *a, const void *b)
{
    return cmp_names((*(XNode *const *)a)->name, (*(XNode *const *)b)->name);
}

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0x100000001B3ull;
}

/* ------------------------------------------------------------ reading the tree */

typedef struct {
    XisoTree *t;
    XNode *dir;
    const char *path;
} Walk;

static void read_dir(XisoTree *t, XNode *dir, const char *path);

static SDL_EnumerationResult SDLCALL on_entry(void *ud, const char *dirname, const char *fname)
{
    Walk *w = ud;
    if (w->t->failed) {
        return SDL_ENUM_FAILURE;
    }
    char full[2048];
    snprintf(full, sizeof(full), "%s%s%s", dirname, dirname[0] && dirname[strlen(dirname) - 1] != '/' &&
                                                      dirname[strlen(dirname) - 1] != '\\' ? "/" : "", fname);
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(full, &pi)) {
        return SDL_ENUM_CONTINUE;
    }
    if (strlen(fname) > 255) {
        snprintf(w->t->err, sizeof(w->t->err), "\"%.60s...\": the name is too long for a disc image", fname);
        w->t->failed = true;
        return SDL_ENUM_FAILURE;
    }
    XNode *d = w->dir;
    if (d->nkids == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->kids = realloc(d->kids, (size_t)d->cap * sizeof(XNode));
    }
    XNode *n = &d->kids[d->nkids++];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", fname);
    n->mtime = pi.modify_time;
    if (pi.type == SDL_PATHTYPE_DIRECTORY) {
        n->dir = true;
        /* the children's array may move while d's grows: the walk is below d, fine */
        read_dir(w->t, n, full);
    } else if (pi.type == SDL_PATHTYPE_FILE) {
        if (pi.size > 0xFFFFFFFFull) {
            snprintf(w->t->err, sizeof(w->t->err), "\"%.60s\" is larger than 4 GB", fname);
            w->t->failed = true;
            return SDL_ENUM_FAILURE;
        }
        n->size = pi.size;
        n->src = strdup(full);
        w->t->data_bytes += pi.size;
    } else {
        d->nkids--;     /* neither (a device, a link): left out */
        return SDL_ENUM_CONTINUE;
    }
    w->t->sig = mix(w->t->sig, (uint64_t)n->size);
    w->t->sig = mix(w->t->sig, (uint64_t)n->mtime);
    for (const char *c = fname; *c; c++) {
        w->t->sig = mix(w->t->sig, (unsigned char)*c);
    }
    return SDL_ENUM_CONTINUE;
}

static void read_dir(XisoTree *t, XNode *dir, const char *path)
{
    Walk w = {t, dir, path};
    SDL_EnumerateDirectory(path, on_entry, &w);
}

XisoTree *xiso_tree_open(const char *dir, char *err, size_t errn)
{
    XisoTree *t = calloc(1, sizeof(*t));
    t->root.dir = true;
    read_dir(t, &t->root, dir);
    if (!t->failed && t->root.nkids == 0) {
        snprintf(t->err, sizeof(t->err), "the folder is empty");
        t->failed = true;
    }
    if (t->failed) {
        snprintf(err, errn, "%s", t->err);
        xiso_tree_free(t);
        return NULL;
    }
    return t;
}

uint64_t xiso_tree_signature(const XisoTree *t)
{
    return t->sig;
}

uint64_t xiso_tree_data_bytes(const XisoTree *t)
{
    return t->data_bytes;
}

static void free_node(XNode *n)
{
    for (int i = 0; i < n->nkids; i++) {
        free_node(&n->kids[i]);
    }
    free(n->kids);
    free(n->src);
}

void xiso_tree_free(XisoTree *t)
{
    if (t) {
        free_node(&t->root);
        free(t);
    }
}

/* ------------------------------------------------------------ the layout */

static uint32_t sectors(uint64_t bytes)
{
    return (uint32_t)((bytes + SECTOR - 1) / SECTOR);
}

static int build_tree(Table *t, int lo, int hi)
{
    if (lo > hi) {
        return -1;
    }
    const int mid = (lo + hi) / 2;
    t->left[mid] = build_tree(t, lo, mid - 1);
    t->right[mid] = build_tree(t, mid + 1, hi);
    return mid;
}

/* pre-order, so the tree's root comes first at offset 0 */
static void place(Table *t, int i, uint32_t *pos)
{
    if (i < 0) {
        return;
    }
    const size_t sz = (14 + strlen(t->kids[i]->name) + 3) & ~(size_t)3;
    if (*pos % SECTOR + sz > SECTOR) {
        *pos = (*pos / SECTOR + 1) * SECTOR;
    }
    t->offset[i] = *pos;
    *pos += (uint32_t)sz;
    place(t, t->left[i], pos);
    place(t, t->right[i], pos);
}

typedef struct {
    Table *v;
    int n, cap;
} Tables;

static bool prepare(XNode *dir, Tables *ts, char *err, size_t errn)
{
    if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 16;
        ts->v = realloc(ts->v, (size_t)ts->cap * sizeof(Table));
    }
    const int me = ts->n++;
    Table t;
    memset(&t, 0, sizeof(t));
    t.dir = dir;
    t.n = dir->nkids;
    t.kids = malloc(sizeof(XNode *) * (size_t)(t.n + 1));
    for (int i = 0; i < t.n; i++) {
        t.kids[i] = &dir->kids[i];
    }
    qsort(t.kids, (size_t)t.n, sizeof(XNode *), cmp_nodes);
    for (int i = 1; i < t.n; i++) {
        if (!cmp_names(t.kids[i - 1]->name, t.kids[i]->name)) {
            snprintf(err, errn, "\"%.80s\" appears twice (names ignore case)", t.kids[i]->name);
            free(t.kids);
            ts->v[me] = t;
            return false;
        }
    }
    t.left = malloc(sizeof(int) * (size_t)(t.n + 1));
    t.right = malloc(sizeof(int) * (size_t)(t.n + 1));
    t.offset = calloc((size_t)t.n + 1, sizeof(uint32_t));
    for (int i = 0; i < t.n; i++) {
        t.left[i] = t.right[i] = -1;
    }
    uint32_t pos = 0;
    t.rootidx = -1;
    if (t.n) {
        t.rootidx = build_tree(&t, 0, t.n - 1);
        place(&t, t.rootidx, &pos);
    }
    if (pos / 4 > 0xFFFF) {
        snprintf(err, errn, "too many files in one folder for a disc image");
        ts->v[me] = t;
        return false;
    }
    t.bytes = (pos ? sectors(pos) : 1u) * SECTOR;
    ts->v[me] = t;
    for (int i = 0; i < dir->nkids; i++) {
        if (dir->kids[i].dir && !prepare(&dir->kids[i], ts, err, errn)) {
            return false;
        }
    }
    return true;
}

static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
}

typedef struct {
    XNode **v;
    int n, cap;
} FileList;

static void collect(XNode *dir, FileList *fl)
{
    for (int i = 0; i < dir->nkids; i++) {
        if (!dir->kids[i].dir) {
            if (fl->n == fl->cap) {
                fl->cap = fl->cap ? fl->cap * 2 : 64;
                fl->v = realloc(fl->v, (size_t)fl->cap * sizeof(XNode *));
            }
            fl->v[fl->n++] = &dir->kids[i];
        }
    }
    for (int i = 0; i < dir->nkids; i++) {
        if (dir->kids[i].dir) {
            collect(&dir->kids[i], fl);
        }
    }
}

static bool write_at(SDL_IOStream *io, uint64_t off, const void *buf, size_t n)
{
    return SDL_SeekIO(io, (Sint64)off, SDL_IO_SEEK_SET) >= 0 && SDL_WriteIO(io, buf, n) == n;
}

bool xiso_tree_write(XisoTree *t, const char *path, SDL_AtomicInt *pct, char *err, size_t errn)
{
    Tables ts = {0};
    FileList fl = {0};
    bool ok = false;
    uint8_t *buf = NULL;
    SDL_IOStream *io = NULL;
    if (pct) {
        SDL_SetAtomicInt(pct, 0);
    }
    t->root.dir = true;
    if (!prepare(&t->root, &ts, err, errn)) {
        goto out;
    }
    /* the directory tables first, right after the volume descriptor, then the files */
    uint32_t sector = VOLUME_SECTOR + 1;
    for (int i = 0; i < ts.n; i++) {
        ts.v[i].dir->sector = sector;
        ts.v[i].dir->size = ts.v[i].bytes;
        sector += ts.v[i].bytes / SECTOR;
    }
    collect(&t->root, &fl);
    for (int i = 0; i < fl.n; i++) {
        fl.v[i]->sector = sector;
        sector += sectors(fl.v[i]->size);
    }
    const uint64_t end = (uint64_t)sector * SECTOR;
    const uint64_t image = (end + IMAGE_MODULUS - 1) / IMAGE_MODULUS * IMAGE_MODULUS;

    io = SDL_IOFromFile(path, "wb");
    if (!io) {
        snprintf(err, errn, "can't write %s", path);
        goto out;
    }
    /* the volume descriptor */
    uint8_t vd[SECTOR];
    memset(vd, 0, sizeof(vd));
    memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
    put32(vd + 20, t->root.sector);
    put32(vd + 24, (uint32_t)t->root.size);
    memcpy(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    ok = write_at(io, (uint64_t)VOLUME_SECTOR * SECTOR, vd, sizeof(vd));
    for (int i = 0; i < ts.n && ok; i++) {
        Table *tb = &ts.v[i];
        uint8_t *b = malloc(tb->bytes);
        memset(b, 0xFF, tb->bytes);
        for (int k = 0; k < tb->n; k++) {
            const XNode *kid = tb->kids[k];
            uint8_t *e = b + tb->offset[k];
            put16(e, tb->left[k] < 0 ? 0 : (uint16_t)(tb->offset[tb->left[k]] / 4));
            put16(e + 2, tb->right[k] < 0 ? 0 : (uint16_t)(tb->offset[tb->right[k]] / 4));
            put32(e + 4, kid->sector);
            put32(e + 8, (uint32_t)kid->size);
            e[12] = kid->dir ? ATTR_DIR : ATTR_FILE;
            e[13] = (uint8_t)strlen(kid->name);
            memcpy(e + 14, kid->name, strlen(kid->name));
        }
        ok = write_at(io, (uint64_t)tb->dir->sector * SECTOR, b, tb->bytes);
        free(b);
    }
    if (!ok) {
        snprintf(err, errn, "the image couldn't be written (disk full?)");
        goto out;
    }
    /* the files' data */
    const size_t BUF = 1u << 20;
    buf = malloc(BUF);
    uint64_t done = 0;
    for (int i = 0; i < fl.n && ok; i++) {
        XNode *f = fl.v[i];
        if (!f->size) {
            continue;
        }
        SDL_IOStream *in = SDL_IOFromFile(f->src, "rb");
        if (!in) {
            snprintf(err, errn, "can't read %.100s", f->src);
            ok = false;
            break;
        }
        if (SDL_SeekIO(io, (Sint64)((uint64_t)f->sector * SECTOR), SDL_IO_SEEK_SET) < 0) {
            ok = false;
        }
        uint64_t left = f->size;
        while (left && ok) {
            const size_t want = left < BUF ? (size_t)left : BUF;
            const size_t got = SDL_ReadIO(in, buf, want);
            if (got != want || SDL_WriteIO(io, buf, got) != got) {
                snprintf(err, errn, "%.100s couldn't be copied (disk full?)", f->src);
                ok = false;
                break;
            }
            left -= got;
            done += got;
            if (pct && t->data_bytes) {
                SDL_SetAtomicInt(pct, (int)(done * 100 / t->data_bytes));
            }
        }
        SDL_CloseIO(in);
    }
    /* padded out to the image's size */
    if (ok) {
        const uint8_t z = 0;
        ok = write_at(io, image - 1, &z, 1);
        if (!ok) {
            snprintf(err, errn, "the image couldn't be finished (disk full?)");
        }
    }
out:
    if (io) {
        ok = SDL_CloseIO(io) && ok;
    }
    if (!ok) {
        remove(path);
    }
    for (int i = 0; i < ts.n; i++) {
        free(ts.v[i].kids);
        free(ts.v[i].left);
        free(ts.v[i].right);
        free(ts.v[i].offset);
    }
    free(ts.v);
    free(fl.v);
    free(buf);
    if (ok && pct) {
        SDL_SetAtomicInt(pct, 100);
    }
    return ok;
}
