/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "gem.h"
#include <string.h>

static int mdio_idle(const GEM_IO *io)
{
    unsigned n;
    for (n = 0; n < 100; ++n) {
        if (io->read(io->context, GEM_NSR) & 4u) return 1;
        io->delay_us(io->context, 10);
    }
    return 0;
}

static int mdio(const GEM_IO *io, unsigned reg, uint16_t *data, int read)
{
    uint32_t command;
    if (reg > 31 || !mdio_idle(io)) return 0;
    /* Clause 22, fixed board PHY address 1; CODE=2, SOF=1. */
    command = (1u << 30) | ((read ? 2u : 1u) << 28) | (1u << 23) |
              (reg << 18) | (2u << 16) | (read ? 0u : *data);
    io->write(io->context, GEM_MAN, command);
    if (!mdio_idle(io)) return 0;
    if (read) *data = (uint16_t)io->read(io->context, GEM_MAN);
    return 1;
}

int gem_mdio_read(const GEM_IO *io, unsigned reg, uint16_t *value)
{ return mdio(io, reg, value, 1); }
int gem_mdio_write(const GEM_IO *io, unsigned reg, uint16_t value)
{ return mdio(io, reg, &value, 0); }

int gem_phy_setup(const GEM_IO *io)
{
    uint16_t value;
    unsigned n;
    /* Soft reset only the already identified BCM54213PE. */
    if (!gem_mdio_write(io, 0, 0x8000)) return 0;
    for (n = 0; n < 500; ++n) {
        if (!gem_mdio_read(io, 0, &value)) return 0;
        if (!(value & 0x8000)) break;
        io->delay_us(io->context, 1000);
    }
    if (n == 500) return 0;
    /* BCM54xx AUXCTL misc shadow 7: both read selectors, RX RGMII delay. */
    if (!gem_mdio_write(io, 0x18, 0x7007) ||
        !gem_mdio_read(io, 0x18, &value) ||
        !gem_mdio_write(io, 0x18, (uint16_t)(value | 0x8107))) return 0;
    /* Clock-control shadow 3, GTXCLK delay enable, preserve other bits. */
    if (!gem_mdio_write(io, 0x1c, 3u << 10) ||
        !gem_mdio_read(io, 0x1c, &value) ||
        !gem_mdio_write(io, 0x1c, (uint16_t)(0x8c00 | (value & 0x3ff) | 0x200))) return 0;
    /* Disable Broadcom's autonomous AutoGrEEEn LPI generation. */
    if (!gem_mdio_write(io, 0x17, 0x0d00) || !gem_mdio_read(io, 0x15, &value) ||
        !gem_mdio_write(io, 0x15, (uint16_t)(value & ~1u)) ||
        !gem_mdio_write(io, 0x17, 0)) return 0;
    /* No EEE or pause offload in this version. Clear EEE advertisement
       through the standard Clause-22 MMD access registers (MMD7, reg60). */
    if (!gem_mdio_write(io, 13, 7) || !gem_mdio_write(io, 14, 60) ||
        !gem_mdio_write(io, 13, 0x4007) || !gem_mdio_write(io, 14, 0) ||
        !gem_mdio_write(io, 13, 0)) return 0;
    /* Advertise 10/100/1000 full duplex, no flow control. */
    return gem_mdio_write(io, 4, 0x0141) && gem_mdio_write(io, 9, 0x0200) &&
           gem_mdio_write(io, 0, 0x1200);
}

int gem_link(const GEM_IO *io)
{
    uint16_t bmsr, adv, partner, giga, giga_partner;
    /* BMSR link is latched low: read twice. Wait for completed autoneg. */
    if (!gem_mdio_read(io, 1, &bmsr) || !gem_mdio_read(io, 1, &bmsr)) return -1;
    if ((bmsr & 0x24) != 0x24) return 0;
    if (!gem_mdio_read(io, 9, &giga) || !gem_mdio_read(io, 10, &giga_partner) ||
        !gem_mdio_read(io, 4, &adv) || !gem_mdio_read(io, 5, &partner)) return -1;
    if (giga_partner & 0x8000) return 0; /* master/slave fault */
    if ((giga & 0x200) && (giga_partner & 0x800)) return 1000;
    if (adv & partner & 0x100) return 100;
    if (adv & partner & 0x40) return 10;
    return 0;
}

int gem_valid_mac(const uint8_t mac[6])
{
    unsigned i, any = 0;
    if (mac[0] & 1) return 0;
    for (i = 0; i < 6; ++i) any |= mac[i];
    return any != 0;
}

int gem_dma_range_valid(uint64_t address, uint32_t size)
{
    return size != 0 && !(address & 4095u) && address <= UINT32_MAX &&
           (uint64_t)(size - 1u) <= UINT32_MAX - address;
}

void gem_copy_to_dma(volatile void *destination, const void *source, size_t length)
{
    volatile uint8_t *d = (volatile uint8_t *)destination;
    const uint8_t *s = (const uint8_t *)source;
    /* Common buffers can be Device/uncached memory on ARM64. Widen only
       aligned DMA accesses; memcpy handles an arbitrarily aligned host buffer.
       Never access beyond the requested frame, even for the final word. */
    while (length && ((uintptr_t)d & 7u)) { *d++ = *s++; --length; }
    while (length >= sizeof(uint64_t)) {
        uint64_t value;
        memcpy(&value, s, sizeof(value));
        *(volatile uint64_t *)d = value;
        d += sizeof(value); s += sizeof(value); length -= sizeof(value);
    }
    while (length--) *d++ = *s++;
}

void gem_copy_from_dma(void *destination, const volatile void *source, size_t length)
{
    uint8_t *d = (uint8_t *)destination;
    const volatile uint8_t *s = (const volatile uint8_t *)source;
    while (length && ((uintptr_t)s & 7u)) { *d++ = *s++; --length; }
    while (length >= sizeof(uint64_t)) {
        uint64_t value = *(const volatile uint64_t *)s;
        memcpy(d, &value, sizeof(value));
        d += sizeof(value); s += sizeof(value); length -= sizeof(value);
    }
    while (length--) *d++ = *s++;
}

void gem_init_rings(volatile void *memory, uint32_t dma)
{
    volatile uint8_t *base = (volatile uint8_t *)memory;
    GEM_DESC *rx = (GEM_DESC *)(base + GEM_RX_DESC_OFFSET);
    GEM_DESC *tx = (GEM_DESC *)(base + GEM_TX_DESC_OFFSET);
    GEM_DESC *tie = (GEM_DESC *)(base + GEM_TIE_OFFSET);
    unsigned i;
    for (i = 0; i < GEM_RING_SIZE; ++i) {
        rx[i].control = 0;
        rx[i].address = dma + GEM_RX_DATA_OFFSET + i * GEM_BUFFER_SIZE;
        if (i == GEM_RING_SIZE - 1) rx[i].address |= GEM_RX_WRAP;
        tx[i].address = dma + GEM_TX_DATA_OFFSET + i * GEM_BUFFER_SIZE;
        tx[i].control = GEM_TX_USED | (i == GEM_RING_SIZE - 1 ? GEM_TX_WRAP : 0);
    }
    /* Inactive queues point at permanently CPU-owned single descriptors. */
    tie[0].address = GEM_RX_OWN | GEM_RX_WRAP;
    tie[0].control = 0;
    tie[1].address = 0;
    tie[1].control = GEM_TX_USED | GEM_TX_WRAP;
}

void gem_disable(const GEM_IO *io)
{
    io->write(io->context, GEM_NCR, GEM_MPE);
    io->write(io->context, GEM_IDR, UINT32_MAX);
}

void gem_configure(const GEM_IO *io, uint32_t dma, const uint8_t mac[6])
{
    uint32_t width = (io->read(io->context, GEM_DCFG1) >> 25) & 7;
    uint32_t queues = io->read(io->context, GEM_DCFG6) & 0xff;
    unsigned q;
    gem_disable(io);
    width = width == 4 ? 2 : width == 2 ? 1 : 0;
    /* MDC <= 2.5 MHz for RP1's 200MHz pclk, no checksum/jumbo offload.
       Receive all valid frames; exact NDIS filtering happens in software. */
    io->write(io->context, GEM_NCFGR, (5u << 18) | (width << 21) |
              (1u << 17) | (1u << 8) | (1u << 4));
    io->write(io->context, GEM_USRIO, 1); /* RGMII */
    io->write(io->context, GEM_DMACFG, (32u << 16) | (3u << 8) | (1u << 10) | 16u);
    io->write(io->context, 0x044, 0); /* no RX cut-through */
    io->write(io->context, 0x054, (io->read(io->context, 0x054) & ~0x1ffffu) | 0x10808u);
    io->write(io->context, GEM_SA1B, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
              ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
    io->write(io->context, GEM_SA1T, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8));
    io->write(io->context, GEM_RBQP, dma + GEM_RX_DESC_OFFSET);
    io->write(io->context, GEM_TBQP, dma + GEM_TX_DESC_OFFSET);
    for (q = 1; q < 8; ++q) {
        if (!(queues & (1u << q))) continue;
        io->write(io->context, 0x620 + (q - 1) * 4, UINT32_MAX);
        io->write(io->context, 0x440 + (q - 1) * 4, dma + GEM_TIE_OFFSET + 8);
        io->write(io->context, 0x480 + (q - 1) * 4, dma + GEM_TIE_OFFSET);
        io->write(io->context, 0x4a0 + (q - 1) * 4, 32);
    }
    io->write(io->context, GEM_TSR, UINT32_MAX);
    io->write(io->context, GEM_RSR, UINT32_MAX);
    io->barrier(io->context);
}

void gem_set_speed(const GEM_IO *io, unsigned mbps)
{
    uint32_t config = io->read(io->context, GEM_NCFGR) & ~0x403u;
    config |= 2u; /* full duplex */
    if (mbps == 100) config |= 1u;
    if (mbps == 1000) config |= 0x400u;
    /* RP1's clock generator follows these speed bits automatically. */
    io->write(io->context, GEM_NCFGR, config);
}

int gem_rx_length(uint32_t control)
{
    unsigned size = control & 0xfffu;
    if ((control & (GEM_RX_SOF | GEM_RX_EOF)) != (GEM_RX_SOF | GEM_RX_EOF) ||
        size < 14 || size > GEM_FRAME_MAX) return -1;
    return (int)size;
}
