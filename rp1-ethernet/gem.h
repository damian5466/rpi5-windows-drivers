/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* RP1 GEM register/descriptor contract. See README.md for primary sources. */
#ifndef RP1_GEM_H
#define RP1_GEM_H
#include <stdint.h>
#include <stddef.h>

#define GEM_NCR       0x000u
#define GEM_NCFGR     0x004u
#define GEM_NSR       0x008u
#define GEM_USRIO     0x00cu
#define GEM_DMACFG    0x010u
#define GEM_TSR       0x014u
#define GEM_RBQP      0x018u
#define GEM_TBQP      0x01cu
#define GEM_RSR       0x020u
#define GEM_IDR       0x02cu
#define GEM_MAN       0x034u
#define GEM_MID       0x0fcu
#define GEM_DCFG1     0x280u
#define GEM_DCFG6     0x294u
#define GEM_SA1B      0x088u
#define GEM_SA1T      0x08cu
#define GEM_RE        (1u << 2)
#define GEM_TE        (1u << 3)
#define GEM_MPE       (1u << 4)
#define GEM_TSTART    (1u << 9)
#define GEM_TX_GO     (1u << 3)
#define GEM_RX_OWN    1u
#define GEM_RX_WRAP   2u
#define GEM_RX_SOF    (1u << 14)
#define GEM_RX_EOF    (1u << 15)
#define GEM_TX_LAST   (1u << 15)
#define GEM_TX_ERRORS (7u << 27)
#define GEM_TX_WRAP   (1u << 30)
#define GEM_TX_USED   (1u << 31)
#define GEM_RING_SIZE 512u
#define GEM_BUFFER_SIZE 2048u
#define GEM_FRAME_MAX 1514u
#define GEM_RX_DESC_OFFSET 0u
#define GEM_TX_DESC_OFFSET (GEM_RING_SIZE * 8u)
#define GEM_TIE_OFFSET (GEM_TX_DESC_OFFSET + GEM_RING_SIZE * 8u)
#define GEM_RX_DATA_OFFSET ((GEM_TIE_OFFSET + 16u + 4095u) & ~4095u)
#define GEM_TX_DATA_OFFSET (GEM_RX_DATA_OFFSET + GEM_RING_SIZE * GEM_BUFFER_SIZE)
#define GEM_DMA_SIZE (GEM_TX_DATA_OFFSET + GEM_RING_SIZE * GEM_BUFFER_SIZE)

typedef struct GEM_DESC { volatile uint32_t address, control; } GEM_DESC;
typedef struct GEM_IO {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*delay_us)(void *, unsigned);
    void (*barrier)(void *);
} GEM_IO;

/* All MDIO operations run at PASSIVE_LEVEL, serialized with start/stop. */
int gem_mdio_read(const GEM_IO *, unsigned reg, uint16_t *value);
int gem_mdio_write(const GEM_IO *, unsigned reg, uint16_t value);
int gem_phy_setup(const GEM_IO *);
/* Returns 0/10/100/1000, or -1 for an MDIO timeout. Full duplex only. */
int gem_link(const GEM_IO *);
int gem_valid_mac(const uint8_t mac[6]);
int gem_dma_range_valid(uint64_t address, uint32_t size);
void gem_copy_to_dma(volatile void *destination, const void *source, size_t length);
void gem_copy_from_dma(void *destination, const volatile void *source, size_t length);
void gem_init_rings(volatile void *memory, uint32_t dma_address);
/* Does not enable DMA. Caller must verify MID/PHY before calling. */
void gem_configure(const GEM_IO *, uint32_t dma_address, const uint8_t mac[6]);
/* The caller serializes these with transmit and descriptor access. */
void gem_set_speed(const GEM_IO *, unsigned mbps);
void gem_disable(const GEM_IO *);
int gem_rx_length(uint32_t control);
#endif
