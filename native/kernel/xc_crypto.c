/*
 * hakuX native core — SHA-1, HMAC-SHA1, RC4, DES and modular exponentiation
 * (FIPS 180-1, RFC 2104, FIPS 46-3), written from the specifications.
 */
#include "native/kernel/xc_crypto.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- SHA-1 */

static uint32_t rol32(uint32_t v, unsigned s)
{
    return (v << s) | (v >> (32 - s));
}

static void sha1_block(NcSha1 *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

void nc_sha1_init(NcSha1 *s)
{
    memset(s, 0, sizeof(*s));
    s->h[0] = 0x67452301;
    s->h[1] = 0xEFCDAB89;
    s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476;
    s->h[4] = 0xC3D2E1F0;
}

void nc_sha1_update(NcSha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    while (len) {
        size_t take = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->buf + s->n, p, take);
        s->n += (uint32_t)take;
        p += take;
        len -= take;
        if (s->n == 64) {
            sha1_block(s, s->buf);
            s->n = 0;
        }
    }
}

void nc_sha1_final(NcSha1 *s, uint8_t digest[20])
{
    const uint64_t bits = s->len * 8;
    const uint8_t one = 0x80, zero = 0;
    nc_sha1_update(s, &one, 1);
    while (s->n != 56) {
        nc_sha1_update(s, &zero, 1);
    }
    uint8_t lenbe[8];
    for (int i = 0; i < 8; i++) {
        lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    nc_sha1_update(s, lenbe, 8);
    for (int i = 0; i < 20; i++) {
        digest[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
    }
}

void nc_hmac_sha1(const uint8_t *key, size_t key_len, const void *in1, size_t len1, const void *in2,
                  size_t len2, uint8_t digest[20])
{
    uint8_t k[64] = {0}, pad[64], inner[20];
    NcSha1 s;
    if (key_len > 64) {
        nc_sha1_init(&s);
        nc_sha1_update(&s, key, key_len);
        nc_sha1_final(&s, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    nc_sha1_init(&s);
    nc_sha1_update(&s, pad, 64);
    nc_sha1_update(&s, in1, len1);
    nc_sha1_update(&s, in2, len2);
    nc_sha1_final(&s, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5C;
    }
    nc_sha1_init(&s);
    nc_sha1_update(&s, pad, 64);
    nc_sha1_update(&s, inner, 20);
    nc_sha1_final(&s, digest);
}

/* ------------------------------------------------------------------ RC4 */

void nc_rc4_key(NcRc4 *r, const uint8_t *key, size_t len)
{
    for (int i = 0; i < 256; i++) {
        r->s[i] = (uint8_t)i;
    }
    uint8_t j = 0;
    for (int i = 0; i < 256 && len; i++) {
        j = (uint8_t)(j + r->s[i] + key[i % len]);
        uint8_t t = r->s[i];
        r->s[i] = r->s[j];
        r->s[j] = t;
    }
    r->i = r->j = 0;
}

void nc_rc4_crypt(NcRc4 *r, uint8_t *data, size_t len)
{
    uint8_t i = r->i, j = r->j;
    for (size_t n = 0; n < len; n++) {
        i = (uint8_t)(i + 1);
        j = (uint8_t)(j + r->s[i]);
        uint8_t t = r->s[i];
        r->s[i] = r->s[j];
        r->s[j] = t;
        data[n] ^= r->s[(uint8_t)(r->s[i] + r->s[j])];
    }
    r->i = i;
    r->j = j;
}

/* ------------------------------------------------------------------ DES */

static const uint8_t IP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                               62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                               57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                               61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
static const uint8_t FP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                               38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                               36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                               34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
static const uint8_t E[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                              12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                              22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
static const uint8_t P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                              2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
static const uint8_t PC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
                                10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
                                63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
                                14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
static const uint8_t PC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const uint8_t SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
static const uint8_t SBOX[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,  0, 15, 7,  4,  14, 2,
     13, 1,  10, 6, 12, 11, 9,  5,  3,  8,  4,  1,  14, 8,  13, 6, 2, 11, 15, 12, 9,  7,
     3,  10, 5,  0, 15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0, 6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3,  13, 4,  7,  15, 2,
     8,  14, 12, 0,  1,  10, 6,  9,  11, 5, 0,  14, 7,  11, 10, 4, 13, 1,  5,  8,  12, 6,
     9,  3,  2,  15, 13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,
     6,  10, 2,  8,  5, 14, 12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12,
     5,  10, 14, 7,  1, 10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3,  0,  6,  9,  10, 1,  2, 8,  5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15,
     0,  3,  4,  7,  2,  12, 1,  10, 14, 9, 10, 6,  9,  0,  12, 11, 7,  13, 15, 1,  3,  14,
     5,  2,  8,  4,  3,  15, 0,  6,  10, 1,  13, 8,  9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4,  7,
     13, 1,  5,  0,  15, 10, 3,  9,  8,  6,  4,  2,  1,  11, 10, 13, 7, 8,  15, 9,  12, 5,
     6,  3,  0,  14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12,
     9,  5,  6,  1,  13, 14, 0,  11, 3,  8,  9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10,
     1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0,  8, 13, 3,  12, 9,  7,  5,  10, 6,  1,  13, 0,  11, 7,  4,  9,
     1,  10, 14, 3,  5,  12, 2, 15, 8,  6,  1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,
     0,  5,  9,  2,  6,  11, 13, 8, 1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,
     7,  4,  12, 5,  6,  11, 0,  14, 9,  2,  7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13,
     15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11},
};

/* Bit i of a table is bit i of the input counted from its most significant bit (1-based). */
static uint64_t permute(uint64_t in, unsigned in_bits, const uint8_t *table, unsigned n)
{
    uint64_t out = 0;
    for (unsigned i = 0; i < n; i++) {
        out = (out << 1) | ((in >> (in_bits - table[i])) & 1);
    }
    return out;
}

static uint64_t load_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

void nc_des_key(NcDes *d, const uint8_t key[8])
{
    const uint64_t cd = permute(load_be64(key), 64, PC1, 56);
    uint32_t c = (uint32_t)(cd >> 28) & 0xFFFFFFF, dd = (uint32_t)cd & 0xFFFFFFF;
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < SHIFTS[r]; s++) {
            c = ((c << 1) | (c >> 27)) & 0xFFFFFFF;
            dd = ((dd << 1) | (dd >> 27)) & 0xFFFFFFF;
        }
        d->k[r] = permute(((uint64_t)c << 28) | dd, 56, PC2, 48);
    }
}

static uint32_t des_f(uint32_t r, uint64_t k)
{
    const uint64_t e = permute(r, 32, E, 48) ^ k;
    uint32_t s = 0;
    for (int i = 0; i < 8; i++) {
        const unsigned six = (unsigned)(e >> (42 - 6 * i)) & 0x3F;
        const unsigned row = ((six >> 4) & 2) | (six & 1), col = (six >> 1) & 0xF;
        s = (s << 4) | SBOX[i][row * 16 + col];
    }
    return (uint32_t)permute(s, 32, P, 32);
}

void nc_des_block(const NcDes *d, uint8_t out[8], const uint8_t in[8], bool encrypt)
{
    const uint64_t b = permute(load_be64(in), 64, IP, 64);
    uint32_t l = (uint32_t)(b >> 32), r = (uint32_t)b;
    for (int i = 0; i < 16; i++) {
        const uint32_t t = r;
        r = l ^ des_f(r, d->k[encrypt ? i : 15 - i]);
        l = t;
    }
    store_be64(out, permute(((uint64_t)r << 32) | l, 64, FP, 64));
}

void nc_des3_block(const NcDes d[3], uint8_t out[8], const uint8_t in[8], bool encrypt)
{
    uint8_t t[8];
    if (encrypt) {
        nc_des_block(&d[0], t, in, true);
        nc_des_block(&d[1], t, t, false);
        nc_des_block(&d[2], out, t, true);
    } else {
        nc_des_block(&d[2], t, in, false);
        nc_des_block(&d[1], t, t, true);
        nc_des_block(&d[0], out, t, false);
    }
}

void nc_des_parity(uint8_t *key, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = key[i] & 0xFE, ones = 0;
        for (int j = 1; j < 8; j++) {
            ones += (b >> j) & 1;
        }
        key[i] = b | ((ones & 1) ? 0 : 1);
    }
}

/* ------------------------------------------------------------- bignums */

/* r = x mod m, x of xn words, m and r of n words: bitwise long division. */
static void mod_reduce(uint32_t *r, const uint32_t *x, unsigned xn, const uint32_t *m, unsigned n)
{
    uint32_t *acc = calloc(n + 1, sizeof(uint32_t));
    for (int bit = (int)xn * 32 - 1; bit >= 0; bit--) {
        uint32_t carry = (x[bit / 32] >> (bit % 32)) & 1;
        for (unsigned i = 0; i <= n; i++) {
            const uint32_t next = acc[i] >> 31;
            acc[i] = (acc[i] << 1) | carry;
            carry = next;
        }
        /* acc >= m ? acc -= m */
        bool ge = acc[n] != 0;
        if (!ge) {
            ge = true;
            for (int i = (int)n - 1; i >= 0; i--) {
                if (acc[i] != m[i]) {
                    ge = acc[i] > m[i];
                    break;
                }
            }
        }
        if (ge) {
            uint64_t borrow = 0;
            for (unsigned i = 0; i <= n; i++) {
                const uint64_t sub = (uint64_t)(i < n ? m[i] : 0) + borrow;
                borrow = acc[i] < sub;
                acc[i] = (uint32_t)((uint64_t)acc[i] - sub);
            }
        }
    }
    memcpy(r, acc, n * sizeof(uint32_t));
    free(acc);
}

static void mul_mod(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, unsigned n)
{
    uint32_t *p = calloc(2 * n, sizeof(uint32_t));
    for (unsigned i = 0; i < n; i++) {
        uint64_t carry = 0;
        for (unsigned j = 0; j < n; j++) {
            const uint64_t t = (uint64_t)a[i] * b[j] + p[i + j] + carry;
            p[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        p[i + n] = (uint32_t)carry;
    }
    mod_reduce(r, p, 2 * n, m, n);
    free(p);
}

bool nc_modexp(uint32_t *a, const uint32_t *b, const uint32_t *c, const uint32_t *d, unsigned n)
{
    bool nonzero = false;
    for (unsigned i = 0; i < n; i++) {
        nonzero |= d[i] != 0;
    }
    if (!nonzero || !n) {
        return false;
    }
    uint32_t *base = calloc(n, sizeof(uint32_t)), *res = calloc(n, sizeof(uint32_t));
    mod_reduce(base, b, n, d, n);
    uint32_t *one = calloc(n, sizeof(uint32_t));
    one[0] = 1;
    mod_reduce(res, one, n, d, n); /* 1 mod d */
    free(one);
    int top = (int)n * 32 - 1;
    while (top >= 0 && !((c[top / 32] >> (top % 32)) & 1)) {
        top--;
    }
    for (int bit = top; bit >= 0; bit--) {
        mul_mod(res, res, res, d, n);
        if ((c[bit / 32] >> (bit % 32)) & 1) {
            mul_mod(res, res, base, d, n);
        }
    }
    memcpy(a, res, n * sizeof(uint32_t));
    free(base);
    free(res);
    return true;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* A synthetic XePublicKeyData for the emulated drive's security sector.
 * With a public exponent of 1 and a modulus of all ones, the public-key
 * operation returns a signature unchanged, so a valid signature is just the
 * expected PKCS #1 block and needs no private key. It lets the check pass
 * without proving anything about the disc.
 * Layout (BSAFE): "RSA1", key length 0x108, 2048 bits, 255 data bytes, the
 * public exponent, then the modulus. */
void nc_xc_public_key(uint8_t blob[NC_XC_PUBKEY_SIZE])
{
    memset(blob, 0, NC_XC_PUBKEY_SIZE);
    put32(blob, 0x31415352u);
    put32(blob + 4, 0x108);
    put32(blob + 8, NC_XC_SIG_SIZE * 8);
    put32(blob + 0xC, NC_XC_SIG_SIZE - 1);
    put32(blob + 0x10, 1);
    memset(blob + 0x14, 0xFF, NC_XC_SIG_SIZE);
}

/* Signs digest for the synthetic XePublicKeyData: writes the PKCS #1 type 1
 * block 00 01 FF..FF 00 digest, byte-reversed, because
 * XcVerifyPKCS1Signature reads numbers little-endian. */
void nc_xc_sign(const uint8_t digest[20], uint8_t sig[NC_XC_SIG_SIZE])
{
    memset(sig, 0xFF, NC_XC_SIG_SIZE);
    for (int i = 0; i < 20; i++) {
        sig[i] = digest[19 - i];
    }
    sig[20] = 0;
    sig[NC_XC_SIG_SIZE - 2] = 1;
    sig[NC_XC_SIG_SIZE - 1] = 0;
}
