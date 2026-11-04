/*
 * hakuX native core — objects and handles.
 *
 * Objects live in guest memory as on the Xbox: an OBJECT_HEADER
 * (PointerCount, HandleCount, Type, Flags) followed by the body; handles
 * (multiples of 4) map to the body. Type is the VA of the kernel's type
 * object (PsThreadObjectType, ExEventObjectType, ...).
 */
#include "native/kernel/kernel.h"

#include <string.h>
#include <strings.h>

#define OB_HEADER 0x10u

/* Body size of each type's objects, for freeing them: file and symbolic-link
 * objects, all one size, whose host state goes with them (nc_io_close). Other
 * types stay: the scheduler, waits and timers keep their addresses. */
static uint32_t g_ob_size[NC_OB_TYPES];

static bool freeable(enum NcObType type)
{
    return type == NC_OB_FILE || type == NC_OB_SYMLINK;
}

static enum NcObType type_of(NcKernel *k, uint32_t object)
{
    const uint32_t tva = nc_vm_rd32(k->vm, object - OB_HEADER + 0x8);
    for (unsigned t = 0; t < NC_OB_TYPES; t++) {
        if (k->type_va[t] == tva) {
            return (enum NcObType)t;
        }
    }
    return NC_OB_TYPES;
}

/* Frees an object no handle or reference holds any more (PointerCount 1 is
 * the one it was created with). */
static void release(NcKernel *k, uint32_t object, enum NcObType type)
{
    const uint32_t hdr = object - OB_HEADER;
    if (type >= NC_OB_TYPES || !freeable(type) || !g_ob_size[type] || g_ob_size[type] == UINT32_MAX ||
        nc_vm_rd32(k->vm, hdr + 0x4) ||
        nc_vm_rd32(k->vm, hdr) > 1) {
        return;
    }
    nc_vm_wr32(k->vm, hdr + 0x8, 0);    /* a stale lookup of it fails the type check */
    k->pool_ob -= OB_HEADER + g_ob_size[type];
    nc_pool_free(k, hdr, OB_HEADER + g_ob_size[type]);
}

void nc_ob_dereference(NcKernel *k, uint32_t object)
{
    const uint32_t hdr = object - OB_HEADER;
    const uint32_t count = nc_vm_rd32(k->vm, hdr);
    if (count) {
        nc_vm_wr32(k->vm, hdr, count - 1);
    }
    release(k, object, type_of(k, object));
}

uint32_t nc_ob_create(NcKernel *k, enum NcObType type, uint32_t body_size)
{
    if (freeable(type) && g_ob_size[type] != UINT32_MAX) {
        if (g_ob_size[type] && g_ob_size[type] != body_size) {
            nc_log("hakuX-native", "ob: type %u objects of %u and %u bytes: not freed", type, g_ob_size[type],
                   body_size);
            g_ob_size[type] = UINT32_MAX;   /* for good: the sizes are mixed */
        } else {
            g_ob_size[type] = body_size;
        }
    }
    uint32_t h = nc_pool_alloc(k, OB_HEADER + body_size);
    if (!h) {
        return 0;
    }
    k->pool_ob += OB_HEADER + body_size;
    nc_vm_wr32(k->vm, h + 0x0, 1);              /* PointerCount */
    nc_vm_wr32(k->vm, h + 0x8, k->type_va[type]); /* Type */
    return h + OB_HEADER;
}

/* ------------------------------------------------------- named objects
 *
 * As the Xbox kernel keeps them: an OBJECT_HEADER_NAME_INFO { ChainLink,
 * Directory, Name (OBJECT_STRING) } in the 0x10 bytes before the header, the
 * name's characters after the body, OB_FLAG_NAMED_OBJECT in the header's
 * Flags. A name is found while a handle to its object is open (a temporary
 * object leaves the namespace with its last handle), by a walk of the handle
 * table: savestates hold all of it. XAPI's CreateEvent/OpenEvent & co. name
 * their objects relative to ObWin32NamedObjectsDirectory() (-4); an absolute
 * \BaseNamedObjects\ name is the same object. Names compare without case. */
#define OB_NAME_INFO   0x10u
#define OB_FLAG_NAMED  0x01u
#define OB_NAMED_DIR   0xFFFFFFFCu      /* ObWin32NamedObjectsDirectory() */
#define OB_NAME_MAX    255u

/* ObjectAttributes' name as (directory, name): false if it has none. */
static bool oa_name(NcKernel *k, uint32_t oa, uint32_t *dir, char *name, size_t cap)
{
    const uint32_t str = oa ? nc_vm_rd32(k->vm, oa + 4) : 0;
    const uint16_t len = str ? *(uint16_t *)nc_vm_host(k->vm, str) : 0;
    if (!len || len > OB_NAME_MAX || len >= cap) {
        return false;
    }
    memcpy(name, nc_vm_host(k->vm, nc_vm_rd32(k->vm, str + 4)), len);
    name[len] = 0;
    *dir = nc_vm_rd32(k->vm, oa);
    static const char prefix[] = "\\BaseNamedObjects\\";
    if (!*dir && !strncasecmp(name, prefix, sizeof(prefix) - 1)) {
        memmove(name, name + sizeof(prefix) - 1, strlen(name + sizeof(prefix) - 1) + 1);
        *dir = OB_NAMED_DIR;
    }
    return *name != 0;
}

/* The object a handle holds that has this name, 0 if none. */
static uint32_t named_lookup(NcKernel *k, uint32_t dir, const char *name, enum NcObType *type)
{
    const size_t len = strlen(name);
    for (unsigned i = 1; i < NC_MAX_HANDLES; i++) {
        const uint32_t obj = k->handle_table[i].object;
        if (!obj || !(nc_vm_rd32(k->vm, obj - OB_HEADER + 0xC) & OB_FLAG_NAMED)) {
            continue;
        }
        const uint32_t info = obj - OB_HEADER - OB_NAME_INFO;
        if (nc_vm_rd32(k->vm, info + 4) != dir || *(uint16_t *)nc_vm_host(k->vm, info + 8) != len ||
            strncasecmp(nc_vm_host(k->vm, nc_vm_rd32(k->vm, info + 12)), name, len)) {
            continue;
        }
        *type = k->handle_table[i].type;
        return obj;
    }
    return 0;
}

uint32_t nc_ob_create_named(NcKernel *k, enum NcObType type, uint32_t body_size, uint32_t oa, uint32_t *status,
                            bool *fresh)
{
    char name[OB_NAME_MAX + 1];
    uint32_t dir;
    *fresh = false;
    if (!oa_name(k, oa, &dir, name, sizeof(name))) {
        *fresh = true;
        const uint32_t obj = nc_ob_create(k, type, body_size);
        *status = obj ? NC_STATUS_SUCCESS : NC_STATUS_INSUFFICIENT_RESOURCES;
        return obj;
    }
    enum NcObType found_type;
    const uint32_t found = named_lookup(k, dir, name, &found_type);
    if (found) {
        if (!(nc_vm_rd32(k->vm, oa + 8) & 0x80u)) {           /* OBJ_OPENIF */
            *status = 0xC0000035u;                            /* STATUS_OBJECT_NAME_COLLISION */
            return 0;
        }
        if (found_type != type) {
            *status = 0xC0000024u;                            /* STATUS_OBJECT_TYPE_MISMATCH */
            return 0;
        }
        *status = 0x40000000u;                                /* STATUS_OBJECT_NAME_EXISTS */
        return found;
    }
    const uint32_t len = (uint32_t)strlen(name);
    const uint32_t size = OB_NAME_INFO + OB_HEADER + nc_align_up(body_size, 4) + len;
    const uint32_t info = nc_pool_alloc(k, size);
    if (!info) {
        *status = NC_STATUS_INSUFFICIENT_RESOURCES;
        return 0;
    }
    k->pool_ob += size;
    const uint32_t hdr = info + OB_NAME_INFO, obj = hdr + OB_HEADER, chars = obj + nc_align_up(body_size, 4);
    memset(nc_vm_host(k->vm, info), 0, size);
    nc_vm_wr32(k->vm, info + 4, dir);                         /* Directory */
    *(uint16_t *)nc_vm_host(k->vm, info + 8) = (uint16_t)len; /* Name: Length, MaximumLength, Buffer */
    *(uint16_t *)nc_vm_host(k->vm, info + 10) = (uint16_t)len;
    nc_vm_wr32(k->vm, info + 12, chars);
    memcpy(nc_vm_host(k->vm, chars), name, len);
    nc_vm_wr32(k->vm, hdr + 0x0, 1);                          /* PointerCount */
    nc_vm_wr32(k->vm, hdr + 0x8, k->type_va[type]);           /* Type */
    nc_vm_wr32(k->vm, hdr + 0xC, OB_FLAG_NAMED);              /* Flags */
    *fresh = true;
    *status = NC_STATUS_SUCCESS;
    return obj;
}

/* ObOpenObjectByName(ObjectAttributes, ObjectType, ParseContext, Handle):
 * the named objects (XAPI's OpenEvent, OpenMutex, OpenSemaphore). */
static uint64_t ObOpenObjectByName(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char name[OB_NAME_MAX + 1];
    uint32_t dir;
    nc_vm_wr32(k->vm, a[3], 0);
    if (!oa_name(k, a[0], &dir, name, sizeof(name))) {
        return 0xC0000033u;                                   /* STATUS_OBJECT_NAME_INVALID */
    }
    enum NcObType type;
    const uint32_t obj = named_lookup(k, dir, name, &type);
    if (!obj) {
        return 0xC0000034u;                                   /* STATUS_OBJECT_NAME_NOT_FOUND */
    }
    if (a[1] && a[1] != k->type_va[type]) {
        return 0xC0000024u;                                   /* STATUS_OBJECT_TYPE_MISMATCH */
    }
    const uint32_t h = nc_ob_handle(k, obj, type);
    if (!h) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nc_vm_wr32(k->vm, a[3], h);
    return NC_STATUS_SUCCESS;
}

uint32_t nc_ob_handle(NcKernel *k, uint32_t object, enum NcObType type)
{
    for (unsigned i = 1; i < NC_MAX_HANDLES; i++) {
        if (!k->handle_table[i].object) {
            k->handle_table[i] = (NcHandle){object, type};
            uint32_t hdr = object - OB_HEADER;
            nc_vm_wr32(k->vm, hdr + 0x4, nc_vm_rd32(k->vm, hdr + 0x4) + 1); /* HandleCount */
            return i * 4;
        }
    }
    nc_log("hakuX-native", "ob: out of handles");
    return 0;
}

uint32_t nc_ob_lookup(NcKernel *k, uint32_t handle, enum NcObType *type)
{
    uint32_t i = handle / 4;
    if (handle & 3 || !i || i >= NC_MAX_HANDLES || !k->handle_table[i].object) {
        return 0;
    }
    if (type) {
        *type = k->handle_table[i].type;
    }
    return k->handle_table[i].object;
}

/* Closes a handle; a file's host state goes with its last handle. */
bool nc_ob_close(NcKernel *k, uint32_t handle)
{
    uint32_t i = handle / 4;
    if (handle & 3 || !i || i >= NC_MAX_HANDLES || !k->handle_table[i].object) {
        return false;
    }
    const uint32_t object = k->handle_table[i].object;
    const enum NcObType type = k->handle_table[i].type;
    const uint32_t hdr = object - OB_HEADER;
    const uint32_t count = nc_vm_rd32(k->vm, hdr + 0x4) - 1;
    nc_vm_wr32(k->vm, hdr + 0x4, count);
    k->handle_table[i].object = 0;
    if (!count && (type == NC_OB_FILE || type == NC_OB_SYMLINK)) {
        nc_io_close(k, object);
    }
    if (!count) {
        release(k, object, type);
    }
    return true;
}

/* NtClose(Handle): a file or symbolic link goes with its last handle and
 * reference (release). */
static uint64_t NtClose(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_ob_close(k, a[0]) ? NC_STATUS_SUCCESS : NC_STATUS_INVALID_HANDLE;
}

/* NtDuplicateObject(SourceHandle, TargetHandle, Options): DUPLICATE_CLOSE_SOURCE is 1.
 * NtCurrentThread (-2) duplicates to a real handle of the running thread. */
static uint64_t NtDuplicateObject(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    enum NcObType type = NC_OB_THREAD;
    uint32_t obj = a[0] == 0xFFFFFFFEu && k->current ? k->current->ethread : nc_ob_lookup(k, a[0], &type);
    if (!obj) {
        return NC_STATUS_INVALID_HANDLE;
    }
    uint32_t h = nc_ob_handle(k, obj, type);
    if (!h) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nc_vm_wr32(k->vm, a[1], h);
    if ((a[2] & 1) && a[0] != 0xFFFFFFFEu) {
        nc_ob_close(k, a[0]);
    }
    return NC_STATUS_SUCCESS;
}

void nc_ob_register(NcKernel *k)
{
    nc_kernel_impl(k, "NtClose", NtClose);
    nc_kernel_impl(k, "NtDuplicateObject", NtDuplicateObject);
    nc_kernel_impl(k, "ObOpenObjectByName", ObOpenObjectByName);
}
