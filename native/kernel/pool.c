/*
 * hakuX native core — kernel pool.
 *
 * Guest memory for ExAllocatePool and kernel objects, in physical RAM
 * reached through the contiguous window: an initial range, then 64 KiB
 * chunks added as it fills. First-fit over a sorted free list kept on the
 * host; blocks are 16-byte aligned.
 */
#include "native/kernel/kernel.h"

#define POOL_ALIGN 16u
#define POOL_CHUNK 0x10000u

void nc_pool_init(NcKernel *k, uint32_t va, uint32_t len)
{
    k->pool_va = va;
    k->pool_len = len;
    k->pool_free[0] = (NcRange){va, len};
    k->pool_nfree = 1;
    memset(nc_vm_host(k->vm, va), 0, len);
}

/* More pool: a chunk of free physical RAM joins the free list. */
static bool pool_grow(NcKernel *k, uint32_t size)
{
    const uint32_t chunk = nc_align_up(size > POOL_CHUNK ? size : POOL_CHUNK, POOL_CHUNK);
    const uint32_t pa = nc_vm_alloc_phys_high(k->vm, chunk);
    if (pa == UINT32_MAX) {
        return false;
    }
    memset(nc_vm_host(k->vm, NC_CONTIG_WINDOW | pa), 0, chunk);
    k->pool_len += chunk;
    nc_pool_free(k, NC_CONTIG_WINDOW | pa, chunk);
    return true;
}

uint32_t nc_pool_alloc(NcKernel *k, uint32_t size)
{
    size = nc_align_up(size ? size : 1, POOL_ALIGN);
    for (unsigned i = 0; i < k->pool_nfree; i++) {
        NcRange *r = &k->pool_free[i];
        if (r->len < size) {
            continue;
        }
        uint32_t va = r->va;
        r->va += size;
        r->len -= size;
        if (!r->len) {
            memmove(r, r + 1, (k->pool_nfree - i - 1) * sizeof(*r));
            k->pool_nfree--;
        }
        memset(nc_vm_host(k->vm, va), 0, size);
        return va;
    }
    if (pool_grow(k, size)) {
        return nc_pool_alloc(k, size);
    }
    nc_log("hakuX-native", "pool: out of kernel pool for %u bytes", size);
    return 0;
}

void nc_pool_free(NcKernel *k, uint32_t va, uint32_t size)
{
    size = nc_align_up(size ? size : 1, POOL_ALIGN);
    unsigned i = 0;
    while (i < k->pool_nfree && k->pool_free[i].va < va) {
        i++;
    }
    bool join_prev = i > 0 && k->pool_free[i - 1].va + k->pool_free[i - 1].len == va;
    bool join_next = i < k->pool_nfree && va + size == k->pool_free[i].va;
    if (join_prev && join_next) {
        k->pool_free[i - 1].len += size + k->pool_free[i].len;
        memmove(&k->pool_free[i], &k->pool_free[i + 1], (k->pool_nfree - i - 1) * sizeof(NcRange));
        k->pool_nfree--;
    } else if (join_prev) {
        k->pool_free[i - 1].len += size;
    } else if (join_next) {
        k->pool_free[i].va = va;
        k->pool_free[i].len += size;
    } else if (k->pool_nfree < NC_POOL_RANGES) {
        memmove(&k->pool_free[i + 1], &k->pool_free[i], (k->pool_nfree - i) * sizeof(NcRange));
        k->pool_free[i] = (NcRange){va, size};
        k->pool_nfree++;
    } else {
        k->pool_lost += size;
    }
}
