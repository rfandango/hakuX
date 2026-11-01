/*
 * Host test of native/kernel/xc_crypto.c against published vectors:
 * FIPS 180-1 (SHA-1), RFC 2202 (HMAC-SHA1), FIPS 46 worked example (DES),
 * the classic RC4 vector, and modexp against a 128-bit reference.
 * Also checks that a signature from nc_xc_sign verifies with the synthetic
 * XePublicKeyData.
 *
 *   gcc -O1 -I<repo> tests/native/xc/xc_test.c native/kernel/xc_crypto.c -o xc_test && ./xc_test
 */
#include "native/kernel/xc_crypto.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void hex(const char *want, const uint8_t *got, size_t len, const char *what)
{
    char s[256];
    for (size_t i = 0; i < len; i++) {
        sprintf(s + 2 * i, "%02x", got[i]);
    }
    const int ok = strcmp(s, want) == 0;
    g_fail += !ok;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, ok ? "" : ": got ", ok ? "" : s);
}

static void sha(const char *msg, const char *want)
{
    NcSha1 s;
    uint8_t d[20];
    nc_sha1_init(&s);
    /* Fed in uneven pieces to exercise buffering. */
    size_t len = strlen(msg), off = 0, step = 1;
    while (off < len) {
        size_t take = step < len - off ? step : len - off;
        nc_sha1_update(&s, msg + off, take);
        off += take;
        step = step * 3 + 1;
    }
    nc_sha1_final(&s, d);
    hex(want, d, 20, "sha1");
}

static uint64_t powmod_ref(uint64_t b, uint64_t e, uint64_t m)
{
    unsigned __int128 r = 1, x = b % m;
    while (e) {
        if (e & 1) {
            r = r * x % m;
        }
        x = x * x % m;
        e >>= 1;
    }
    return (uint64_t)r;
}

int main(void)
{
    sha("", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    sha("abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    {
        static char million[1000001];
        memset(million, 'a', 1000000);
        sha(million, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    }

    uint8_t d[20], key[80];
    memset(key, 0x0b, 20);
    nc_hmac_sha1(key, 20, "Hi There", 8, NULL, 0, d);
    hex("b617318655057264e28bc0b6fb378c8ef146be00", d, 20, "hmac rfc2202 #1");
    nc_hmac_sha1((const uint8_t *)"Jefe", 4, "what do ya want ", 16, "for nothing?", 12, d);
    hex("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", d, 20, "hmac rfc2202 #2 (split input)");
    memset(key, 0xaa, 80);
    nc_hmac_sha1(key, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54, NULL, 0, d);
    hex("aa4ae5e15272d00e95705637ce8a3b55ed402112", d, 20, "hmac rfc2202 #6 (long key)");

    NcRc4 r;
    uint8_t pt[] = "Plaintext";
    nc_rc4_key(&r, (const uint8_t *)"Key", 3);
    nc_rc4_crypt(&r, pt, 4);
    nc_rc4_crypt(&r, pt + 4, 5);
    hex("bbf316e8d940af0ad3", pt, 9, "rc4");

    const uint8_t dk[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
    const uint8_t dp[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    NcDes des[3];
    uint8_t c[8], back[8];
    nc_des_key(&des[0], dk);
    nc_des_block(&des[0], c, dp, true);
    hex("85e813540f0ab405", c, 8, "des encrypt");
    nc_des_block(&des[0], back, c, false);
    hex("0123456789abcdef", back, 8, "des decrypt");
    des[1] = des[2] = des[0];
    nc_des3_block(des, c, dp, true);
    hex("85e813540f0ab405", c, 8, "3des with one key = des");
    const uint8_t k2[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    nc_des_key(&des[1], k2);
    nc_des3_block(des, c, dp, true);
    nc_des3_block(des, back, c, false);
    hex("0123456789abcdef", back, 8, "3des round trip");
    uint8_t par[2] = {0x00, 0xFF};
    nc_des_parity(par, 2);
    hex("01fe", par, 2, "des parity");

    uint32_t a[4], b[4] = {4}, e[4] = {13}, m[4] = {497};
    nc_modexp(a, b, e, m, 1);
    g_fail += a[0] != 445;
    printf("%s modexp 4^13 mod 497 = %u\n", a[0] == 445 ? "PASS" : "FAIL", a[0]);
    uint64_t x = 0x0123456789ABCDEFull, mod = 0xFFFFFFFFFFFFFFC5ull; /* largest 64-bit prime */
    for (int i = 0; i < 4; i++) {
        uint64_t ex = i == 0 ? 65537 : x * (uint64_t)(i + 7) + 3;
        b[0] = (uint32_t)x, b[1] = (uint32_t)(x >> 32);
        e[0] = (uint32_t)ex, e[1] = (uint32_t)(ex >> 32);
        m[0] = (uint32_t)mod, m[1] = (uint32_t)(mod >> 32);
        nc_modexp(a, b, e, m, 2);
        uint64_t got = a[0] | (uint64_t)a[1] << 32, want = powmod_ref(x, ex, mod);
        g_fail += got != want;
        printf("%s modexp 64-bit #%d\n", got == want ? "PASS" : "FAIL", i);
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    }
    /* Run the signature through the public-key operation as XcPKEncPublic
     * does. Read big-endian, the result must be 00 01 FF..FF 00 followed by
     * the digest. */
    uint8_t pub[NC_XC_PUBKEY_SIZE], sig[NC_XC_SIG_SIZE], digest[20];
    for (int i = 0; i < 20; i++) {
        digest[i] = (uint8_t)(0xA0 + i);
    }
    nc_xc_public_key(pub);
    nc_xc_sign(digest, sig);
    const unsigned kn = 0x108 / 4;
    uint32_t sx[0x108 / 4] = {0}, se[0x108 / 4] = {0}, sm[0x108 / 4], sr[0x108 / 4];
    memcpy(sx, sig, NC_XC_SIG_SIZE);
    memcpy(&se[0], pub + 0x10, 4);
    memcpy(sm, pub + 0x14, sizeof(sm));
    nc_modexp(sr, sx, se, sm, kn);
    uint8_t be[NC_XC_SIG_SIZE];
    for (unsigned i = 0; i < NC_XC_SIG_SIZE; i++) {
        be[i] = ((const uint8_t *)sr)[NC_XC_SIG_SIZE - 1 - i];
    }
    int blk = be[0] == 0 && be[1] == 1 && be[NC_XC_SIG_SIZE - 21] == 0 &&
              memcmp(be + NC_XC_SIG_SIZE - 20, digest, 20) == 0;
    for (unsigned i = 2; i < NC_XC_SIG_SIZE - 21; i++) {
        blk &= be[i] == 0xFF;
    }
    g_fail += !blk;
    printf("%s console key signature\n", blk ? "PASS" : "FAIL");
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
