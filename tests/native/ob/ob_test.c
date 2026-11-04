/*
 * Host test of native/kernel/ob.c's named objects: what XAPI's CreateEvent
 * (NtCreateEvent with a name relative to ObWin32NamedObjectsDirectory, -4,
 * and OBJ_OPENIF) and OpenEvent (ObOpenObjectByName) do with them.
 *
 *   gcc -O1 -I<repo> tests/native/ob/ob_test.c native/kernel/ob.c -o ob_test && ./ob_test
 */
#include "native/kernel/kernel.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int g_fail;
static NcKernel g_k;
static NcVm g_vm;
static NcKFn g_open, g_close;
static uint32_t g_pool = 0x80100000u;

/* What ob.c needs of the rest of the kernel. */
void nc_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s: ", tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void nc_kernel_impl(NcKernel *k, const char *name, NcKFn fn)
{
    if (!strcmp(name, "ObOpenObjectByName")) {
        g_open = fn;
    } else if (!strcmp(name, "NtClose")) {
        g_close = fn;
    }
}

uint32_t nc_pool_alloc(NcKernel *k, uint32_t size)
{
    const uint32_t va = g_pool;
    g_pool += (size + 15) & ~15u;
    return va;
}

void nc_pool_free(NcKernel *k, uint32_t va, uint32_t size)
{
}

void nc_io_close(NcKernel *k, uint32_t object)
{
}

static void check(bool ok, const char *what)
{
    g_fail += !ok;
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

#define EVENT_TYPE 0x80012400u
#define SEM_TYPE   0x80012780u
#define DATA       0x00011000u

/* An OBJECT_ATTRIBUTES { RootDirectory, ObjectName, Attributes } at oa with
 * its OBJECT_STRING and characters after it. */
static uint32_t attributes(uint32_t oa, uint32_t root, const char *name, uint32_t flags)
{
    const uint32_t str = oa + 12, chars = oa + 20;
    nc_vm_wr32(&g_vm, oa, root);
    nc_vm_wr32(&g_vm, oa + 4, str);
    nc_vm_wr32(&g_vm, oa + 8, flags);
    nc_vm_wr32(&g_vm, str, (uint32_t)strlen(name) * 0x10001u);
    nc_vm_wr32(&g_vm, str + 4, chars);
    memcpy(nc_vm_host(&g_vm, chars), name, strlen(name));
    return oa;
}

static uint32_t open_by_name(uint32_t oa, uint32_t type, uint32_t *handle)
{
    const uint32_t out = DATA + 0xF00;
    const uint32_t args[4] = { oa, type, 0, out };
    const uint32_t status = (uint32_t)g_open(&g_k, NULL, args);
    *handle = nc_vm_rd32(&g_vm, out);
    return status;
}

static void close_handle(uint32_t handle)
{
    const uint32_t args[1] = { handle };
    g_close(&g_k, NULL, args);
}

int main(void)
{
    /* the guest's address space: its data page and the pool, the rest unmapped */
    uint8_t *space = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (space == MAP_FAILED || mprotect(space + DATA, 0x1000, PROT_READ | PROT_WRITE) ||
        mprotect(space + g_pool, 0x10000, PROT_READ | PROT_WRITE)) {
        printf("FAIL guest address space\n");
        return 1;
    }
    g_vm.base = space;
    g_k.vm = &g_vm;
    g_k.type_va[NC_OB_EVENT] = EVENT_TYPE;
    g_k.type_va[NC_OB_SEMAPHORE] = SEM_TYPE;
    nc_ob_register(&g_k);
    check(g_open && g_close, "ObOpenObjectByName and NtClose registered");

    const uint32_t create = attributes(DATA + 0x000, 0xFFFFFFFCu, "TestEvent", 0x80);   /* OBJ_OPENIF */
    const uint32_t again = attributes(DATA + 0x100, 0xFFFFFFFCu, "TestEvent", 0);
    const uint32_t other = attributes(DATA + 0x200, 0xFFFFFFFCu, "tESTeVENT", 0);
    const uint32_t missing = attributes(DATA + 0x300, 0xFFFFFFFCu, "Missing", 0);
    const uint32_t absolute = attributes(DATA + 0x400, 0, "\\BaseNamedObjects\\TestEvent", 0);
    const uint32_t nameless = attributes(DATA + 0x500, 0xFFFFFFFCu, "", 0);

    uint32_t status, h;
    bool fresh;
    const uint32_t ev = nc_ob_create_named(&g_k, NC_OB_EVENT, 0x10, create, &status, &fresh);
    check(ev && status == NC_STATUS_SUCCESS && fresh, "a named event is created");
    const uint32_t h1 = nc_ob_handle(&g_k, ev, NC_OB_EVENT);

    const uint32_t ev2 = nc_ob_create_named(&g_k, NC_OB_EVENT, 0x10, create, &status, &fresh);
    check(ev2 == ev && status == 0x40000000u && !fresh, "OBJ_OPENIF opens it: STATUS_OBJECT_NAME_EXISTS");
    const uint32_t h2 = nc_ob_handle(&g_k, ev2, NC_OB_EVENT);
    check(!nc_ob_create_named(&g_k, NC_OB_EVENT, 0x10, again, &status, &fresh) && status == 0xC0000035u,
          "without OBJ_OPENIF: STATUS_OBJECT_NAME_COLLISION");
    check(!nc_ob_create_named(&g_k, NC_OB_SEMAPHORE, 0x14, create, &status, &fresh) && status == 0xC0000024u,
          "a semaphore of that name: STATUS_OBJECT_TYPE_MISMATCH");

    check(open_by_name(other, EVENT_TYPE, &h) == NC_STATUS_SUCCESS && nc_ob_lookup(&g_k, h, NULL) == ev,
          "ObOpenObjectByName finds it without case");
    close_handle(h);
    check(open_by_name(absolute, 0, &h) == NC_STATUS_SUCCESS && nc_ob_lookup(&g_k, h, NULL) == ev,
          "... by its \\BaseNamedObjects name, of any type");
    close_handle(h);
    check(open_by_name(other, SEM_TYPE, &h) == 0xC0000024u && !h, "... not as a semaphore");
    check(open_by_name(missing, EVENT_TYPE, &h) == 0xC0000034u && !h, "a name nobody made: not found");
    check(open_by_name(nameless, EVENT_TYPE, &h) == 0xC0000033u, "no name: invalid");

    const uint32_t plain = nc_ob_create_named(&g_k, NC_OB_EVENT, 0x10, 0, &status, &fresh);
    check(plain && plain != ev && status == NC_STATUS_SUCCESS && fresh, "no ObjectAttributes: an unnamed event");

    close_handle(h1);
    check(open_by_name(other, EVENT_TYPE, &h) == NC_STATUS_SUCCESS, "the name stays while a handle is open");
    close_handle(h);
    close_handle(h2);
    check(open_by_name(other, EVENT_TYPE, &h) == 0xC0000034u, "the last handle closed: the name is gone");

    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
