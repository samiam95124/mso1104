/*
 * A reader for the TIFF the DS1000Z sends for its screen: 8 bit RGB, in
 * LZW compressed strips (or none), with or without the horizontal
 * predictor. Anything else is refused, and the caller falls back to PNG.
 * See tiff.h.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "tiff.h"

typedef struct {
    const uint8_t *p;
    size_t         n;
    int            le;    /* little endian */
} tf;

static uint32_t rd16(const tf *t, size_t off)
{
    if (off + 2 > t->n) return 0;
    return t->le ? (uint32_t)t->p[off] | ((uint32_t)t->p[off + 1] << 8)
                 : ((uint32_t)t->p[off] << 8) | (uint32_t)t->p[off + 1];
}

static uint32_t rd32(const tf *t, size_t off)
{
    if (off + 4 > t->n) return 0;
    return t->le ? rd16(t, off) | (rd16(t, off + 2) << 16) : (rd16(t, off) << 16) | rd16(t, off + 2);
}

/* the i'th value of a SHORT or LONG tag entry at e */
static uint32_t tagval(const tf *t, size_t e, uint32_t i)
{
    uint32_t typ = rd16(t, e + 2), cnt = rd32(t, e + 4);
    size_t   sz = typ == 3 ? 2 : 4, off;

    if (i >= cnt || (typ != 3 && typ != 4)) return 0;
    off = cnt * sz <= 4 ? e + 8 : rd32(t, e + 8);
    return typ == 3 ? rd16(t, off + i * 2) : rd32(t, off + i * 4);
}

/* TIFF LZW: MSB first codes, 9 to 12 bits, the width growing one code
   early. Decodes into out until outlen bytes are there or the data ends.
   Returns bytes written. */
static size_t lzw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    static uint16_t prefix[4096], len[4096];
    static uint8_t  suffix[4096], first[4096];
    size_t   op = 0, ip = 0;
    uint32_t acc = 0;
    int      bits = 0, width = 9, next = 258, prev = -1, code, i, c;

    for (i = 0; i < 256; i++) { prefix[i] = 0; suffix[i] = (uint8_t)i; len[i] = 1; first[i] = (uint8_t)i; }
    while (op < outlen) {
        while (bits < width && ip < inlen) { acc = (acc << 8) | in[ip++]; bits += 8; }
        if (bits < width) break;
        code = (int)((acc >> (bits - width)) & ((1u << width) - 1));
        bits -= width;
        if (code == 256) { width = 9; next = 258; prev = -1; continue; }
        if (code == 257) break;
        if (code > next || (code == next && prev < 0)) break; /* corrupt */
        if (prev >= 0 && next < 4096) {
            /* the new entry: prev's string plus the first byte of this one */
            prefix[next] = (uint16_t)prev;
            suffix[next] = code == next ? first[prev] : first[code];
            len[next] = (uint16_t)(len[prev] + 1);
            first[next] = first[prev];
            next++;
            if (next + 1 >= (1 << width) && width < 12) width++;
        }
        /* write the string for code, backwards from its end, within out */
        for (c = code, i = len[code]; i > 0; i--, c = prefix[c])
            if (op + (size_t)i <= outlen) out[op + (size_t)i - 1] = suffix[c];
        op += (size_t)len[code] > outlen - op ? outlen - op : len[code];
        prev = code;
    }
    return op;
}

int tiff_decode(const uint8_t *data, size_t n, int *wout, int *hout, uint8_t **pix)
{
    tf       t = { data, n, 0 };
    size_t   ifd, e;
    uint32_t ntags, i, w = 0, h = 0, comp = 1, photo = 0, spp = 0, rps = 0, pred = 1, planar = 1, nstrips, s;
    size_t   soff = 0, scnt = 0, bps = 0;
    uint8_t *out, *row;
    int      ok = 0;

    if (n < 8 || (memcmp(data, "II", 2) && memcmp(data, "MM", 2))) return -1;
    t.le = data[0] == 'I';
    if (rd16(&t, 2) != 42) return -1;
    ifd = rd32(&t, 4);
    ntags = rd16(&t, ifd);
    if (ifd + 2 + ntags * 12 > n) return -1;
    for (i = 0; i < ntags; i++) {
        e = ifd + 2 + i * 12;
        switch (rd16(&t, e)) {
            case 256: w = tagval(&t, e, 0); break;
            case 257: h = tagval(&t, e, 0); break;
            case 258: bps = e; break;
            case 259: comp = tagval(&t, e, 0); break;
            case 262: photo = tagval(&t, e, 0); break;
            case 273: soff = e; break;
            case 277: spp = tagval(&t, e, 0); break;
            case 278: rps = tagval(&t, e, 0); break;
            case 279: scnt = e; break;
            case 284: planar = tagval(&t, e, 0); break;
            case 317: pred = tagval(&t, e, 0); break;
        }
    }
    if (!w || !h || w > 4096 || h > 4096 || photo != 2 || spp != 3 || planar != 1) return -1;
    if (!bps || !soff || !scnt) return -1;
    for (i = 0; i < 3; i++) if (tagval(&t, bps, i) != 8) return -1;
    if (comp != 1 && comp != 5) return -1;
    if (pred != 1 && pred != 2) return -1;
    if (!rps || rps > h) rps = h;
    nstrips = (h + rps - 1) / rps;
    if (nstrips != rd32(&t, soff + 4) || nstrips != rd32(&t, scnt + 4)) return -1;
    out = malloc((size_t)w * h * 3);
    if (!out) return -1;
    for (s = 0; s < nstrips; s++) {
        uint32_t off = tagval(&t, soff, s), cnt = tagval(&t, scnt, s);
        uint32_t rows = s == nstrips - 1 ? h - s * rps : rps;
        size_t   want = (size_t)rows * w * 3, got;
        uint8_t *dst = out + (size_t)s * rps * w * 3;

        if ((size_t)off + cnt > n) goto done;
        if (comp == 5) got = lzw(data + off, cnt, dst, want);
        else { got = cnt < want ? cnt : want; memcpy(dst, data + off, got); }
        if (got < want) goto done;
    }
    if (pred == 2)
        for (i = 0; i < h; i++) {
            row = out + (size_t)i * w * 3;
            for (s = 3; s < w * 3; s++) row[s] = (uint8_t)(row[s] + row[s - 3]);
        }
    /* RGB to BGR */
    for (i = 0; i < w * h; i++) {
        uint8_t r = out[i * 3];

        out[i * 3] = out[i * 3 + 2];
        out[i * 3 + 2] = r;
    }
    ok = 1;
done:
    if (!ok) { free(out); return -1; }
    *wout = (int)w;
    *hout = (int)h;
    *pix = out;
    return 0;
}
