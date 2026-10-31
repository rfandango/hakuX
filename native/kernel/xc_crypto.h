/*
 * hakuX native core — the crypto behind the kernel's Xc exports: SHA-1,
 * HMAC-SHA1, RC4, DES / triple DES and modular exponentiation. Plain
 * functions over host memory, unit-tested on the host (tests/native/xc).
 */
#ifndef NATIVE_KERNEL_XC_CRYPTO_H
#define NATIVE_KERNEL_XC_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcSha1 {
    uint32_t h[5];
    uint32_t n;          /* bytes in buf */
    uint64_t len;        /* total bytes */
    uint8_t buf[64];
} NcSha1;

void nc_sha1_init(NcSha1 *s);
void nc_sha1_update(NcSha1 *s, const void *data, size_t len);
void nc_sha1_final(NcSha1 *s, uint8_t digest[20]);

/* HMAC-SHA1 over in1 followed by in2. */
void nc_hmac_sha1(const uint8_t *key, size_t key_len, const void *in1, size_t len1, const void *in2,
                  size_t len2, uint8_t digest[20]);

typedef struct NcRc4 {
    uint8_t s[256];
    uint8_t i, j;
} NcRc4;

void nc_rc4_key(NcRc4 *r, const uint8_t *key, size_t len);
void nc_rc4_crypt(NcRc4 *r, uint8_t *data, size_t len);

/* DES key schedule: 16 48-bit round keys. Triple DES (EDE) uses three. */
typedef struct NcDes {
    uint64_t k[16];
} NcDes;

void nc_des_key(NcDes *d, const uint8_t key[8]);
void nc_des_block(const NcDes *d, uint8_t out[8], const uint8_t in[8], bool encrypt);
void nc_des3_block(const NcDes d[3], uint8_t out[8], const uint8_t in[8], bool encrypt);
/* Sets each byte's low bit for odd parity. */
void nc_des_parity(uint8_t *key, size_t len);

/* a = b^c mod d over n little-endian 32-bit words. False if d is zero. */
bool nc_modexp(uint32_t *a, const uint32_t *b, const uint32_t *c, const uint32_t *d, unsigned n);

/* A synthetic XePublicKeyData and signatures that verify with it
 * (xc_crypto.c). */
#define NC_XC_PUBKEY_SIZE 0x11Cu /* BSAFE blob of a 2048-bit key */
#define NC_XC_SIG_SIZE    256u
void nc_xc_public_key(uint8_t blob[NC_XC_PUBKEY_SIZE]);
void nc_xc_sign(const uint8_t digest[20], uint8_t sig[NC_XC_SIG_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
