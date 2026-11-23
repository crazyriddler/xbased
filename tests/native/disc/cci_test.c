/*
 * Host test of CCI images (native/core/cci_image.c) through the disc reader
 * itself (native/core/xdvdfs.c): an image made by the test (a volume
 * descriptor, a root table, a default.xbe, then sectors of zeros, of runs
 * and of noise, so LZ4 blocks with overlapping matches and blocks kept as
 * they are), written as a CCI by an LZ4 packer of the test's own, whole and
 * split in parts cut as XboxToolkit cuts them (at a cut of the test's, 32
 * KiB, for FATX's 4 GiB). Every read must give the image's bytes, whole
 * sectors and pieces across sector, chunk and part ends; past its end,
 * zeros. A split image with a part missing, or its first part named as an
 * image of its own, is none; two images of their own named like parts
 * ("Game v1.1.cci", "Game v1.2.cci") are two. A damaged image (a header of
 * another kind, an index entry out of place, an LZ4 block that reaches back
 * before its sector) must fail to attach or to read, and never read or
 * write outside its buffers.
 *
 *   clang -O1 -g -fsanitize=address -DNC_CCI_SPLIT_AT=0x8000 -I<repo> tests/native/disc/cci_test.c \
 *       native/core/cci_image.c native/core/xdvdfs.c -o cci_test -lpthread
 */
#include "native/core/xdvdfs.h"
#include "native/core/cci_image.h"
#include "native/core/chd_image.h"
#include "native/core/fold_image.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* what the two link to, not used here */
void nc_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}
bool nc_chd_attach(int fd) { (void)fd; return false; }
void nc_chd_detach(int fd) { (void)fd; }
bool nc_chd_is(int fd) { (void)fd; return false; }
bool nc_chd_read(int fd, void *buf, size_t len, uint64_t off) { (void)fd; (void)buf; (void)len; (void)off; return false; }
bool nc_nbd_attach(int fd) { (void)fd; return false; }
void nc_nbd_detach(int fd) { (void)fd; }
bool nc_nbd_is(int fd) { (void)fd; return false; }
bool nc_nbd_read(int fd, void *buf, size_t len, uint64_t off) { (void)fd; (void)buf; (void)len; (void)off; return false; }
bool nc_fold_attach(int fd) { (void)fd; return false; }
void nc_fold_detach(int fd) { (void)fd; }
bool nc_fold_is(int fd) { (void)fd; return false; }
bool nc_fold_read(int fd, void *buf, size_t len, uint64_t off) { (void)fd; (void)buf; (void)len; (void)off; return false; }

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define SECTOR 2048u
#define SECTORS 200u                /* more than a read's chunk (32) several times over */
#define XBE_SIZE 5000u
#define CUT 0x8000u                 /* NC_CCI_SPLIT_AT, as the test builds cci_image.c */

static char g_root[512];
static uint8_t g_img[SECTORS * SECTOR];

static void w16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t v)
{
    w16(p, v & 0xFFFF);
    w16(p + 2, v >> 16);
}

static void w64(uint8_t *p, uint64_t v)
{
    w32(p, (uint32_t)v);
    w32(p + 4, (uint32_t)(v >> 32));
}

/* The image: sector 32 the volume descriptor, 33 the root table (one
 * entry, default.xbe at 34), then by the sector's number zeros, a short
 * pattern repeated (matches that overlap what they copy), text-like runs,
 * or noise (no LZ4 block shorter than the sector) */
static void make_image(void)
{
    uint32_t seed = 12345;
    for (uint32_t s = 0; s < SECTORS; s++) {
        uint8_t *p = g_img + (size_t)s * SECTOR;
        for (uint32_t i = 0; i < SECTOR; i++) {
            switch (s % 4) {
            case 0:
                p[i] = 0;
                break;
            case 1:
                p[i] = (uint8_t)("abcdefg"[i % 7] + s);
                break;
            case 2:
                p[i] = (uint8_t)((i / 61) * 3 + s);
                break;
            default:
                seed = seed * 1103515245u + 12345u;
                p[i] = (uint8_t)(seed >> 16);
                break;
            }
        }
    }
    uint8_t *vd = g_img + 32 * SECTOR;
    memset(vd, 0, SECTOR);
    memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
    w32(vd + 20, 33);
    w32(vd + 24, SECTOR);
    memcpy(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    uint8_t *t = g_img + 33 * SECTOR;
    memset(t, 0xFF, SECTOR);
    w16(t, 0);
    w16(t + 2, 0);
    w32(t + 4, 34);
    w32(t + 8, XBE_SIZE);
    t[12] = 0x20;
    t[13] = 11;
    memcpy(t + 14, "default.xbe", 11);
    memcpy(g_img + 34 * SECTOR, "XBEH", 4);
}

/* An LZ4 block of src's n bytes (greedy, a match at most 65535 back, the
 * format's end kept: the last 5 bytes literals, no match starting in the
 * last 12) at dst: its length */
static size_t lz4_pack(const uint8_t *src, size_t n, uint8_t *dst)
{
    int32_t table[4096];
    for (unsigned i = 0; i < 4096; i++) {
        table[i] = -1;
    }
    uint8_t *d = dst;
    size_t i = 0, anchor = 0;
    while (n >= 13 && i + 12 <= n) {
        uint32_t v;
        memcpy(&v, src + i, 4);
        const unsigned h = (v * 2654435761u) >> 20;
        const int32_t cand = table[h];
        table[h] = (int32_t)i;
        if (cand < 0 || i - (size_t)cand > 65535 || memcmp(src + cand, src + i, 4)) {
            i++;
            continue;
        }
        size_t len = 4;
        while (i + len < n - 5 && src[cand + len] == src[i + len]) {
            len++;
        }
        const size_t lit = i - anchor, extra = len - 4, back = i - (size_t)cand;
        uint8_t *token = d++;
        *token = (uint8_t)((lit < 15 ? lit : 15) << 4 | (extra < 15 ? extra : 15));
        if (lit >= 15) {
            size_t r = lit - 15;
            for (; r >= 255; r -= 255) {
                *d++ = 255;
            }
            *d++ = (uint8_t)r;
        }
        memcpy(d, src + anchor, lit);
        d += lit;
        *d++ = (uint8_t)back;
        *d++ = (uint8_t)(back >> 8);
        if (extra >= 15) {
            size_t r = extra - 15;
            for (; r >= 255; r -= 255) {
                *d++ = 255;
            }
            *d++ = (uint8_t)r;
        }
        i += len;
        anchor = i;
    }
    const size_t lit = n - anchor;
    *d++ = (uint8_t)((lit < 15 ? lit : 15) << 4);
    if (lit >= 15) {
        size_t r = lit - 15;
        for (; r >= 255; r -= 255) {
            *d++ = 255;
        }
        *d++ = (uint8_t)r;
    }
    memcpy(d, src + anchor, lit);
    d += lit;
    return (size_t)(d - dst);
}

/* The image's sectors from first, n at most, as a CCI at path, as
 * XboxToolkit writes one: an LZ4 block where it's shorter than the sector
 * (a byte, the padding's length, then the block and the padding to
 * 1 << shift), else the sector as it is; with cut (0: none), a split
 * image's part, its blocks ending at the first block's end past it.
 * took: the sectors written; compressed: how many as LZ4 blocks. */
static bool put_cci(const char *path, uint32_t first, uint32_t n, unsigned shift, uint64_t cut, uint32_t *took,
                    unsigned *compressed)
{
    FILE *f = fopen(path, "wb");
    uint32_t *index = malloc((n + 1) * sizeof(uint32_t));
    uint8_t packed[SECTOR + SECTOR / 255 + 16], zeros[16] = { 0 }, h[32] = { 0 };
    if (!f || !index) {
        if (f) {
            fclose(f);
        }
        free(index);
        return false;
    }
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
    uint64_t pos = sizeof(h);
    *compressed = 0;
    uint32_t i = 0;
    for (; i < n && ok && !(cut && pos >= cut); i++) {
        const uint8_t *s = g_img + (size_t)(first + i) * SECTOR;
        const size_t len = lz4_pack(s, SECTOR, packed);
        if (len + 1 < SECTOR) {
            const uint64_t end = pos + 1 + len, align = 1ull << shift;
            const uint8_t pad = (uint8_t)((align - end % align) % align);
            index[i] = (uint32_t)(pos >> shift) | 0x80000000u;
            ok = fwrite(&pad, 1, 1, f) == 1 && fwrite(packed, 1, len, f) == len && fwrite(zeros, 1, pad, f) == pad;
            pos = end + pad;
            ++*compressed;
        } else {
            index[i] = (uint32_t)(pos >> shift);
            ok = fwrite(s, 1, SECTOR, f) == SECTOR;
            pos += SECTOR;
        }
    }
    n = *took = i;
    index[n] = (uint32_t)(pos >> shift);
    for (i = 0; i <= n && ok; i++) {
        uint8_t e[4];
        w32(e, index[i]);
        ok = fwrite(e, 1, 4, f) == 4;
    }
    memcpy(h, "CCIM", 4);
    w32(h + 4, 32);
    w64(h + 8, (uint64_t)n * SECTOR);
    w64(h + 16, pos);
    w32(h + 24, SECTOR);
    h[28] = 1;
    h[29] = (uint8_t)shift;
    ok = ok && fseek(f, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof(h), f) == sizeof(h);
    free(index);
    return fclose(f) == 0 && ok;
}

static void mkpath(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_root, name);
}

/* The image as a split CCI, stem.1.cci, stem.2.cci, ...: the parts' number */
static unsigned put_split(const char *stem)
{
    unsigned k = 0, packed;
    for (uint32_t first = 0, took = 0; first < SECTORS; first += took) {
        char name[128], p[1024];
        snprintf(name, sizeof(name), "%s.%u.cci", stem, ++k);
        mkpath(p, sizeof(p), name);
        if (!put_cci(p, first, SECTORS - first, 2, CUT, &took, &packed) || !took) {
            return 0;
        }
    }
    return k;
}

/* Bytes of the file at off, as they are on disk */
static bool patch(const char *path, uint64_t off, const void *bytes, size_t len)
{
    const int fd = open(path, O_RDWR | O_BINARY);
    const bool ok = fd >= 0 && pwrite(fd, bytes, len, (off_t)off) == (ssize_t)len;
    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

static bool peek(const char *path, uint64_t off, void *bytes, size_t len)
{
    const int fd = open(path, O_RDONLY | O_BINARY);
    const bool ok = fd >= 0 && pread(fd, bytes, len, (off_t)off) == (ssize_t)len;
    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

/* The image read back through fd: whole, in pieces at random places, its
 * default.xbe through the disc reader, and zeros past its end */
static void check_image(int fd, const char *what)
{
    uint8_t *got = malloc(sizeof(g_img));
    memset(got, 0xAA, sizeof(g_img));
    CHECK(nc_cci_read(fd, got, sizeof(g_img), 0));
    CHECK(!memcmp(got, g_img, sizeof(g_img)));
    uint32_t seed = 777;
    unsigned bad = 0;
    for (unsigned k = 0; k < 2000; k++) {
        seed = seed * 1103515245u + 12345u;
        const uint64_t off = (seed >> 4) % sizeof(g_img);
        seed = seed * 1103515245u + 12345u;
        /* a byte, a sector's piece, sectors across chunks */
        const size_t cap = k % 3 == 0 ? 3 : k % 3 == 1 ? SECTOR + 500 : 40 * SECTOR;
        size_t len = 1 + (seed >> 4) % cap;
        len = off + len > sizeof(g_img) ? sizeof(g_img) - off : len;
        if (!nc_cci_read(fd, got, len, off) || memcmp(got, g_img + off, len)) {
            bad++;
        }
    }
    CHECK(bad == 0);
    if (bad) {
        printf("        %s: %u reads wrong\n", what, bad);
    }
    /* up to the image's end and past it: the rest zeros */
    memset(got, 0xAA, 3 * SECTOR);
    CHECK(nc_cci_read(fd, got, 3 * SECTOR, sizeof(g_img) - SECTOR - 10));
    CHECK(!memcmp(got, g_img + sizeof(g_img) - SECTOR - 10, SECTOR + 10));
    bool zeros = true;
    for (unsigned i = SECTOR + 10; i < 3 * SECTOR; i++) {
        zeros = zeros && got[i] == 0;
    }
    CHECK(zeros);
    free(got);
}

static void check_disc(int fd)
{
    NcDisc d;
    NcDiscFile f;
    CHECK(nc_disc_open(&d, fd));
    CHECK(nc_cci_is(fd));
    uint8_t x[XBE_SIZE];
    CHECK(nc_disc_find(&d, "\\DEFAULT.XBE", &f) && f.size == XBE_SIZE && f.sector == 34);
    CHECK(nc_disc_read(&d, &f, 0, x, sizeof(x)) && !memcmp(x, g_img + 34 * SECTOR, sizeof(x)));
    nc_disc_close(&d);          /* closes fd */
}

int main(void)
{
    const char *tmp = getenv("TMP");
    snprintf(g_root, sizeof(g_root), "%s/cci_test_%d", tmp && tmp[0] ? tmp : ".", (int)getpid());
    mkdir(g_root, 0777);
    make_image();

    /* a split image's part names */
    char got[64];
    CHECK(nc_cci_part_path("Game (USA).1.cci", 2, got, sizeof(got)) && !strcmp(got, "Game (USA).2.cci"));
    CHECK(nc_cci_part_path("C:\\g\\Game.12.CCI", 3, got, sizeof(got)) && !strcmp(got, "C:\\g\\Game.3.CCI"));
    CHECK(nc_cci_part_path("/g/v1.1/Game.2.cci", 1, got, sizeof(got)) && !strcmp(got, "/g/v1.1/Game.1.cci"));
    CHECK(!nc_cci_part_path("Game.cci", 2, got, sizeof(got)));
    CHECK(!nc_cci_part_path("Game2.cci", 2, got, sizeof(got)));
    CHECK(!nc_cci_part_path("Game.02.cci", 2, got, sizeof(got)));
    CHECK(!nc_cci_part_path("Game.2.iso", 1, got, sizeof(got)));
    CHECK(!nc_cci_part_path(".2.cci", 1, got, sizeof(got)));
    CHECK(!nc_cci_part_path("Game.1.cci", 2, got, 8));                /* no room */

    /* one file, the index shifted as XboxToolkit's and not at all */
    char one[1024], one0[1024];
    unsigned packed = 0;
    uint32_t took;
    mkpath(one, sizeof(one), "game.cci");
    mkpath(one0, sizeof(one0), "game0.cci");
    CHECK(put_cci(one, 0, SECTORS, 2, 0, &took, &packed) && took == SECTORS);
    CHECK(packed > SECTORS / 2 && packed < SECTORS);    /* LZ4 blocks and sectors as they are */
    CHECK(put_cci(one0, 0, SECTORS, 0, 0, &took, &packed));
    for (int k = 0; k < 2; k++) {
        const int fd = open(k ? one0 : one, O_RDONLY | O_BINARY);
        CHECK(fd >= 0 && nc_cci_attach(fd) && nc_cci_is(fd));
        CHECK(nc_cci_attach(fd));                       /* twice: the one registration */
        check_image(fd, k ? "game0.cci" : "game.cci");
        struct stat st;
        CHECK(!fstat(fd, &st) && nc_cci_file_size(fd) == (uint64_t)st.st_size);
        nc_cci_detach(fd);
        CHECK(!nc_cci_is(fd));
        CHECK(nc_cci_file_size(fd) == 0);
        uint8_t b[16];
        CHECK(!nc_cci_read(fd, b, sizeof(b), 0));      /* not a CCI any more */
        close(fd);
    }
    check_disc(open(one, O_RDONLY | O_BINARY));
    CHECK(!nc_cci_later_part(one));

    /* split, opened by its first part; its later parts no games */
    const unsigned parts = put_split("split");
    CHECK(parts >= 3 && parts <= 8);
    char part[8][1024];
    for (unsigned k = 0; k < 8; k++) {
        char name[32];
        snprintf(name, sizeof(name), "split.%u.cci", k + 1);
        mkpath(part[k], sizeof(part[k]), name);
    }
    if (parts >= 3 && parts <= 8) {
        const int fd = open(part[0], O_RDONLY | O_BINARY);
        CHECK(fd >= 0 && nc_cci_attach(fd));
        check_image(fd, "split.1.cci");
        uint64_t all = 0;
        for (unsigned k = 0; k < parts && k < 8; k++) {
            struct stat st;
            CHECK(!stat(part[k], &st));
            all += (uint64_t)st.st_size;
        }
        CHECK(nc_cci_file_size(fd) == all);
        nc_cci_detach(fd);
        close(fd);
        check_disc(open(part[0], O_RDONLY | O_BINARY));
        CHECK(!nc_cci_later_part(part[0]));
        CHECK(nc_cci_later_part(part[1]));
        CHECK(nc_cci_later_part(part[2]));
        /* its last part gone: no image (past it, zeros the title would read) */
        char away[1024];
        mkpath(away, sizeof(away), "away.cci");
        CHECK(!rename(part[parts - 1], away));
        int afd = open(part[0], O_RDONLY | O_BINARY);
        CHECK(afd >= 0 && !nc_cci_attach(afd) && !nc_cci_is(afd));
        close(afd);
        CHECK(!rename(away, part[parts - 1]));
        /* its first part named as an image of its own: its parts unknown, no image */
        CHECK(!rename(part[0], away));
        afd = open(away, O_RDONLY | O_BINARY);
        CHECK(afd >= 0 && !nc_cci_attach(afd));
        close(afd);
        CHECK(!nc_cci_later_part(part[1]));                 /* no first part beside it */
        CHECK(!rename(away, part[0]));
    }

    /* two images of their own named like a split one's parts: two games */
    char v1[1024], v2[1024];
    mkpath(v1, sizeof(v1), "Game v1.1.cci");
    mkpath(v2, sizeof(v2), "Game v1.2.cci");
    CHECK(put_cci(v1, 0, SECTORS, 2, 0, &took, &packed));
    CHECK(put_cci(v2, 0, SECTORS, 2, 0, &took, &packed));
    CHECK(!nc_cci_later_part(v2));
    {
        const int fd = open(v1, O_RDONLY | O_BINARY);
        struct stat st;
        CHECK(fd >= 0 && nc_cci_attach(fd));
        CHECK(!fstat(fd, &st) && nc_cci_file_size(fd) == (uint64_t)st.st_size);    /* v1.2 not a part of it */
        check_image(fd, "Game v1.1.cci");
        nc_cci_detach(fd);
        close(fd);
    }

    /* a header of another kind: no CCI */
    char bad[1024];
    mkpath(bad, sizeof(bad), "bad.cci");
    static const struct {
        unsigned off;
        uint8_t byte;
    } k_bad_header[] = {
        { 0, 'X' },     /* magic */
        { 4, 31 },      /* header size */
        { 25, 0x10 },   /* block size 4096 */
        { 28, 2 },      /* version */
        { 29, 9 },      /* the index's shift */
        { 23, 0x10 },   /* the index past the file */
    };
    for (unsigned k = 0; k < sizeof(k_bad_header) / sizeof(k_bad_header[0]); k++) {
        CHECK(put_cci(bad, 0, SECTORS, 2, 0, &took, &packed));
        CHECK(patch(bad, k_bad_header[k].off, &k_bad_header[k].byte, 1));
        const int fd = open(bad, O_RDONLY | O_BINARY);
        CHECK(fd >= 0 && !nc_cci_attach(fd) && !nc_cci_is(fd));
        close(fd);
    }

    /* the index's offset past the 64 bits' end with the index's size */
    {
        CHECK(put_cci(bad, 0, SECTORS, 2, 0, &took, &packed));
        uint8_t far[8];
        w64(far, 0xFFFFFFFFFFFFFFF0ull);
        CHECK(patch(bad, 16, far, sizeof(far)));
        const int fd = open(bad, O_RDONLY | O_BINARY);
        CHECK(fd >= 0 && !nc_cci_attach(fd));
        close(fd);
    }

    /* an index entry out of place, and an LZ4 block that reaches back
     * before its sector: the reads of those sectors fail, the rest read */
    {
        CHECK(put_cci(bad, 0, SECTORS, 2, 0, &took, &packed));
        uint8_t h[32], e[4];
        CHECK(peek(bad, 0, h, sizeof(h)));
        const uint64_t index_off = (uint64_t)h[16] | (uint64_t)h[17] << 8 | (uint64_t)h[18] << 16 |
                                   (uint64_t)h[19] << 24;
        w32(e, 0x80000000u | 0x7FFFFFFFu);      /* sector 5's block far past the file (and so 4's end) */
        CHECK(patch(bad, index_off + 5 * 4, e, 4));
        /* sector 8: zeros, the block a literal then a match one back; that
         * match from two back instead (before the sector's start) */
        uint8_t blk[8];
        CHECK(peek(bad, index_off + 8 * 4, e, 4));
        const uint64_t at = (uint64_t)((e[0] | e[1] << 8 | e[2] << 16 | (uint32_t)(e[3] & 0x7F) << 24)) << 2;
        CHECK(e[3] & 0x80);
        CHECK(peek(bad, at, blk, sizeof(blk)));
        CHECK(blk[2] == 0 && blk[3] == 1 && blk[4] == 0);       /* pad, token, literal 0, back 1 */
        const uint8_t two[2] = { 2, 0 };
        CHECK(patch(bad, at + 3, two, 2));
        const int fd = open(bad, O_RDONLY | O_BINARY);
        CHECK(fd >= 0 && nc_cci_attach(fd));
        uint8_t *got = malloc(sizeof(g_img));
        CHECK(!nc_cci_read(fd, got, SECTOR, 5ull * SECTOR));
        CHECK(!nc_cci_read(fd, got, SECTOR, 4ull * SECTOR));
        CHECK(!nc_cci_read(fd, got, 10, 8ull * SECTOR + 100));
        CHECK(!nc_cci_read(fd, got, sizeof(g_img), 0));
        CHECK(nc_cci_read(fd, got, 4 * SECTOR, 0) && !memcmp(got, g_img, 4 * SECTOR));
        CHECK(nc_cci_read(fd, got, 2 * SECTOR, 6ull * SECTOR) && !memcmp(got, g_img + 6 * SECTOR, 2 * SECTOR));
        CHECK(nc_cci_read(fd, got, 100 * SECTOR, 9ull * SECTOR) && !memcmp(got, g_img + 9 * SECTOR, 100 * SECTOR));
        free(got);
        nc_cci_detach(fd);
        close(fd);
    }

    /* an ISO is no CCI */
    {
        char iso[1024];
        mkpath(iso, sizeof(iso), "game.iso");
        FILE *f = fopen(iso, "wb");
        CHECK(f && fwrite(g_img, 1, sizeof(g_img), f) == sizeof(g_img));
        if (f) {
            fclose(f);
        }
        const int fd = open(iso, O_RDONLY | O_BINARY);
        CHECK(fd >= 0 && !nc_cci_attach(fd) && !nc_cci_is(fd));
        close(fd);
        unlink(iso);
    }

    unlink(one);
    unlink(one0);
    for (unsigned k = 0; k < 8; k++) {
        unlink(part[k]);
    }
    unlink(v1);
    unlink(v2);
    unlink(bad);
    rmdir(g_root);
    printf(g_fail ? "cci_test: %d failed\n" : "cci_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
