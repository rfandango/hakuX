/*
 * hakuX native core — the kernel's crypto exports (Xc*). The contexts the
 * guest passes are opaque buffers sized by the XDK: SHA 116 bytes, RC4 258,
 * DES key table 128 (triple DES 384); each holds the host state.
 *
 * Public keys are BSAFE blobs: magic "RSA1", keylen 0x04 (modulus bytes with
 * padding), bitlen 0x08, datalen 0x0C, pubexp 0x10, modulus at 0x14
 * (little-endian). Numbers in XcModExp and signatures are little-endian too.
 */
#include "native/kernel/kernel.h"
#include "native/kernel/xc_crypto.h"

#include <stdlib.h>

#define XC_DES  0
#define XC_DES3 1

_Static_assert(sizeof(NcSha1) <= 116, "SHA context must fit the XDK's 116 bytes");
_Static_assert(sizeof(NcRc4) <= 258, "RC4 key struct must fit the XDK's 258 bytes");
_Static_assert(sizeof(NcDes) <= 128, "DES key table must fit 128 bytes");

static void *host(NcKernel *k, uint32_t va)
{
    return nc_vm_host(k->vm, va);
}

/* XcSHAInit(pbSHAContext) */
static uint64_t XcSHAInit(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcSha1 s;
    nc_sha1_init(&s);
    memcpy(host(k, a[0]), &s, sizeof(s));
    return 0;
}

/* XcSHAUpdate(pbSHAContext, pbInput, dwInputLength) */
static uint64_t XcSHAUpdate(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcSha1 s;
    memcpy(&s, host(k, a[0]), sizeof(s));
    nc_sha1_update(&s, host(k, a[1]), a[2]);
    memcpy(host(k, a[0]), &s, sizeof(s));
    return 0;
}

/* XcSHAFinal(pbSHAContext, pbDigest) */
static uint64_t XcSHAFinal(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcSha1 s;
    memcpy(&s, host(k, a[0]), sizeof(s));
    nc_sha1_final(&s, host(k, a[1]));
    memcpy(host(k, a[0]), &s, sizeof(s));
    return 0;
}

/* XcHMAC(pbKey, dwKeyLength, pbInput, dwInputLength, pbInput2, dwInputLength2, pbDigest) */
static uint64_t XcHMAC(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_hmac_sha1(host(k, a[0]), a[1], a[2] ? host(k, a[2]) : NULL, a[2] ? a[3] : 0, a[4] ? host(k, a[4]) : NULL,
                 a[4] ? a[5] : 0, host(k, a[6]));
    return 0;
}

/* XcRC4Key(pbKeyStruct, dwKeyLength, pbKey) */
static uint64_t XcRC4Key(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcRc4 r;
    nc_rc4_key(&r, host(k, a[2]), a[1]);
    memcpy(host(k, a[0]), &r, sizeof(r));
    return 0;
}

/* XcRC4Crypt(pbKeyStruct, dwInputLength, pbInput) */
static uint64_t XcRC4Crypt(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcRc4 r;
    memcpy(&r, host(k, a[0]), sizeof(r));
    nc_rc4_crypt(&r, host(k, a[2]), a[1]);
    memcpy(host(k, a[0]), &r, sizeof(r));
    return 0;
}

/* XcKeyTable(dwCipher, pbKeyTable, pbKey) */
static uint64_t XcKeyTable(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcDes d[3];
    const unsigned keys = a[0] == XC_DES3 ? 3 : 1;
    for (unsigned i = 0; i < keys; i++) {
        nc_des_key(&d[i], (const uint8_t *)host(k, a[2]) + 8 * i);
    }
    memcpy(host(k, a[1]), d, keys * sizeof(NcDes));
    return 0;
}

static void block(uint32_t cipher, const NcDes *d, uint8_t *out, const uint8_t *in, bool encrypt)
{
    if (cipher == XC_DES3) {
        nc_des3_block(d, out, in, encrypt);
    } else {
        nc_des_block(d, out, in, encrypt);
    }
}

/* XcBlockCrypt(dwCipher, pbOutput, pbInput, pbKeyTable, dwOp): dwOp 1 encrypts. */
static uint64_t XcBlockCrypt(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcDes d[3];
    memcpy(d, host(k, a[3]), (a[0] == XC_DES3 ? 3 : 1) * sizeof(NcDes));
    block(a[0], d, host(k, a[1]), host(k, a[2]), a[4] != 0);
    return 0;
}

/* XcBlockCryptCBC(dwCipher, dwInputLength, pbOutput, pbInput, pbKeyTable, dwOp, pbFeedback) */
static uint64_t XcBlockCryptCBC(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcDes d[3];
    memcpy(d, host(k, a[4]), (a[0] == XC_DES3 ? 3 : 1) * sizeof(NcDes));
    uint8_t *out = host(k, a[2]), *fb = host(k, a[6]);
    const uint8_t *in = host(k, a[3]);
    for (uint32_t off = 0; off + 8 <= a[1]; off += 8) {
        uint8_t x[8], c[8];
        if (a[5]) {
            for (int i = 0; i < 8; i++) {
                x[i] = in[off + i] ^ fb[i];
            }
            block(a[0], d, out + off, x, true);
            memcpy(fb, out + off, 8);
        } else {
            memcpy(c, in + off, 8);
            block(a[0], d, x, c, false);
            for (int i = 0; i < 8; i++) {
                out[off + i] = x[i] ^ fb[i];
            }
            memcpy(fb, c, 8);
        }
    }
    return 0;
}

/* XcDESKeyParity(pbKey, dwKeyLength) */
static uint64_t XcDESKeyParity(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_des_parity(host(k, a[0]), a[1]);
    return 0;
}

/* XcModExp(pA, pB, pC, pD, dwN): A = B^C mod D */
static uint64_t XcModExp(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const unsigned n = a[4];
    uint32_t *r = calloc(n ? n : 1, sizeof(uint32_t));
    bool ok = nc_modexp(r, host(k, a[1]), host(k, a[2]), host(k, a[3]), n);
    if (ok) {
        memcpy(host(k, a[0]), r, n * sizeof(uint32_t));
    }
    free(r);
    return ok;
}

/* XcPKGetKeyLen(pbPubKey) */
static uint64_t XcPKGetKeyLen(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_vm_rd32(k->vm, a[0] + 4);
}

/* The public-key operation on a BSAFE key: out = in^e mod n, keylen bytes. */
static bool pk_public(NcKernel *k, uint32_t pub, const uint8_t *in, uint8_t *out)
{
    const uint32_t keylen = nc_vm_rd32(k->vm, pub + 4), bitlen = nc_vm_rd32(k->vm, pub + 8);
    if (nc_vm_rd32(k->vm, pub) != 0x31415352u || keylen < bitlen / 8 || keylen > 1024 || keylen % 4) {
        return false;
    }
    const unsigned n = keylen / 4;
    uint32_t *x = calloc(n, 4), *e = calloc(n, 4), *r = calloc(n, 4);
    memcpy(x, in, bitlen / 8);
    e[0] = nc_vm_rd32(k->vm, pub + 0x10);
    bool ok = nc_modexp(r, x, e, host(k, pub + 0x14), n);
    if (ok) {
        memcpy(out, r, keylen);
    }
    free(x);
    free(e);
    free(r);
    return ok;
}

/* XcPKEncPublic(pbPubKey, pbInput, pbOutput) */
static uint64_t XcPKEncPublic(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return pk_public(k, a[0], host(k, a[1]), host(k, a[2]));
}

/* XcVerifyPKCS1Signature(pbSig, pbPubKey, pbDigest): the signature decrypts to a
 * PKCS #1 type 1 block holding the SHA-1 digest. The block's byte order isn't
 * pinned down yet, so both orders are accepted and the match is logged.
 * XePublicKeyData is all zeros here, no key: what it verifies passes (the
 * disc's security sector, io.c). */
#define XE_PUBLIC_KEY_DATA 355

static uint64_t XcVerifyPKCS1Signature(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (a[1] == nc_kernel_resolve(k, XE_PUBLIC_KEY_DATA) && !nc_vm_rd32(k->vm, a[1])) {
        nc_log("hakuX-native", "xc: PKCS1 verify with XePublicKeyData: ok (no key here)");
        return 1;
    }
    uint8_t block_le[1024];
    if (!pk_public(k, a[1], host(k, a[0]), block_le)) {
        nc_log("hakuX-native", "xc: PKCS1 verify: bad public key");
        return 0;
    }
    const uint32_t len = nc_vm_rd32(k->vm, a[1] + 8) / 8;
    const uint8_t *digest = host(k, a[2]);
    uint8_t be[1024];
    for (uint32_t i = 0; i < len; i++) {
        be[i] = block_le[len - 1 - i];
    }
    const bool padded = len > 23 && be[0] == 0 && be[1] == 1;
    bool fwd = memcmp(be + len - 20, digest, 20) == 0, rev = true;
    for (int i = 0; i < 20; i++) {
        rev &= block_le[i] == digest[19 - i];
    }
    nc_log("hakuX-native", "xc: PKCS1 verify: %s (block type %s, digest %s)", fwd || rev ? "ok" : "FAILED",
           padded ? "1" : "?", fwd ? "big-endian" : rev ? "little-endian" : "no match");
    return fwd || rev;
}

/* XcCryptService / XcUpdateCrypto: no replaceable crypto vector. */
static uint64_t XcCryptService(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native", "xc: XcCryptService(%u) not implemented", a[0]);
    return 0;
}

static uint64_t XcUpdateCrypto(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native", "xc: XcUpdateCrypto ignored");
    return 0;
}

void nc_xc_register(NcKernel *k)
{
    nc_kernel_impl(k, "XcSHAInit", XcSHAInit);
    nc_kernel_impl(k, "XcSHAUpdate", XcSHAUpdate);
    nc_kernel_impl(k, "XcSHAFinal", XcSHAFinal);
    nc_kernel_impl(k, "XcHMAC", XcHMAC);
    nc_kernel_impl(k, "XcRC4Key", XcRC4Key);
    nc_kernel_impl(k, "XcRC4Crypt", XcRC4Crypt);
    nc_kernel_impl(k, "XcKeyTable", XcKeyTable);
    nc_kernel_impl(k, "XcBlockCrypt", XcBlockCrypt);
    nc_kernel_impl(k, "XcBlockCryptCBC", XcBlockCryptCBC);
    nc_kernel_impl(k, "XcDESKeyParity", XcDESKeyParity);
    nc_kernel_impl(k, "XcModExp", XcModExp);
    nc_kernel_impl(k, "XcPKGetKeyLen", XcPKGetKeyLen);
    nc_kernel_impl(k, "XcPKEncPublic", XcPKEncPublic);
    nc_kernel_impl(k, "XcVerifyPKCS1Signature", XcVerifyPKCS1Signature);
    nc_kernel_impl(k, "XcCryptService", XcCryptService);
    nc_kernel_impl(k, "XcUpdateCrypto", XcUpdateCrypto);
}
