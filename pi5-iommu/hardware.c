/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "hardware.h"

static const uint32_t Offsets[9] = {0,4,0x14,0x1c,0x20,0x24,0x30,0x34,0x38};

static int Address(uint64_t dma, uint32_t pages)
{
    return pages && pages <= MMU_MAX_PAGES && !(dma & (MMU_PAGE - 1u)) &&
        dma <= MMU_DMA_LIMIT &&
        (uint64_t)pages * MMU_PAGE - 1u <= MMU_DMA_LIMIT - dma;
}

static void Write(const MMU_IO *io, MMU_HW *s, uint32_t unit,
                  uint32_t offset, uint32_t value)
{
    ++s->Writes;
    io->Write(io->Context, unit, offset, value);
}

static int Fault(MMU_HW *s, int error)
{
    s->Fault = 1;
    return error;
}

int MmuCheck(const MMU_IO *io, MMU_HW *s)
{
    uint32_t i, j;
    if (s->Fault) return MmuDrift;
    if (!s->Staged) return MmuInvalid;
    if (s->Active) return MmuBusy;
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 9; ++j) {
            uint32_t expected = i == 1 ? s->Expected[j] : s->Baseline[i][j];
            if (io->Read(io->Context, i, Offsets[j]) != expected)
                return Fault(s, MmuDrift);
        }
    }
    if (io->Read(io->Context, 3, 0) != s->Cache) return Fault(s, MmuDrift);
    return MmuOk;
}

static int Poll(const MMU_IO *io, MMU_HW *s, uint32_t unit, uint32_t mask)
{
    uint32_t i;
    for (i = 0; i < MMU_POLL_LIMIT; ++i) {
        if (!(io->Read(io->Context, unit, 0) & mask)) return MmuOk;
        io->Delay(io->Context);
    }
    return Fault(s, MmuTimeout);
}

int MmuInvalidate(const MMU_IO *io, MMU_HW *s)
{
    int result = MmuCheck(io, s);
    if (result) return result;
    io->Barrier(io->Context);
    /* The sole MMU0 lock covers the shared cache AND the local TLB. All
     * translation units must remain disabled for this staging interface. */
    Write(io, s, 3, 0, s->Cache | 3u);
    result = Poll(io, s, 3, 4u);
    if (result) return result;
    Write(io, s, 3, 0, s->Cache);
    Write(io, s, 1, 0, 4u);
    result = Poll(io, s, 1, 0x80u);
    if (result) return result;
    Write(io, s, 1, 0, 0);
    io->Barrier(io->Context);
    result = MmuCheck(io, s);
    if (!result) ++s->Flushes;
    return result;
}

int MmuBegin(const MMU_IO *io, MMU_HW *s, uint64_t table, uint64_t trap)
{
    uint32_t i, j, debug;
    if (s->Staged || s->Fault) return MmuBusy;
    if (!Address(table, 1) || !Address(trap, 1) || table < (MMU_IOVA_BASE >> 20))
        return MmuInvalid;
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 9; ++j)
            s->Baseline[i][j] = io->Read(io->Context, i, Offsets[j]);
        if (s->Baseline[i][0]) return MmuBusy;
    }
    debug = s->Baseline[1][8];
    if ((debug & 15u) < 4 || ((debug >> 4) & 15u) < 6 ||
        ((debug >> 8) & 15u) < 6 || !(debug & 0x20000000u)) return MmuInvalid;
    s->Cache = io->Read(io->Context, 3, 0);
    if (s->Cache & 6u) return MmuBusy;
    for (j = 0; j < 9; ++j) s->Expected[j] = s->Baseline[1][j];
    s->Staged = 1;
    if (MmuCheck(io, s)) return MmuDrift;
    s->Expected[1] = (uint32_t)(table >> 12) - (uint32_t)(MMU_IOVA_BASE >> 32);
    s->Expected[2] = 0x80000000u | (uint32_t)(MMU_IOVA_BASE >> 28);
    s->Expected[3] = 0;
    s->Expected[4] = 0x80000000u |
        (uint32_t)(MMU_IOVA_BASE >> ((debug & 0x10000000u) ? 22 : 28));
    s->Expected[5] &= ~0x80000000u;
    s->Expected[6] = 0x80000000u | (uint32_t)(trap >> 12);
    io->Barrier(io->Context);
    for (j = 1; j <= 6; ++j) Write(io, s, 1, Offsets[j], s->Expected[j]);
    return MmuInvalidate(io, s);
}

int MmuEnd(const MMU_IO *io, MMU_HW *s)
{
    uint32_t j;
    int result = MmuInvalidate(io, s);
    if (result) return result;
    for (j = 1; j <= 6; ++j) {
        s->Expected[j] = s->Baseline[1][j];
        Write(io, s, 1, Offsets[j], s->Expected[j]);
    }
    result = MmuInvalidate(io, s);
    if (!result) s->Staged = 0;
    return result;
}

int MmuActivate(const MMU_IO *io, MMU_HW *s)
{
    int result = MmuInvalidate(io, s);
    if (result) return result;
    /* Only the synchronous consumer callback may publish an IOVA. Keep
     * the shared cache enabled until that consumer has drained its DMA. */
    Write(io, s, 3, 0, s->Cache | 1u);
    s->Active = 1;
    Write(io, s, 1, 0, MMU_ACTIVE_CONTROL);
    io->Barrier(io->Context);
    if (io->Read(io->Context, 3, 0) != (s->Cache | 1u) ||
        io->Read(io->Context, 1, 0) != MMU_ACTIVE_CONTROL)
        return Fault(s, MmuDrift);
    return MmuOk;
}

int MmuDeactivate(const MMU_IO *io, MMU_HW *s, uint32_t quiesced)
{
    uint32_t i, j, control;
    if (!s->Active) return MmuInvalid;
    s->FaultFlags = io->Read(io->Context, 1, 0) & MMU_FAULT_FLAGS;
    s->ViolationAddress = io->Read(io->Context, 1, 0x34);
    s->Hits = io->Read(io->Context, 1, 8);
    s->Misses = io->Read(io->Context, 1, 12);
    s->Stalls = io->Read(io->Context, 1, 16);
    if (!quiesced) return Fault(s, MmuBusy);
    if (s->Fault) return MmuDrift;
    control = io->Read(io->Context, 1, 0);
    if ((control & ~MMU_FAULT_FLAGS) != MMU_ACTIVE_CONTROL ||
        io->Read(io->Context, 3, 0) != (s->Cache | 1u))
        return Fault(s, MmuDrift);
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 9; ++j) {
            uint32_t expected = i == 1 ? s->Expected[j] : s->Baseline[i][j];
            if (i == 1 && (j == 0 || (j == 7 && s->FaultFlags))) continue;
            if (io->Read(io->Context, i, Offsets[j]) != expected)
                return Fault(s, MmuDrift);
        }
    }
    /* Fault status is W1C. Writing zero disables translation without
     * discarding the evidence. A device fault still quarantines the pages. */
    Write(io, s, 1, 0, 0);
    io->Barrier(io->Context);
    if (io->Read(io->Context, 1, 0) != s->FaultFlags)
        return Fault(s, MmuDrift);
    s->Active = 0;
    Write(io, s, 3, 0, s->Cache);
    if (io->Read(io->Context, 3, 0) != s->Cache)
        return Fault(s, MmuDrift);
    if (s->FaultFlags) return Fault(s, MmuDeviceFault);
    return MmuInvalidate(io, s);
}

int MmuMap(volatile uint32_t *l2, uint32_t slot, uint64_t dma,
           uint32_t pages, uint32_t writable)
{
    uint32_t i, start;
    if (slot >= MMU_SLOTS || !Address(dma, pages) || writable > 1) return MmuInvalid;
    start = slot * MMU_SLOT_PAGES;
    for (i = 0; i < MMU_SLOT_PAGES; ++i)
        if (l2[start + i]) return MmuBusy;
    for (i = 0; i < pages; ++i)
        l2[start + 1 + i] = ((uint32_t)(dma >> 12) + i) |
            MMU_PTE_VALID | (writable ? MMU_PTE_WRITE : 0);
    return MmuOk;
}

int MmuUnmap(volatile uint32_t *l2, uint32_t slot, uint32_t pages)
{
    uint32_t i, start;
    if (slot >= MMU_SLOTS || !pages || pages > MMU_MAX_PAGES) return MmuInvalid;
    start = slot * MMU_SLOT_PAGES;
    if (l2[start]) return MmuDrift;
    for (i = 1; i <= pages; ++i)
        if (!(l2[start + i] & MMU_PTE_VALID)) return MmuDrift;
    for (i = pages + 1; i < MMU_SLOT_PAGES; ++i)
        if (l2[start + i]) return MmuDrift;
    for (i = 1; i <= pages; ++i) l2[start + i] = 0;
    return MmuOk;
}

int MmuWalk(const volatile uint32_t *l2, uint64_t iova, uint32_t write,
            uint64_t *dma)
{
    uint32_t pte;
    *dma = 0;
    if (write > 1 || iova < MMU_IOVA_BASE || iova - MMU_IOVA_BASE >= MMU_PAGE * MMU_WORDS)
        return MmuInvalid;
    pte = l2[(uint32_t)((iova - MMU_IOVA_BASE) >> 12)];
    if (!(pte & MMU_PTE_VALID) || (write && !(pte & MMU_PTE_WRITE))) return MmuInvalid;
    *dma = ((uint64_t)(pte & 0x0fffffffu) << 12) | (iova & (MMU_PAGE - 1u));
    return MmuOk;
}
