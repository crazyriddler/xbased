/*
 * xbased native core — CCI disc images (cci_image.h).
 *
 * A part's layout (little-endian):
 *   header, 32 bytes: "CCIM", u32 header size (32), u64 the bytes it holds,
 *     u64 the index's offset, u32 block size (2048), u8 version (1),
 *     u8 the index's shift (2), u16 reserved
 *   the blocks, one a sector, in order
 *   the index: a u32 a block and one more (where the blocks end): the
 *     block's offset shifted right by the index's shift, the top bit set
 *     for an LZ4 block (a byte, the padding's length, then the LZ4 block,
 *     then the padding); clear, the sector's 2048 bytes as they are.
 *
 * Nothing is kept in memory but the parts' headers: a read takes the index
 * entries of its blocks and the blocks themselves, each in one pread (the
 * blocks are in order), and decodes them into the reader's buffer. No lock
 * between reads; a detach waits for the reads under way.
 */
#include "native/core/cci_image.h"
#include "native/core/nc.h"
#include "native/core/xdvdfs.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#define MAX_CCIS 8
#define MAX_PARTS 9                 /* Name.1.cci to Name.9.cci */
#define SECTOR NC_XDVDFS_SECTOR
#define LZ4_FLAG 0x80000000u
#define CHUNK 32u                   /* blocks a pread at most */
#define MAX_SHIFT 4                 /* the index's shift: XboxToolkit's 2 */
/* A block's bytes at most: a sector's as they are, or an LZ4 block worth
 * keeping (shorter than the sector), its padding byte and padding. */
#define MAX_STORED (SECTOR + 1u + (1u << MAX_SHIFT))
/* Where a split image's parts are cut (XboxToolkit's, 16 MiB short of FATX's
 * 4 GiB file limit): each part but the last ends its blocks at the first
 * block's end past it. An image in one file ends them anywhere else (of
 * 850 measured, none closer than 100 KB). A test builds with its own. */
#ifndef NC_CCI_SPLIT_AT
#define NC_CCI_SPLIT_AT 0xFF000000ull
#endif

typedef struct {
    int fd;                         /* the first part's is the caller's */
    uint64_t first;                 /* its first block in the image */
    uint64_t blocks;
    uint64_t bytes;                 /* the image's it holds */
    uint64_t index_off;
    uint64_t file_size;
    unsigned shift;
    bool full;                      /* filled to the cut: a split image's part, another after it */
} Part;

typedef struct {
    int fd;                         /* the key (-1: free) */
    Part part[MAX_PARTS];
    unsigned nparts;
    uint64_t size;                  /* the image's bytes */
    pthread_rwlock_t rw;            /* reads share it, a detach takes it */
} Cci;

static Cci g_cci[MAX_CCIS] = {{-1}, {-1}, {-1}, {-1}, {-1}, {-1}, {-1}, {-1}};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static Cci *find(int fd)
{
    for (int i = 0; i < MAX_CCIS; i++) {
        if (g_cci[i].fd == fd && fd >= 0) {
            return &g_cci[i];
        }
    }
    return NULL;
}

static bool pread_full(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    for (unsigned tries = 0; len;) {
        const ssize_t n = pread(fd, p, len, (off_t)off);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            off += (uint64_t)n;
            tries = 0;
        } else if (n == 0 || ++tries > 4) {
            return false;           /* past the file's end, or failing */
        }
    }
    return true;
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)nc_rd32(p) | (uint64_t)nc_rd32(p + 4) << 32;
}

/* ------------------------------------------------------------ LZ4 */

/* An LZ4 block (the raw format, no frame) of sn bytes decoded into exactly
 * dn. False: a block that doesn't decode to dn bytes or reaches outside
 * either buffer (a damaged or crafted image). */
static bool lz4_block(const uint8_t *src, size_t sn, uint8_t *dst, size_t dn)
{
    const uint8_t *s = src, *const se = src + sn;
    uint8_t *d = dst, *const de = dst + dn;
    while (s < se) {
        const unsigned token = *s++;
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (s >= se) {
                    return false;
                }
                b = *s++;
                lit += b;
            } while (b == 255);
        }
        if (lit > (size_t)(se - s) || lit > (size_t)(de - d)) {
            return false;
        }
        memcpy(d, s, lit);
        d += lit;
        s += lit;
        if (s == se) {
            break;                  /* the last sequence: literals alone */
        }
        if (se - s < 2) {
            return false;
        }
        const size_t back = (size_t)s[0] | (size_t)s[1] << 8;
        s += 2;
        if (!back || back > (size_t)(d - dst)) {
            return false;
        }
        size_t len = token & 15;
        if (len == 15) {
            unsigned b;
            do {
                if (s >= se) {
                    return false;
                }
                b = *s++;
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (len > (size_t)(de - d)) {
            return false;
        }
        const uint8_t *m = d - back;
        if (back >= len) {
            memcpy(d, m, len);
            d += len;
        } else {
            while (len--) {
                *d++ = *m++;        /* overlapping: a run repeated */
            }
        }
    }
    return d == de;
}

/* ------------------------------------------------------------ parts */

static bool part_open(Part *p, int fd)
{
    uint8_t h[32];
    struct stat st;
    if (!pread_full(fd, h, sizeof(h), 0) || memcmp(h, "CCIM", 4) || fstat(fd, &st)) {
        return false;
    }
    const uint64_t size = rd64(h + 8), index_off = rd64(h + 16);
    const unsigned shift = h[29];
    if (nc_rd32(h + 4) != 32 || nc_rd32(h + 24) != SECTOR || h[28] != 1 || shift > MAX_SHIFT || !size) {
        nc_log("xbased-native", "disc: CCI image of an unknown kind (header %u, block %u, version %u, shift %u)",
               nc_rd32(h + 4), nc_rd32(h + 24), h[28], shift);
        return false;
    }
    const uint64_t blocks = (size + SECTOR - 1) / SECTOR;
    if (index_off < 32 || index_off > (uint64_t)st.st_size || blocks > 0x3FFFFFFFu ||
        (blocks + 1) * 4 > (uint64_t)st.st_size - index_off) {
        nc_log("xbased-native", "disc: CCI image whose index isn't in its file (%llu bytes, index at +0x%llx of %llu)",
               (unsigned long long)size, (unsigned long long)index_off, (unsigned long long)st.st_size);
        return false;
    }
    p->full = index_off >= NC_CCI_SPLIT_AT && index_off - NC_CCI_SPLIT_AT < MAX_STORED;
    if (p->full && size % SECTOR) {
        /* each part but the last ends on a sector, the next starting the next */
        nc_log("xbased-native", "disc: a split CCI image's part of %llu bytes, not whole sectors",
               (unsigned long long)size);
        return false;
    }
    p->fd = fd;
    p->blocks = blocks;
    p->bytes = size;
    p->index_off = index_off;
    p->file_size = (uint64_t)st.st_size;
    p->shift = shift;
    return true;
}

/* n blocks from the part's block b (n <= CHUNK), each SECTOR bytes at out;
 * buf: CHUNK * MAX_STORED bytes of room for what they're stored as */
static bool part_blocks(const Part *p, uint64_t b, unsigned n, uint8_t *out, uint8_t *buf)
{
    uint8_t raw[(CHUNK + 1) * 4];
    if (!pread_full(p->fd, raw, (n + 1) * 4, p->index_off + b * 4)) {
        return false;
    }
    uint32_t e[CHUNK + 1];
    uint64_t at[CHUNK + 1];
    for (unsigned i = 0; i <= n; i++) {
        e[i] = nc_rd32(raw + i * 4);
        at[i] = (uint64_t)(e[i] & ~LZ4_FLAG) << p->shift;
        if (at[i] < 32 || at[i] > p->index_off || (i && (at[i] < at[i - 1] || at[i] - at[i - 1] > MAX_STORED))) {
            nc_log("xbased-native", "disc: CCI block %llu's index entry is out of place",
                   (unsigned long long)(p->first + b + i));
            return false;
        }
    }
    if (!pread_full(p->fd, buf, (size_t)(at[n] - at[0]), at[0])) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *s = buf + (at[i] - at[0]);
        const size_t len = (size_t)(at[i + 1] - at[i]);
        uint8_t *d = out + (size_t)i * SECTOR;
        bool ok;
        if (e[i] & LZ4_FLAG) {
            ok = len >= 1 && (size_t)s[0] + 1 <= len && lz4_block(s + 1, len - 1 - s[0], d, SECTOR);
        } else {
            ok = len >= SECTOR;
            if (ok) {
                memcpy(d, s, SECTOR);
            }
        }
        if (!ok) {
            nc_log("xbased-native", "disc: CCI block %llu doesn't decode", (unsigned long long)(p->first + b + i));
            return false;
        }
    }
    return true;
}

/* The path of an open file, from its descriptor */
static bool fd_path(int fd, char *out, size_t cap)
{
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    char p[MAX_PATH * 4];
    const DWORD n = h != INVALID_HANDLE_VALUE ? GetFinalPathNameByHandleA(h, p, sizeof(p), 0) : 0;
    if (!n || n >= sizeof(p)) {
        return false;
    }
    const char *s = p;
    if (!strncmp(s, "\\\\?\\UNC\\", 8)) {
        snprintf(out, cap, "\\\\%s", s + 8);   /* \\server\share\... */
        return true;
    }
    if (!strncmp(s, "\\\\?\\", 4)) {
        s += 4;
    }
    snprintf(out, cap, "%s", s);
    return true;
#else
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(link, out, cap - 1);
    if (n <= 0) {
        return false;
    }
    out[n] = 0;
    return true;
#endif
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            b = p + 1;
        }
    }
    return b;
}

/* name's part number (Name.N.cci: N, 1 to 99), else 0 */
static unsigned part_number(const char *name)
{
    const size_t n = strlen(name);
    if (n < 7 || strcasecmp(name + n - 4, ".cci")) {
        return 0;
    }
    size_t i = n - 4;
    unsigned v = 0, digits = 0;
    while (i && isdigit((unsigned char)name[i - 1]) && digits < 2) {
        i--;
        digits++;
    }
    if (!digits || !i || name[i - 1] != '.' || name[i] == '0') {
        return 0;
    }
    for (size_t k = i; k < n - 4; k++) {
        v = v * 10 + (unsigned)(name[k] - '0');
    }
    return v;
}

static int open_part(const char *path)
{
#ifdef O_BINARY
    return open(path, O_RDONLY | O_BINARY | O_CLOEXEC);
#else
    return open(path, O_RDONLY | O_CLOEXEC);
#endif
}

bool nc_cci_part_path(const char *path, unsigned k, char *out, size_t cap)
{
    const unsigned n0 = part_number(base_name(path));
    if (!n0 || k < 1 || k > MAX_PARTS) {
        return false;
    }
    /* "Name." + k + the extension as it's spelled */
    const size_t n = strlen(path), digits = n0 >= 10 ? 2 : 1;
    const int w = snprintf(out, cap, "%.*s%u%s", (int)(n - 4 - digits), path, k, path + n - 4);
    return w > 0 && (size_t)w < cap;
}

bool nc_cci_later_part(const char *path)
{
    char first[1040];
    if (part_number(base_name(path)) < 2 || !nc_cci_part_path(path, 1, first, sizeof(first))) {
        return false;
    }
    /* of a split image if Name.1.cci is one's first part: "Game v1.2.cci"
     * beside an image of its own named "Game v1.1.cci" is a game too */
    const int fd = open_part(first);
    Part p;
    const bool later = fd >= 0 && part_open(&p, fd) && p.full;
    if (fd >= 0) {
        close(fd);
    }
    return later;
}

/* ------------------------------------------------------------ the image */

bool nc_cci_attach(int fd)
{
    char magic[4];
    if (fd < 0 || pread(fd, magic, 4, 0) != 4 || memcmp(magic, "CCIM", 4)) {
        return false;
    }
    if (nc_cci_is(fd)) {
        return true;
    }
    /* the parts read without the lock (a slow drive's holds up no other
     * image's reads): a part filled to the cut, the next beside it
     * (Name.2.cci after Name.1.cci), and so on to one that isn't. A part
     * missing fails the image: past it the title would read zeros. */
    Part part[MAX_PARTS];
    bool ok = part_open(&part[0], fd);
    unsigned nparts = ok ? 1 : 0;
    char path[1024];
    if (ok && part[0].full && (!fd_path(fd, path, sizeof(path)) || part_number(base_name(path)) != 1)) {
        nc_log("xbased-native", "disc: CCI image of a split one's first part, not named Name.1.cci: no other parts");
        ok = false;
    }
    while (ok && part[nparts - 1].full) {
        char next[1040];
        int pfd = -1;
        if (nparts < MAX_PARTS && nc_cci_part_path(path, nparts + 1, next, sizeof(next))) {
            pfd = open_part(next);
        }
        if (pfd < 0 || !part_open(&part[nparts], pfd)) {
            nc_log("xbased-native", "disc: a split CCI image's part %u missing or unreadable", nparts + 1);
            if (pfd >= 0) {
                close(pfd);
            }
            ok = false;
            break;
        }
        nparts++;
    }
    uint64_t size = 0;
    for (unsigned k = 0; k < nparts; k++) {
        part[k].first = size / SECTOR;
        size += part[k].bytes;
    }
    pthread_mutex_lock(&g_mu);
    Cci *c = ok ? find(fd) : NULL;
    const bool there = c != NULL;           /* attached by another thread meanwhile */
    for (int i = 0; i < MAX_CCIS && ok && !c; i++) {
        c = g_cci[i].fd < 0 ? &g_cci[i] : NULL;
    }
    if (!ok || there || !c) {
        pthread_mutex_unlock(&g_mu);
        for (unsigned k = 1; k < nparts; k++) {
            close(part[k].fd);              /* the first part's descriptor is its owner's */
        }
        if (ok && !there) {
            nc_log("xbased-native", "disc: too many CCI images open");
        }
        return there;
    }
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    memcpy(c->part, part, sizeof(part[0]) * nparts);
    c->nparts = nparts;
    c->size = size;
    pthread_rwlock_init(&c->rw, NULL);
    pthread_mutex_unlock(&g_mu);
    nc_log("xbased-native", "disc: CCI image, %llu MiB in %llu MiB (%u part%s)", (unsigned long long)(size >> 20),
           (unsigned long long)(nc_cci_file_size(fd) >> 20), nparts, nparts > 1 ? "s" : "");
    return true;
}

void nc_cci_detach(int fd)
{
    pthread_mutex_lock(&g_mu);
    Cci *c = find(fd);
    if (c) {
        pthread_rwlock_wrlock(&c->rw);      /* the reads under way done */
        for (unsigned k = 1; k < c->nparts; k++) {
            close(c->part[k].fd);           /* the first part's descriptor is its owner's */
        }
        c->nparts = 0;
        pthread_rwlock_unlock(&c->rw);
        pthread_rwlock_destroy(&c->rw);
        c->fd = -1;
    }
    pthread_mutex_unlock(&g_mu);
}

bool nc_cci_is(int fd)
{
    pthread_mutex_lock(&g_mu);
    const bool r = find(fd) != NULL;
    pthread_mutex_unlock(&g_mu);
    return r;
}

uint64_t nc_cci_file_size(int fd)
{
    pthread_mutex_lock(&g_mu);
    const Cci *c = find(fd);
    uint64_t n = 0;
    for (unsigned k = 0; c && k < c->nparts; k++) {
        n += c->part[k].file_size;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

bool nc_cci_read(int fd, void *buf, size_t len, uint64_t off)
{
    pthread_mutex_lock(&g_mu);
    Cci *c = find(fd);
    if (c) {
        pthread_rwlock_rdlock(&c->rw);
    }
    pthread_mutex_unlock(&g_mu);
    if (!c) {
        return false;
    }
    uint8_t *p = buf, *stored = NULL, one[SECTOR];
    bool ok = true;
    while (len) {
        if (off >= c->size) {
            memset(p, 0, len);              /* past the image: zeros, as for a trimmed one */
            break;
        }
        const uint64_t blk = off / SECTOR;
        const Part *pt = &c->part[0];
        for (unsigned k = 1; k < c->nparts && blk >= c->part[k].first; k++) {
            pt = &c->part[k];
        }
        if (!stored && !(stored = malloc(CHUNK * MAX_STORED))) {
            ok = false;
            break;
        }
        const uint32_t in = (uint32_t)(off % SECTOR);
        /* whole sectors straight into the reader's buffer, up to the part's
         * end; a piece of one through a sector of its own */
        uint64_t n = len / SECTOR, room = (c->size - off) / SECTOR;
        n = n < CHUNK ? n : CHUNK;
        n = n < pt->first + pt->blocks - blk ? n : pt->first + pt->blocks - blk;
        n = n < room ? n : room;
        if (!in && n) {
            if (!(ok = part_blocks(pt, blk - pt->first, (unsigned)n, p, stored))) {
                break;
            }
            p += n * SECTOR;
            len -= (size_t)(n * SECTOR);
            off += n * SECTOR;
            continue;
        }
        if (!(ok = part_blocks(pt, blk - pt->first, 1, one, stored))) {
            break;
        }
        uint64_t take = SECTOR - in;
        take = take < len ? take : len;
        take = take < c->size - off ? take : c->size - off;
        memcpy(p, one + in, (size_t)take);
        p += take;
        len -= (size_t)take;
        off += take;
    }
    pthread_rwlock_unlock(&c->rw);
    free(stored);
    return ok;
}
