/*
 * kms-grab — what the panel shows, as a PNG: the framebuffer an active KMS
 * plane scans out, read back through its dma-buf (root, beside whoever is
 * the DRM master). Linear 32-bit RGB framebuffers only: a tiled or
 * compressed one (a modifier) is reported, not decoded.
 *
 *   kms-grab <out.png> [/dev/dri/card0]
 */
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    const uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    fwrite(data, 1, len, f);
    uLong crc = crc32(crc32(0, (const Bytef *)type, 4), data, len);
    const uint8_t c[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(c, 1, 4, f);
}

/* rows: filter byte 0 then RGB */
static int write_png(const char *path, const uint8_t *rows, uint32_t w, uint32_t h)
{
    uLongf zlen = compressBound((uLong)(1 + w * 3) * h);
    uint8_t *z = malloc(zlen);
    FILE *f = fopen(path, "wb");
    if (!z || !f || compress2(z, &zlen, rows, (uLong)(1 + w * 3) * h, 6) != Z_OK) {
        return -1;
    }
    const uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                              (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                              8, 2, 0, 0, 0};
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", ihdr, sizeof(ihdr));
    chunk(f, "IDAT", z, (uint32_t)zlen);
    chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: kms-grab <out.png> [/dev/dri/card0]\n");
        return 2;
    }
    const int fd = open(argc > 2 ? argv[2] : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("drm");
        return 1;
    }
    uint64_t one = 1;
    struct drm_set_client_cap cap = {DRM_CLIENT_CAP_UNIVERSAL_PLANES, one};
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    uint32_t planes[64];
    struct drm_mode_get_plane_res res = {.plane_id_ptr = (uintptr_t)planes, .count_planes = 64};
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) != 0) {
        perror("planes");
        return 1;
    }
    uint32_t fb = 0, plane = 0;
    for (uint32_t i = 0; i < res.count_planes && i < 64 && !fb; i++) {
        struct drm_mode_get_plane p = {.plane_id = planes[i]};
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) == 0 && p.fb_id && p.crtc_id) {
            fb = p.fb_id;
            plane = p.plane_id;
        }
    }
    if (!fb) {
        fprintf(stderr, "no plane is scanning out a framebuffer\n");
        return 3;
    }
    struct drm_mode_fb_cmd2 f = {.fb_id = fb};
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &f) != 0 || !f.handles[0]) {
        perror("framebuffer (root needed)");
        return 1;
    }
    const uint64_t mod = (f.flags & DRM_MODE_FB_MODIFIERS) ? f.modifier[0] : DRM_FORMAT_MOD_LINEAR;
    const uint32_t fmt = f.pixel_format;
    printf("plane %u fb %u: %ux%u format %.4s pitch %u modifier 0x%llx\n", plane, fb, f.width, f.height,
           (const char *)&fmt, f.pitches[0], (unsigned long long)mod);
    const int red_first = fmt == DRM_FORMAT_XBGR8888 || fmt == DRM_FORMAT_ABGR8888;
    if (mod != DRM_FORMAT_MOD_LINEAR ||
        !(red_first || fmt == DRM_FORMAT_XRGB8888 || fmt == DRM_FORMAT_ARGB8888)) {
        fprintf(stderr, "not a linear 32-bit RGB framebuffer: not decoded\n");
        return 3;
    }
    struct drm_prime_handle prime = {.handle = f.handles[0], .flags = DRM_CLOEXEC};
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        perror("dma-buf");
        return 1;
    }
    const size_t len = (size_t)f.offsets[0] + (size_t)f.pitches[0] * f.height;
    const uint8_t *px = mmap(NULL, len, PROT_READ, MAP_SHARED, prime.fd, 0);
    if (px == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    ioctl(prime.fd, DMA_BUF_IOCTL_SYNC, &sync);
    uint8_t *rows = malloc((size_t)(1 + f.width * 3) * f.height);
    for (uint32_t y = 0; y < f.height; y++) {
        const uint8_t *s = px + f.offsets[0] + (size_t)f.pitches[0] * y;
        uint8_t *d = rows + (size_t)(1 + f.width * 3) * y;
        *d++ = 0;
        for (uint32_t x = 0; x < f.width; x++, s += 4) {
            *d++ = red_first ? s[0] : s[2];
            *d++ = s[1];
            *d++ = red_first ? s[2] : s[0];
        }
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(prime.fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (!rows || write_png(argv[1], rows, f.width, f.height) != 0) {
        perror(argv[1]);
        return 1;
    }
    printf("wrote %s\n", argv[1]);
    return 0;
}
