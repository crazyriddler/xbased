/*
 * hakuX HLE GPU — push-buffer mode's resources (see pbres.h).
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/prop.h"
#include "hw/xbox/hle/front/fe.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/pb/pbres.h"
#include "hw/xbox/hle/pb/texpack.h"
#include <math.h>
#include "hw/xbox/hle/tex/xdk_res.h"
#include "hw/xbox/hle/tex/xg_swizzle.h"
#include "hw/xbox/hle/vk/back.h"
#include "hw/xbox/hle/vk/guest_ram.h"

/* ---- arena ------------------------------------------------------------------- */

/* A draw being put together (hle_draw_arena_hold): its blocks are tagged
 * with the submission open when each was made, but the draw goes out with
 * the one open when its command is written. A kick or a wait between (a
 * landing, a texture's upload with the arena full, a copy it waits on) had
 * the GPU pass the first while the draw was not even recorded: its blocks
 * went, and with the GPU idle the arena started again at 0 over its
 * constants and vertices. Warrior Within's ship then drew whole passes with
 * another pass's matrices (a view from under the sail, the deck black) or a
 * mesh's vertices from another, now and then, as the kicks fell. Held:
 * the draw's allocations in blocks of their own; before a retirement
 * the draw's own blocks (those made since its start) move on to the open
 * submission once the one they were tagged with is done; at its command,
 * to the submission it went out in. */
static uint64_t g_hold = UINT64_MAX, g_hold_made;

void hle_draw_arena_hold(bool on)
{
    if (on) {
        g_hold = hle_fe_last_value() + 1;
        g_hold_made = hle_arena_mark(&g_harena.alloc);
        return;
    }
    if (g_hold == UINT64_MAX) {
        return;
    }
    const uint64_t v = hle_fe_last_value() + 1;   /* the submission the draw is in */
    if (v > g_hold) {
        hle_arena_retag(&g_harena.alloc, g_hold_made, v);
    }
    hle_arena_coalesce(&g_harena.alloc);          /* its blocks back with the others */
    g_hold = UINT64_MAX;
}

/* Room in arena a for len bytes (HLE_ARENA_FULL: none even with the GPU
 * idle). Full: while its oldest block is a submitted one, wait for just
 * that submission (the blocks behind it stay in flight, the GPU busy);
 * once only the open one holds it, kick that and wait for it. Waiting for
 * all that was submitted left the GPU idle and the decoder stopped for
 * over 50 ms at a time. */
static uint32_t ring_alloc(HleArena *a, uint32_t len, uint32_t align)
{
    for (unsigned kicks = 0; kicks < 2;) {
        const uint64_t done = hle_tl_completed();
        if (a == &g_harena.alloc && g_hold != UINT64_MAX && done >= g_hold) {
            /* the draw being put together: its blocks go with the open
             * submission now (the one it will be in, or a later one) */
            g_hold = hle_fe_last_value() + 1;
            hle_arena_retag(a, g_hold_made, g_hold);
        }
        hle_arena_retire(a, done);
        uint32_t off = hle_arena_alloc(a, len, align, hle_fe_last_value() + 1);
        if (off != HLE_ARENA_FULL) {
            return off;
        }
        uint32_t pa = hle_hitch_enter(HLE_ACT_ARENA);
        uint64_t v = hle_arena_oldest(a);
        if (!v || v > hle_fe_last_value()) {
            /* everything pending belongs to the next submit: kick it */
            v = hle_fe_next_value();
            HleCmdFence *f = hle_fe_begin(HLE_OP_KICK, sizeof(*f));
            f->value = v;
            hle_fe_commit();
            kicks++;
        }
        hle_tl_wait(v);
        hle_hitch_leave(pa);
    }
    return HLE_ARENA_FULL;
}

static uint32_t arena_alloc(uint32_t len, uint32_t align, uint8_t **ptr)
{
    uint32_t off = ring_alloc(&g_harena.alloc, len, align);
    if (off != HLE_ARENA_FULL) {
        *ptr = g_harena.map + off;
    }
    return off;
}

uint32_t hle_draw_arena_alloc(uint32_t len, uint8_t **ptr)
{
    return arena_alloc(len, 256, ptr);
}

uint32_t hle_draw_arena_alloc16(uint32_t len, uint8_t **ptr)
{
    return arena_alloc(len, 16, ptr);
}

/* ---- textures ------------------------------------------------------------------ */

/* Packed byte size of one level of a w x h texture in format fi. */
static uint32_t level_bytes(const HleFmtInfo *fi, uint32_t w, uint32_t h, uint32_t *row)
{
    if (fi->flags & HLE_FI_DXT) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = fi->bpp * 2;   /* 8 or 16 */
        *row = bw * bb;
        return *row * bh;
    }
    *row = w * fi->bpp / 8;
    return *row * h;
}

/* DXT1/3/5 (BC1/2/3) volume -> A8R8G8B8 (bytes B, G, R, A). A DXT volume
 * level is not its slices one after another: the NV2A takes the depth four
 * slices at a time and, inside such a group, keeps each 4x4 block's slices
 * together (group, block row, block column, slice). Read slice by slice, the
 * blocks of other slices land all over the image (the dots on Forza's car
 * paint, whose reflection is a DXT5 volume). */
static uint32_t rgb565(uint16_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static uint32_t mix565(uint32_t a, uint32_t b, unsigned wa, unsigned wb, unsigned div)
{
    uint32_t o = 0;
    for (unsigned sh = 0; sh < 24; sh += 8) {
        o |= (((a >> sh & 0xFF) * wa + (b >> sh & 0xFF) * wb) / div) << sh;
    }
    return o;
}

/* one block at block (bx, by) of a w x h slice */
static void dxt_block(uint32_t xfmt, const uint8_t *s, uint32_t bx, uint32_t by, uint32_t w,
                      uint32_t h, uint32_t *out)
{
    const uint8_t *cb = s + (xfmt == HLE_XFMT_DXT1 ? 0 : 8);
    uint16_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
    uint32_t pal[4] = { rgb565(c0), rgb565(c1) }, idx = cb[4] | cb[5] << 8 |
                      cb[6] << 16 | (uint32_t)cb[7] << 24;
    bool four = xfmt != HLE_XFMT_DXT1 || c0 > c1;
    pal[2] = four ? mix565(pal[0], pal[1], 2, 1, 3) : mix565(pal[0], pal[1], 1, 1, 2);
    pal[3] = four ? mix565(pal[0], pal[1], 1, 2, 3) : 0;
    uint8_t a[16];
    if (xfmt == HLE_XFMT_DXT3) {
        for (unsigned i = 0; i < 16; i++) {
            unsigned v = (s[i / 2] >> (4 * (i & 1))) & 15;
            a[i] = v << 4 | v;
        }
    } else if (xfmt == HLE_XFMT_DXT5) {
        unsigned a0 = s[0], a1 = s[1], ap[8] = { a0, a1 };
        for (unsigned i = 2; i < 8; i++) {
            ap[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7 :
                    i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : i == 6 ? 0 : 255;
        }
        uint64_t bits = 0;
        for (unsigned i = 0; i < 6; i++) {
            bits |= (uint64_t)s[2 + i] << (8 * i);
        }
        for (unsigned i = 0; i < 16; i++) {
            a[i] = ap[(bits >> (3 * i)) & 7];
        }
    } else {
        for (unsigned i = 0; i < 16; i++) {
            a[i] = !four && ((idx >> (2 * i)) & 3) == 3 ? 0 : 255;
        }
    }
    for (unsigned i = 0; i < 16; i++) {
        uint32_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
        if (x < w && y < h) {
            out[y * w + x] = pal[(idx >> (2 * i)) & 3] | (uint32_t)a[i] << 24;
        }
    }
}

static void dxt_volume(uint32_t xfmt, const uint8_t *s, uint32_t w, uint32_t h, uint32_t d,
                       uint32_t *out)
{
    unsigned bb = xfmt == HLE_XFMT_DXT1 ? 8 : 16;
    for (uint32_t z0 = 0; z0 < d; z0 += 4) {
        uint32_t z1 = MIN(d, z0 + 4);
        for (uint32_t by = 0; by < (h + 3) / 4; by++) {
            for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                for (uint32_t z = z0; z < z1; z++, s += bb) {
                    dxt_block(xfmt, s, bx, by, w, h, out + (size_t)z * w * h);
                }
            }
        }
    }
}

/* A4R4G4B4 -> A8R8G8B8 (bytes B, G, R, A), each 4-bit value n as n * 17, for a
 * GPU without the 4444 formats (Mali's r26: HleCaps.x4444 off) */
static void expand_4444(const uint16_t *s, size_t n, uint32_t *out)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        uint32_t x = (c & 0xF) | (c & 0xF0) << 4 | (c & 0xF00) << 8 | (c & 0xF000) << 12;
        out[i] = x | x << 4;
    }
}

static uint8_t clip8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* YUY2 / UYVY (BT.601 video range) -> B8G8R8A8, w x h texels: integer form
 * as xemu's convert_yuy2_to_rgb. */
static void yuv_convert(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, bool uyvy,
                        uint8_t *dst)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *l = src + y * pitch;
        uint8_t *q = dst + y * w * 4;
        for (uint32_t x = 0; x + 1 < w; x += 2, l += 4, q += 8) {
            int y0 = (uyvy ? l[1] : l[0]) - 16, y1 = (uyvy ? l[3] : l[2]) - 16;
            int u = (uyvy ? l[0] : l[1]) - 128, v = (uyvy ? l[2] : l[3]) - 128;
            int yy[2] = { y0, y1 };
            for (unsigned k = 0; k < 2; k++) {
                int c = 298 * yy[k];
                q[4 * k + 0] = clip8((c + 516 * u + 128) >> 8);
                q[4 * k + 1] = clip8((c - 100 * u - 208 * v + 128) >> 8);
                q[4 * k + 2] = clip8((c + 409 * v + 128) >> 8);
                q[4 * k + 3] = 255;
            }
        }
    }
}

/* A YUY2 / UYVY texture's texels converted to A8R8G8B8 into the arena as
 * object id's image (HLE_OP_TEX), sent as a linear A8R8G8B8 texture (texel
 * coordinates like the source); false if they can't be. */
static bool yuv_define(uint32_t id, const uint32_t h5[5], const HleResDesc *d)
{
    uint32_t pitch;
    uint32_t off0 = hle_res_subresource(d, 0, 0, &pitch);
    const uint8_t *src = hle_pa_ptr(hle_res_data_pa(h5[1]) + off0, pitch * d->h);
    if (!src || d->w < 2) {
        return false;
    }
    uint8_t *dst;
    uint32_t total = d->w * d->h * 4;
    uint32_t off = arena_alloc(total, 16, &dst);
    if (off == HLE_ARENA_FULL) {
        return false;
    }
    yuv_convert(src, pitch, d->w, d->h, d->fmt == HLE_XFMT_UYVY, dst);
    static unsigned s_logged;
    if (s_logged++ < 4) {               /* movies: when their frames start converting */
        hle_log("hakuX-hle-front", "YUV texture %u: %ux%u %s converted (#%u of 4 logged)", id,
                d->w, d->h, d->fmt == HLE_XFMT_UYVY ? "UYVY" : "YUY2", s_logged);
    }
    HleCmdTex *c = hle_fe_begin(HLE_OP_TEX, sizeof(*c));
    c->id = id;
    c->xfmt = HLE_XFMT_LIN_A8R8G8B8;
    c->w = d->w;
    c->h = d->h;
    c->d = 1;
    c->levels = 1;
    c->faces = 1;
    c->arena_off = off;
    c->arena_len = total;
    return true;
}

/* What header words h5 describe, when it is a kind of texture the back end
 * defines (else logged, the first few). */
static bool tex_desc(const uint32_t h5[5], HleResDesc *dp, HleResInfo *rip)
{
    HleResDesc d;
    HleResInfo ri;
    if (!hle_res_from_header(h5[0], h5[3], h5[4], &d) || !hle_res_info(&d, &ri)) {
        static unsigned s_logged;
        if (s_logged++ < 8) {
            hle_log("hakuX-hle-front", "texture header not understood: %08x %08x %08x %08x "
                    "%08x (not bound; first 8 logged)", h5[0], h5[1], h5[2], h5[3], h5[4]);
        }
        return false;
    }
    if (d.kind != HLE_RK_TEXTURE && d.kind != HLE_RK_CUBE && d.kind != HLE_RK_SURFACE &&
        d.kind != HLE_RK_VOLUME) {
        static uint32_t s_kinds;
        if (!(s_kinds & (1u << (d.kind & 31)))) {
            s_kinds |= 1u << (d.kind & 31);
            hle_log("hakuX-hle-front", "texture kind %u not bound (%08x %08x %08x %08x %08x)",
                    d.kind, h5[0], h5[1], h5[2], h5[3], h5[4]);
        }
        return false;
    }
    *dp = d;
    *rip = ri;
    return true;
}

/* ---- texture packs (texpack.h) ---- */

/* The largest level of texture d (its first face) as bytes R, G, B, A, as the texture samples: g_free; NULL for
 * a kind this doesn't decode (a volume, a linear, signed or two-channel format). */
static uint8_t *tex_rgba(const HleResDesc *d, const HleFmtInfo *fi, const uint8_t *src, const uint32_t *pal)
{
    const uint32_t w = d->w, h = d->h;
    const size_t n = (size_t)w * h;
    uint32_t pitch, sw, sh, bo;
    const uint8_t *s = src + hle_res_subresource(d, 0, 0, &pitch);
    hle_res_level(d, 0, &sw, &sh, &bo);
    HleXgRect img = { bo, bo, w, h };
    uint8_t *o = g_malloc(n * 4);
    if ((fi->flags & HLE_FI_DXT) || d->fmt == HLE_XFMT_P8) {
        /* both come out as A8R8G8B8 (bytes B, G, R, A) */
        if (d->fmt == HLE_XFMT_P8) {
            hle_xg_unswizzle_rect_p8(s, sw, sh, &img, pal, o, w * 4, 0, 0);
        } else {
            dxt_volume(d->fmt, s, w, h, 1, (uint32_t *)o);
        }
        for (size_t i = 0; i < n; i++) {
            const uint8_t b = o[i * 4];
            o[i * 4] = o[i * 4 + 2];
            o[i * 4 + 2] = b;
        }
        return o;
    }
    const uint32_t bytes = fi->bpp / 8;
    if (!(fi->flags & HLE_FI_SWZ) || bytes < 1 || bytes > 4) {
        g_free(o);
        return NULL;
    }
    g_autofree uint8_t *t = g_malloc(n * bytes);
    hle_xg_unswizzle_rect(s, sw, sh, &img, t, w * bytes, 0, 0, bytes);
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = t + i * bytes;
        const unsigned v = bytes == 2 ? (unsigned)(p[0] | p[1] << 8) : p[0];
        uint8_t r, g, b, a = 255;
        switch (d->fmt) {
        case HLE_XFMT_A8R8G8B8: b = p[0]; g = p[1]; r = p[2]; a = p[3]; break;
        case HLE_XFMT_X8R8G8B8: b = p[0]; g = p[1]; r = p[2]; break;
        case HLE_XFMT_A8B8G8R8: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
        case HLE_XFMT_B8G8R8A8: a = p[0]; r = p[1]; g = p[2]; b = p[3]; break;
        case HLE_XFMT_R8G8B8A8: a = p[0]; b = p[1]; g = p[2]; r = p[3]; break;
        case HLE_XFMT_R5G6B5:
            r = (uint8_t)((v >> 11) * 255 / 31);
            g = (uint8_t)((v >> 5 & 63) * 255 / 63);
            b = (uint8_t)((v & 31) * 255 / 31);
            break;
        case HLE_XFMT_A1R5G5B5: case HLE_XFMT_X1R5G5B5:
            r = (uint8_t)((v >> 10 & 31) * 255 / 31);
            g = (uint8_t)((v >> 5 & 31) * 255 / 31);
            b = (uint8_t)((v & 31) * 255 / 31);
            a = d->fmt == HLE_XFMT_X1R5G5B5 || (v & 0x8000) ? 255 : 0;
            break;
        case HLE_XFMT_A4R4G4B4:
            a = (uint8_t)((v >> 12) * 17);
            r = (uint8_t)((v >> 8 & 15) * 17);
            g = (uint8_t)((v >> 4 & 15) * 17);
            b = (uint8_t)((v & 15) * 17);
            break;
        case HLE_XFMT_L8: r = g = b = p[0]; break;
        case HLE_XFMT_AL8: r = g = b = a = p[0]; break;
        case HLE_XFMT_A8: r = g = b = 255; a = p[0]; break;
        case HLE_XFMT_A8L8: r = g = b = p[0]; a = p[1]; break;
        default:
            g_free(o);
            return NULL;
        }
        o[i * 4] = r;
        o[i * 4 + 1] = g;
        o[i * 4 + 2] = b;
        o[i * 4 + 3] = a;
    }
    return o;
}

/* An upscaler's picture against the game's own texture: it must be a whole number of times its size and,
 * averaged back down, look like it in colour (where the texture isn't transparent; everywhere if nearly all of
 * it is). A picture that fails (an upscaler's black or garbled output) is not used. The picture's alpha is not
 * looked at: it is replaced (texpack_alpha). */
static bool texpack_like(const uint8_t *px, uint32_t w, uint32_t h, const uint8_t *own, uint32_t ow, uint32_t oh,
                         float *colour)
{
    *colour = -1.0f;                    /* not compared */
    if (!ow || !oh || w % ow || h % oh || w / ow != h / oh) {
        return false;
    }
    const uint32_t k = w / ow;
    /* a grid of at most 64 x 64 of the texture's texels is enough to tell */
    const uint32_t sx = MAX(ow / 64, 1u), sy = MAX(oh / 64, 1u);
    double dc = 0.0, wc = 0.0, du = 0.0, n = 0.0;
    for (uint32_t y = 0; y < oh; y += sy) {
        for (uint32_t x = 0; x < ow; x += sx) {
            uint32_t sum[3] = { 0 };
            for (uint32_t j = 0; j < k; j++) {
                const uint8_t *p = px + (((size_t)y * k + j) * w + (size_t)x * k) * 4;
                for (uint32_t i = 0; i < k; i++, p += 4) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const uint8_t *o = own + ((size_t)y * ow + x) * 4;
            const double a = o[3] / 255.0, kk = (double)k * k;
            const double e = (fabs(sum[0] / kk - o[0]) + fabs(sum[1] / kk - o[1]) + fabs(sum[2] / kk - o[2])) / 3.0;
            dc += a * e;
            wc += a;
            du += e;
            n += 1.0;
        }
    }
    *colour = (float)(wc > 0.05 * n ? dc / wc : du / n);
    return *colour < 28.0f;
}

/* The game's own alpha into the upscaler's picture (k times the size), read bilinearly. The upscaler is given
 * colour alone: what it made of textures with an alpha channel came back wrong in alpha and often in colour (a
 * log of 1.0.76.26: 180 of 193 pictures refused, nearly all of palettized or A8R8G8B8 textures). */
static void texpack_alpha(uint8_t *px, uint32_t w, uint32_t h, const uint8_t *own, uint32_t ow, uint32_t oh)
{
    const float k = (float)w / (float)ow;
    for (uint32_t y = 0; y < h; y++) {
        const float fy = MAX(((float)y + 0.5f) / k - 0.5f, 0.0f);
        const uint32_t y0 = MIN((uint32_t)fy, oh - 1), y1 = MIN(y0 + 1, oh - 1);
        const float ty = fy - (float)y0;
        for (uint32_t x = 0; x < w; x++) {
            const float fx = MAX(((float)x + 0.5f) / k - 0.5f, 0.0f);
            const uint32_t x0 = MIN((uint32_t)fx, ow - 1), x1 = MIN(x0 + 1, ow - 1);
            const float tx = fx - (float)x0;
            const float a = ((float)own[((size_t)y0 * ow + x0) * 4 + 3] * (1.0f - tx) +
                             (float)own[((size_t)y0 * ow + x1) * 4 + 3] * tx) * (1.0f - ty) +
                            ((float)own[((size_t)y1 * ow + x0) * 4 + 3] * (1.0f - tx) +
                             (float)own[((size_t)y1 * ow + x1) * 4 + 3] * tx) * ty;
            px[((size_t)y * w + x) * 4 + 3] = (uint8_t)(a + 0.5f);
        }
    }
}

/* A texture as a file on the disc holds it (an XPR container's resource: its header's words and its data, `avail`
 * bytes of it), for the textures to be written out without the game loading them (native/app/texscan.c): the
 * hash the game's own definition of it would get, and its largest level as R, G, B, A (g_free). False for
 * what the game's definition wouldn't take either (hle_draw_tex_define's rules for a pack), a palettized
 * texture (its palette is another resource, bound at run time) and data that isn't all there. */
bool hle_draw_tex_scan(const uint32_t common, uint32_t format, uint32_t size, const uint8_t *data, size_t avail,
                       uint64_t *hash, uint8_t **rgba, uint32_t *w, uint32_t *h)
{
    HleResDesc d;
    HleResInfo ri;
    if (!hle_res_from_header(common, format, size, &d) || !hle_res_info(&d, &ri)) {
        return false;
    }
    const HleFmtInfo *fi = hle_fmt_info(d.fmt);
    if (d.kind != HLE_RK_TEXTURE || !fi || (fi->flags & (HLE_FI_YUV | HLE_FI_DEPTH | HLE_FI_LIN)) ||
        d.fmt == HLE_XFMT_P8 || d.w < 16 || d.h < 16 || d.w > 4096 || d.h > 4096 || !ri.bytes || ri.bytes > avail) {
        return false;
    }
    const uint32_t levels = MAX(ri.levels, 1u);
    uint32_t p0, p1;
    const uint32_t o0 = hle_res_subresource(&d, 0, 0, &p0);
    const uint32_t o1 = levels > 1 ? hle_res_subresource(&d, 0, 1, &p1) : ri.bytes;
    if (o1 <= o0 || o1 > ri.bytes) {
        return false;
    }
    uint8_t *px = tex_rgba(&d, fi, data, NULL);
    if (!px) {
        return false;
    }
    *hash = hle_texpack_hash(data + o0, o1 - o0, d.fmt, d.w, d.h, NULL, 0);
    *rgba = px;
    *w = d.w;
    *h = d.h;
    return true;
}

static bool g_pack_deferred, g_pack_pending;
static uint64_t g_pack_pending_hash;

bool hle_draw_tex_pack_pending(uint64_t *hash)
{
    const bool p = g_pack_pending;
    g_pack_pending = false;
    *hash = g_pack_pending_hash;
    return p;
}

bool hle_draw_tex_pack_deferred(void)
{
    const bool d = g_pack_deferred;
    g_pack_deferred = false;
    return d;
}

/* The largest side a pack's picture goes up at (hle_host_texpack_cap: by the GPU's memory), 2048 at most. */
static uint32_t texpack_cap(void)
{
    const int c = hle_host_texpack_cap();
    return c >= 2048 ? 2048u : c >= 1024 ? 1024u : c >= 512 ? 512u : c > 0 ? 256u : 2048u;
}

/* The pack's picture for `hash` into the arena as object id's image, with its smaller levels made here when the
 * game's texture has any (each the average of four texels of the one above). 1: done; 0: there is none (or it
 * isn't usable); -1: there is one but no room for it now (hle_draw_tex_pack_deferred: asked again a frame
 * later, the game's own meanwhile).
 *
 * Room: these pictures are megabytes, and the arena is a ring the draws' vertices and constants share. Waiting
 * for room (ring_alloc) in the middle of a draw ends the submission that draw's own data was put in, and the
 * next allocation may land on it: with pictures this size that happened at every other texture (the draws came
 * out with other draws' data: black scenes, noise, a texture where another belongs). So a picture only goes up
 * when it fits in what is free now, with a quarter of the arena left for the draws. */
static int texpack_define(uint32_t id, uint64_t hash, bool mips, const HleResDesc *d, const HleFmtInfo *fi,
                          const uint8_t *src, const uint32_t *pal)
{
    HleArena *a = &g_harena.alloc;
    if (!hle_texpack_has(hash)) {
        return 0;
    }
    const uint32_t cap = texpack_cap();
    const uint32_t worst = cap * cap * 4 / 3 * 4 + (1u << 20);
    hle_arena_retire(a, hle_tl_completed());
    if (worst > a->size / 2 || hle_arena_used(a) + worst > a->size / 4 * 3) {
        g_pack_deferred = worst <= a->size / 2;     /* an arena too small for it at all: never */
        return g_pack_deferred ? -1 : 0;
    }
    uint32_t w = 0, h = 0;
    bool made = false;
    uint8_t *px = hle_texpack_load(hash, &w, &h, &made);
    if (!px) {
        return 0;
    }
    if (made) {
        g_autofree uint8_t *own = tex_rgba(d, fi, src, pal);
        float dc = -1.0f;
        if (!own || !texpack_like(px, w, h, own, d->w, d->h, &dc)) {
            hle_texpack_reject(hash, w, h, d->w, d->h, d->fmt, own != NULL, dc);
            hle_texpack_free(px);
            return 0;
        }
        texpack_alpha(px, w, h, own, d->w, d->h);
    }
    hle_texpack_used(hash, w, h, made);
    /* its levels, each the average of four texels of the one above, on the heap (the arena's memory reads
     * slowly); the first of at most `cap` a side is the one that goes up, with those under it if the game's
     * texture has levels (an upscaler's 4x of a large texture is taken at half or a quarter) */
    enum { MAXL = 14 };
    uint8_t *lv[MAXL] = { px };
    uint32_t lw[MAXL] = { w }, lh[MAXL] = { h };
    unsigned n = 1, first = 0;
    while (lw[first] > cap || lh[first] > cap || (mips && (lw[n - 1] > 1 || lh[n - 1] > 1))) {
        if (n == MAXL) {
            break;
        }
        const uint32_t pw = lw[n - 1], ph = lh[n - 1], cw = MAX(pw / 2, 1u), ch = MAX(ph / 2, 1u);
        const uint8_t *prev = lv[n - 1];
        uint8_t *cur = g_malloc((size_t)cw * ch * 4);
        for (uint32_t y = 0; y < ch; y++) {
            const uint32_t y0 = MIN(y * 2, ph - 1), y1 = MIN(y * 2 + 1, ph - 1);
            for (uint32_t x = 0; x < cw; x++) {
                const uint32_t x0 = MIN(x * 2, pw - 1), x1 = MIN(x * 2 + 1, pw - 1);
                for (unsigned c = 0; c < 4; c++) {
                    cur[((size_t)y * cw + x) * 4 + c] = (uint8_t)(
                        (prev[((size_t)y0 * pw + x0) * 4 + c] + prev[((size_t)y0 * pw + x1) * 4 + c] +
                         prev[((size_t)y1 * pw + x0) * 4 + c] + prev[((size_t)y1 * pw + x1) * 4 + c] + 2) / 4);
                }
            }
        }
        lv[n] = cur;
        lw[n] = cw;
        lh[n] = ch;
        n++;
        if (lw[first] > cap || lh[first] > cap) {
            first = n - 1;
        }
    }
    const uint32_t levels = mips ? n - first : 1;
    uint32_t total = 0;
    for (uint32_t l = 0; l < levels; l++) {
        total += lw[first + l] * lh[first + l] * 4;
    }
    /* no waiting for room (see above): what is free now, or not this time */
    uint32_t off = HLE_ARENA_FULL;
    if (lw[first] <= cap && lh[first] <= cap && total <= worst) {
        off = hle_arena_alloc(a, total, 16, hle_fe_last_value() + 1);
    }
    if (off != HLE_ARENA_FULL) {
        uint8_t *dst = g_harena.map + off;
        uint32_t pos = 0;
        for (uint32_t l = 0; l < levels; l++) {
            const uint32_t bytes = lw[first + l] * lh[first + l] * 4;
            memcpy(dst + pos, lv[first + l], bytes);
            pos += bytes;
        }
    }
    w = lw[first];
    h = lh[first];
    for (unsigned i = 1; i < n; i++) {
        g_free(lv[i]);
    }
    hle_texpack_free(px);
    if (off == HLE_ARENA_FULL) {
        g_pack_deferred = true;
        return -1;
    }
    HleCmdTex *c = hle_fe_begin(HLE_OP_TEX, sizeof(*c));
    c->id = id;
    c->xfmt = HLE_XFMT_A8B8G8R8;        /* bytes R, G, B, A */
    c->w = w;
    c->h = h;
    c->d = 1;
    c->levels = levels;
    c->faces = 1;
    c->arena_off = off;
    c->arena_len = total;
    return 1;
}

/* The texels of texture d (header words h5; P8: palette pal, npal entries)
 * unswizzled or converted into the arena as object id's image (HLE_OP_TEX);
 * false if they can't be (format, memory, arena). */
static bool tex_define(uint32_t id, const uint32_t h5[5], const HleResDesc *dp,
                       const HleResInfo *rip, const uint32_t *pal, uint32_t npal)
{
    const HleResDesc d = *dp;
    const HleResInfo ri = *rip;
    const HleFmtInfo *fi = hle_fmt_info(d.fmt);
    bool p8 = d.fmt == HLE_XFMT_P8;
    if (!fi || (fi->flags & (HLE_FI_YUV | HLE_FI_DEPTH)) || (p8 && !npal)) {
        /* YUV / depth textures, P8 without a palette */
        static uint32_t s_seen[8];
        for (unsigned i = 0; i < 8; i++) {
            if (s_seen[i] == d.fmt + 1) {
                break;
            }
            if (!s_seen[i]) {
                s_seen[i] = d.fmt + 1;
                hle_log("hakuX-hle-front", "texture format 0x%x not uploaded yet (%ux%u kind %u)",
                        d.fmt, d.w, d.h, d.kind);
                break;
            }
        }
        return false;
    }
    const uint8_t *src = hle_pa_ptr(hle_res_data_pa(h5[1]), ri.bytes);
    if (!src) {
        return false;
    }
    /* P8 expands to A8R8G8B8 on the way into the arena, and so does DXT in a
     * volume (dxt_volume) or on a GPU without BC (Mali, PowerVR's B-Series:
     * one slice); a volume's levels are depth slices (3D swizzle, or DXT
     * blocks in groups of four slices), depth halving per level */
    bool vol = d.kind == HLE_RK_VOLUME;
    bool dxtd = (fi->flags & HLE_FI_DXT) && (vol || !g_hvk.caps.bc);
    /* A4R4G4B4 on a GPU without the 4444 formats: A8R8G8B8 as well (expand_4444),
     * a linear texture a linear one */
    bool x4444 = (d.fmt == HLE_XFMT_A4R4G4B4 || d.fmt == HLE_XFMT_LIN_A4R4G4B4) &&
                 !g_hvk.caps.x4444;
    /* R6G5B5 (L6V5U5): no host format, A8R8G8B8 too (hle_tex_expand_r6g5b5), its signed
     * channels as h5[2] says */
    bool r655 = d.fmt == HLE_XFMT_R6G5B5 || d.fmt == HLE_XFMT_LIN_R6G5B5;
    bool x16 = x4444 || r655;           /* 16-bit texels expanded to 32 */
    uint32_t oxfmt = p8 || dxtd ? HLE_XFMT_A8R8G8B8 :
                     x16 ? (fi->flags & HLE_FI_LIN ? HLE_XFMT_LIN_A8R8G8B8 : HLE_XFMT_A8R8G8B8) : d.fmt;
    const HleFmtInfo *ofi = hle_fmt_info(oxfmt);
    uint32_t faces = d.kind == HLE_RK_CUBE ? 6 : 1, levels = MAX(ri.levels, 1u);
    uint32_t depth = vol ? MAX(d.d, 1u) : 1;
    /* what it comes to in the arena, in 64 bits: an expanded volume (P8, DXT) is up to eight
     * times its data */
    uint64_t total64 = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t row;
        total64 += (uint64_t)level_bytes(ofi, MAX(d.w >> l, 1u), MAX(d.h >> l, 1u), &row) *
                   MAX(depth >> l, 1u);
    }
    total64 *= faces;
    if (total64 > (1u << 30)) {
        return false;                           /* more than any arena */
    }
    const uint32_t total = (uint32_t)total64;
    /* Texture packs: a plain texture (not a cube, a volume, a linear or a signed one, 16 texels a side or more:
     * an upscaler makes nothing of less) is known by a hash of its
     * largest level as the game holds it; the pack's picture goes up in its place, or the game's own is written
     * out for a pack to be made from. */
    const int pack = hle_host_texpack();
    if ((pack & 7) && !vol && faces == 1 && !h5[2] && !(fi->flags & HLE_FI_LIN) && d.w >= 16 && d.h >= 16) {
        uint32_t p0, p1;
        const uint32_t o0 = hle_res_subresource(&d, 0, 0, &p0);
        const uint32_t o1 = levels > 1 ? hle_res_subresource(&d, 0, 1, &p1) : ri.bytes;
        if (o1 > o0 && o1 <= ri.bytes) {
            const uint64_t hash = hle_texpack_hash(src + o0, o1 - o0, d.fmt, d.w, d.h, p8 ? pal : NULL,
                                                   p8 ? npal : 0);
            const int got = (pack & 5) ? texpack_define(id, hash, levels > 1, &d, fi, src, pal) : 0;
            if (got > 0) {
                return true;
            }
            if ((pack & 6) && hle_texpack_dump_wanted(hash)) {
                g_autofree uint8_t *rgba = tex_rgba(&d, fi, src, pal);
                if (rgba) {
                    hle_texpack_dump(hash, rgba, d.w, d.h);
                    hle_texpack_note_dumped(hash);
                }
            }
            /* no picture yet, and the upscaler runs beside the game: the draw code asks for it again */
            if (!got && hle_texpack_awaited(hash)) {
                g_pack_pending = true;
                g_pack_pending_hash = hash;
            }
        }
    }
    uint8_t *dst;
    uint32_t off = arena_alloc(total, 16, &dst);
    if (off == HLE_ARENA_FULL) {
        return false;
    }
    uint32_t pos = 0;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t w = MAX(d.w >> l, 1u), h = MAX(d.h >> l, 1u), row, pitch;
            uint32_t dd = MAX(depth >> l, 1u), n = level_bytes(ofi, w, h, &row) * dd;
            const uint8_t *s = src + hle_res_subresource(&d, f, l, &pitch);
            /* a bordered level (hle_res_level): the image inside the border;
             * a cube's border holds its neighbours' texels, which the host's
             * seamless cube filtering takes from them itself */
            uint32_t sw, sh, bo;
            hle_res_level(&d, l, &sw, &sh, &bo);
            HleXgRect img = { bo, bo, w, h };
            /* x16: the 16-bit texels to a scratch copy first, expanded below */
            uint8_t *out = dst + pos;
            uint32_t orow = row;
            g_autofree uint8_t *t16 = NULL;
            if (x16) {
                orow = w * 2;
                out = t16 = g_malloc((size_t)orow * h * dd);
            }
            if (dxtd && !vol) {
                dxt_volume(d.fmt, s, w, h, 1, (uint32_t *)(dst + pos));
            } else if (vol) {
                if (dxtd) {
                    dxt_volume(d.fmt, s, w, h, dd, (uint32_t *)(dst + pos));
                } else if (p8) {
                    g_autofree uint8_t *idx = g_malloc((size_t)w * h * dd);
                    hle_xg_unswizzle_box(s, w, h, dd, NULL, idx, w, w * h, 0, 0, 0, 1);
                    uint32_t *o32 = (uint32_t *)(dst + pos);
                    for (uint32_t i = 0; i < w * h * dd; i++) {
                        o32[i] = pal[idx[i]];
                    }
                } else {
                    hle_xg_unswizzle_box(s, w, h, dd, NULL, out, orow, orow * h, 0, 0, 0,
                                         fi->bpp / 8);
                }
            } else if (p8) {
                hle_xg_unswizzle_rect_p8(s, sw, sh, &img, pal, dst + pos, row, 0, 0);
            } else if (fi->flags & HLE_FI_SWZ) {
                hle_xg_unswizzle_rect(s, sw, sh, &img, out, orow, 0, 0, fi->bpp / 8);
            } else if (fi->flags & HLE_FI_DXT) {
                memcpy(dst + pos, s, n);
            } else {
                uint32_t sp = pitch ? pitch : orow;
                for (uint32_t y = 0; y < h; y++) {
                    memcpy(out + y * orow, s + y * sp, orow);
                }
            }
            if (h5[2] && !p8 && !dxtd && !x16 && !(fi->flags & HLE_FI_DXT)) {
                /* signed channels (hle_tex_sign_xor): their bytes flipped */
                const size_t nt = (size_t)w * h * dd;
                if (fi->bpp == 32) {
                    uint32_t *p = (uint32_t *)out;
                    for (size_t i = 0; i < nt; i++) {
                        p[i] ^= h5[2];
                    }
                } else if (fi->bpp == 16) {
                    uint16_t *p = (uint16_t *)out;
                    for (size_t i = 0; i < nt; i++) {
                        p[i] ^= (uint16_t)h5[2];
                    }
                }
            }
            if (x4444) {
                expand_4444((const uint16_t *)t16, (size_t)w * h * dd, (uint32_t *)(dst + pos));
            } else if (r655) {
                hle_tex_expand_r6g5b5((const uint16_t *)t16, (size_t)w * h * dd, h5[2],
                                      (uint32_t *)(dst + pos));
            }
            pos += n;
        }
    }
    {   /* debug: HAKUX_HLE_TEXDUMP=<id>, that texture's linear texels once */
        static int64_t want = -2;
        if (want == -2) {
            const char *e = getenv("HAKUX_HLE_TEXDUMP");
            want = e ? atoll(e) : -1;
        }
#if defined(__ANDROID__) || defined(_WIN32) || defined(NC_DESKTOP)
        {   /* on demand: a new value of debug.hakux.texdump=<id>, its next upload */
            static HleProp prop;
            char v[PROP_VALUE_MAX];
            if (hle_prop_new(&prop, "debug.hakux.texdump", v) && atoll(v) > 0) {
                want = atoll(v);
            }
        }
#endif
        if (want == id) {
            want = -1;
            g_autofree char *p = g_strdup_printf("%shle_shadow/tex_%u_f%x_%ux%u_l%u.bin",
                                                 hle_host_base_path(), id, oxfmt, d.w, d.h,
                                                 levels);
            g_file_set_contents(p, (const char *)dst, total, NULL);
            hle_log("hakuX-hle-front", "texdump %s (%u bytes)", p, total);
        }
    }
    HleCmdTex *c = hle_fe_begin(HLE_OP_TEX, sizeof(*c));
    c->id = id;
    c->xfmt = oxfmt;
    c->w = d.w;
    c->h = d.h;
    c->d = depth;
    c->levels = levels;
    c->faces = faces;
    c->arena_off = off;
    c->arena_len = total;
    return true;
}

uint32_t hle_draw_tex_bytes(const uint32_t h5[5])
{
    HleResDesc d;
    HleResInfo ri;
    return hle_res_from_header(h5[0], h5[3], h5[4], &d) && hle_res_info(&d, &ri) ? ri.bytes : 0;
}

uint32_t hle_draw_tex_define(uint32_t id, const uint32_t h5[5], const uint32_t *pal, uint32_t npal,
                             uint32_t *bytes)
{
    HleResDesc d;
    HleResInfo ri;
    if (!tex_desc(h5, &d, &ri)) {
        return 0;
    }
    *bytes = ri.bytes;
    if (d.fmt == HLE_XFMT_YUY2 || d.fmt == HLE_XFMT_UYVY) {
        return yuv_define(id, h5, &d) ? id : 0;        /* movies: converted to A8R8G8B8 */
    }
    return tex_define(id, h5, &d, &ri, pal, npal) ? id : 0;
}

/* The overlay is not a texture object: the video decoder rewrites it every
 * frame and the back end keeps its own image (HleCmdOverlay). */
uint32_t hle_draw_overlay_upload(const uint32_t h5[5], uint32_t *w, uint32_t *h)
{
    HleResDesc d;
    HleResInfo ri;
    uint32_t pitch;
    if (!hle_res_from_header(h5[0], h5[3], h5[4], &d) || !hle_res_info(&d, &ri) ||
        (d.fmt != HLE_XFMT_YUY2 && d.fmt != HLE_XFMT_UYVY) || d.w < 2 || !d.h) {
        return HLE_ARENA_FULL;
    }
    uint32_t off0 = hle_res_subresource(&d, 0, 0, &pitch);
    const uint8_t *src = hle_pa_ptr(hle_res_data_pa(h5[1]) + off0, pitch * d.h);
    uint8_t *dst;
    uint32_t off = src ? arena_alloc(d.w * d.h * 4, 16, &dst) : HLE_ARENA_FULL;
    if (off != HLE_ARENA_FULL) {
        yuv_convert(src, pitch, d.w, d.h, d.fmt == HLE_XFMT_UYVY, dst);
        *w = d.w;
        *h = d.h;
    }
    return off;
}

/* ---- primitives ---------------------------------------------------------------- */

enum {
    PT_POINTLIST = 1, PT_LINELIST, PT_LINELOOP, PT_LINESTRIP, PT_TRIANGLELIST,
    PT_TRIANGLESTRIP, PT_TRIANGLEFAN, PT_QUADLIST, PT_QUADSTRIP, PT_POLYGON,
    PT_QUADLIST_EDGES, PT_QUADSTRIP_EDGES,      /* pbres.h */
};
/* VkPrimitiveTopology (no Vulkan headers here) */
enum { VT_POINTS = 0, VT_LINES = 1, VT_LINE_STRIP = 2, VT_TRIS = 3, VT_TRI_STRIP = 4,
       VT_TRI_FAN = 5 };

uint32_t hle_draw_prim_topology(uint32_t pt)
{
    switch (pt) {
    case PT_POINTLIST:     return VT_POINTS;
    case PT_LINELIST:
    case PT_QUADLIST_EDGES:
    case PT_QUADSTRIP_EDGES: return VT_LINES;
    case PT_LINELOOP:
    case PT_LINESTRIP:     return VT_LINE_STRIP;
    case PT_TRIANGLESTRIP:
    case PT_QUADSTRIP:     return VT_TRI_STRIP;    /* same vertex order */
    case PT_TRIANGLEFAN:
    case PT_POLYGON:       return VT_TRI_FAN;
    default:               return VT_TRIS;         /* lists; quads become triangles */
    }
}

uint32_t hle_draw_prim_count(uint32_t pt, uint32_t n)
{
    if (pt == PT_QUADLIST) {
        return n / 4 * 6;
    }
    if (pt == PT_LINELOOP) {
        return n ? n + 1 : 0;
    }
    if (pt == PT_QUADLIST_EDGES) {
        return n / 4 * 8;
    }
    if (pt == PT_QUADSTRIP_EDGES) {
        return n >= 4 ? (n / 2 - 1) * 8 : 0;
    }
    return n;
}

void hle_draw_prim_indices(uint32_t pt, const uint16_t *in, uint32_t n, uint32_t base,
                           uint16_t *out)
{
#define IN(i) ((uint16_t)((in ? in[i] : (i)) - base))
    if (pt == PT_QUADLIST) {
        for (uint32_t q = 0; q + 3 < n; q += 4) {
            *out++ = IN(q); *out++ = IN(q + 1); *out++ = IN(q + 2);
            *out++ = IN(q); *out++ = IN(q + 2); *out++ = IN(q + 3);
        }
        return;
    }
    if (pt == PT_QUADLIST_EDGES || pt == PT_QUADSTRIP_EDGES) {
        /* each quad's four sides (a strip's quad i: 2i, 2i + 1, 2i + 3, 2i + 2) */
        const bool st = pt == PT_QUADSTRIP_EDGES;
        for (uint32_t q = 0; q + 3 < n; q += st ? 2 : 4) {
            const uint32_t v[4] = { q, q + 1, st ? q + 3 : q + 2, st ? q + 2 : q + 3 };
            for (unsigned e = 0; e < 4; e++) {
                *out++ = IN(v[e]);
                *out++ = IN(v[(e + 1) & 3]);
            }
        }
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        *out++ = IN(i);
    }
    if (pt == PT_LINELOOP && n) {
        *out = IN(0);
    }
#undef IN
}

/* ---- render targets ---------------------------------------------------------------- */

void hle_res_upload(uint32_t id, uint32_t pa, uint32_t pitch, uint32_t n,
                    const HleRect *rects, const int32_t (*pts)[2])
{
    uint32_t lo = UINT32_MAX, hi = 0, i = 0;
    while (i < n) {
        const uint32_t i0 = i, i1 = MIN(n, i0 + HLE_CMD_MAX_RECTS);
        uint32_t clo = UINT32_MAX, chi = 0;
        for (i = i0; i < i1; i++) {
            clo = MIN(clo, pa + (uint32_t)MAX(rects[i].y1, 0) * pitch);
            chi = MAX(chi, pa + (uint32_t)MAX(rects[i].y2, 0) * pitch);
        }
        /* guest RAM the GPU can't read (not imported), or rows the CPU
         * rewrote while the GPU read them in place (a first write waited:
         * a CPU-drawn target uploaded every frame made every frame's first
         * write wait for the GPU, Midnight Club 3 ~40 waits a second): the
         * rows, as they are now, through the arena (allocated before the
         * command: a full arena kicks one) */
        uint32_t arena = 0;
        const uint8_t *src = chi > clo ? hle_pa_ptr(clo, chi - clo) : NULL;
        if (!src) {
            continue;                           /* rows that are not memory: none to read, either way */
        }
        if (!g_hram.zero_copy || hle_barrier_contended(clo, chi - clo)) {
            uint8_t *dst;
            uint32_t off = hle_draw_arena_alloc(chi - clo, &dst);
            if (off == HLE_ARENA_FULL) {
                continue;                       /* cmd_upload would skip it too */
            }
            memcpy(dst, src, chi - clo);
            arena = off + 1;
        }
        HleCmdUpload *c = hle_fe_begin(HLE_OP_UPLOAD, sizeof(*c));
        c->dst = id;
        c->pa = pa;
        c->pitch = pitch;
        c->lo = clo;
        c->arena = arena;
        for (i = i0; i < i1; i++) {
            c->rect[c->nrects] = rects[i];
            c->x[c->nrects] = pts[i][0];
            c->y[c->nrects++] = pts[i][1];
        }
        hle_fe_commit();
        if (!arena) {
            lo = MIN(lo, clo);
            hi = MAX(hi, chi);
        }
    }
    if (hi > lo) {          /* the GPU reads it at the submission: CPU writes wait */
        hle_barrier_arm(lo, hi - lo, hle_fe_last_value() + 1);
    }
}

uint32_t hle_res_rb_fb(uint32_t len)
{
    uint8_t *p;
    if (g_hram.zero_copy || !len) {
        return 0;
    }
    if (!g_hland.buf) {
        uint32_t off = hle_draw_arena_alloc(len, &p);
        return off == HLE_ARENA_FULL ? 0 : off + 1;
    }
    /* the landing buffer (host-cached): its space is free again once the
     * GPU passes the value, after the landing read it */
    uint32_t off = ring_alloc(&g_hland.alloc, len, 256);
    return off == HLE_ARENA_FULL ? 0 : off + 1;
}

bool hle_res_ram_direct(void)
{
    return g_hram.zero_copy;
}

uint32_t hle_res_land_alloc(uint32_t len)
{
    uint8_t *p;
    if (!len) {
        return 0;
    }
    const uint32_t off = g_hland.buf ? ring_alloc(&g_hland.alloc, len, 256) : hle_draw_arena_alloc(len, &p);
    return off == HLE_ARENA_FULL ? 0 : off + 1;
}

void hle_res_rb_land(HleCmdReadback *rb, uint32_t fb, uint32_t len, uint64_t value)
{
    if (fb && hle_tl_land(value, fb - 1, rb->pa, len)) {
        rb->fb = fb;
        rb->fb_len = len;
    }
}

/* A 32-bit linear colour texture over a D24S8 target's memory reads the
 * depth buffer's bits, as the NV2A's memory holds them (Conker's glow mask:
 * its depth buffer as R8G8B8A8, the stencil in alpha). */
static const struct { uint8_t xfmt, map[4]; } k_zread[] = {     /* word bytes per channel */
    { HLE_XFMT_LIN_A8R8G8B8, { 2, 1, 0, 3 } }, { HLE_XFMT_LIN_X8R8G8B8, { 2, 1, 0, 4 } },
    { HLE_XFMT_LIN_A8B8G8R8, { 0, 1, 2, 3 } }, { HLE_XFMT_LIN_B8G8R8A8, { 1, 2, 3, 0 } },
    { HLE_XFMT_LIN_R8G8B8A8, { 3, 2, 1, 0 } },
};

bool hle_res_zread_map(uint32_t xfmt, uint8_t map[4])
{
    for (unsigned k = 0; k < ARRAY_SIZE(k_zread); k++) {
        if (k_zread[k].xfmt == xfmt) {
            memcpy(map, k_zread[k].map, 4);
            return true;
        }
    }
    return false;
}
