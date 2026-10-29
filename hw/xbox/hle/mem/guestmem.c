/*
 * hakuX HLE GPU — guest memory from host code.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/mem/guestmem.h"

static uint8_t *g_ram;
static uint64_t g_ram_size, g_ram_addr_base;

void hle_guestmem_init(uint8_t *ram, uint64_t size, uint64_t ram_addr_base)
{
    g_ram = ram;
    g_ram_size = size;
    g_ram_addr_base = ram_addr_base;
}

void hle_guestmem_reset(void)
{
    g_ram = NULL;
    g_ram_size = g_ram_addr_base = 0;
}

uint64_t hle_ram_size(void)
{
    return g_ram_size;
}

uint64_t hle_ram_addr_base(void)
{
    return g_ram_addr_base;
}

uint8_t *hle_pa_ptr(uint32_t pa, uint32_t n)
{
    if (!g_ram || (uint64_t)pa + n > g_ram_size) {
        return NULL;
    }
    return g_ram + pa;
}
