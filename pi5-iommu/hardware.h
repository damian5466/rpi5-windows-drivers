/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

#define MMU_PAGE 4096u
#define MMU_WORDS 1024u
#define MMU_SLOTS 16u
#define MMU_MAX_PAGES 16u
#define MMU_SLOT_PAGES (MMU_MAX_PAGES + 2u)
#define MMU_IOVA_BASE UINT64_C(0xa00000000)
#define MMU_DMA_LIMIT UINT64_C(0xfffffffff)
#define MMU_PTE_VALID 0x10000000u
#define MMU_PTE_WRITE 0x20000000u
#define MMU_POLL_LIMIT 1000u
#define MMU_ACTIVE_CONTROL 0x04090803u
#define MMU_FAULT_FLAGS 0x08101000u

typedef struct {
    void *Context;
    uint32_t (*Read)(void *, uint32_t, uint32_t);
    void (*Write)(void *, uint32_t, uint32_t, uint32_t);
    void (*Barrier)(void *);
    void (*Delay)(void *);
} MMU_IO;

typedef struct {
    uint32_t Baseline[3][9];
    uint32_t Expected[9];
    uint32_t Cache, Staged, Fault, Writes, Flushes, Active;
    uint32_t FaultFlags, ViolationAddress, Hits, Misses, Stalls;
} MMU_HW;

enum { MmuOk, MmuInvalid, MmuBusy, MmuTimeout, MmuDrift, MmuDeviceFault };
int MmuBegin(const MMU_IO *io, MMU_HW *state, uint64_t table, uint64_t trap);
int MmuInvalidate(const MMU_IO *io, MMU_HW *state);
int MmuEnd(const MMU_IO *io, MMU_HW *state);
int MmuCheck(const MMU_IO *io, MMU_HW *state);
int MmuActivate(const MMU_IO *io, MMU_HW *state);
/* The caller must first stop and drain every master using this aperture.
 * On an unproven stop, translation and all backing memory remain pinned. */
int MmuDeactivate(const MMU_IO *io, MMU_HW *state, uint32_t quiesced);

/* Only HAL logical addresses of provider-owned buffers enter these tables.
 * Each slot has an invalid page on either side, including short allocations. */
int MmuMap(volatile uint32_t *l2, uint32_t slot, uint64_t dma,
           uint32_t pages, uint32_t writable);
int MmuUnmap(volatile uint32_t *l2, uint32_t slot, uint32_t pages);
int MmuWalk(const volatile uint32_t *l2, uint64_t iova, uint32_t write,
            uint64_t *dma);
