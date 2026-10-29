/*
 * hakuX HLE GPU — guest RAM from host code (physical addresses).
 */
#ifndef HW_XBOX_HLE_MEM_GUESTMEM_H
#define HW_XBOX_HLE_MEM_GUESTMEM_H

#include "hw/xbox/hle/hle_pure.h"

void hle_guestmem_init(uint8_t *ram, uint64_t size, uint64_t ram_addr_base);
/* The core stops (M2): guest RAM is unmapped after it, and hle_pa_ptr finds
 * none until the next boot's hle_guestmem_init. */
void hle_guestmem_reset(void);
uint64_t hle_ram_size(void);
/* uint64_t of guest PA 0 (dirty bitmap / notdirty_write address space). */
uint64_t hle_ram_addr_base(void);
/* Host pointer for [pa, pa + n), or NULL when it leaves guest RAM. */
uint8_t *hle_pa_ptr(uint32_t pa, uint32_t n);

#endif
