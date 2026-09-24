/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

/* BCM2712 V3D 7.1 register facts, checked against Raspberry Pi Linux and
 * Mesa. Hub, core0 and SMS are separate translated ACPI resources. */
enum { V3dHub, V3dCore, V3dSms };
#define V3D_INT_STS 0x50u
#define V3D_INT_SET 0x54u
#define V3D_INT_CLR 0x58u
#define V3D_INT_MASK 0x5cu
#define V3D_INT_MASK_SET 0x60u
#define V3D_INT_MASK_CLR 0x64u
#define V3D_HUB_IRQS 0x7au
#define V3D_CORE_IRQS 0x47u
#define V3D_TFU_DONE 2u
#define V3D_CSD_DONE 0x40u
#define V3D_RENDER_DONE 1u
#define V3D_CSD_STATUS 0x900u
#define V3D_MMU_FAULT_IRQS 0x78u
#define V3D_GMP_STATUS 0x600u
#define V3D_GMP_CONFIG 0x604u
#define V3D_GMP_BUSY 0x7f7f0008u
#define V3D_TFU_CS 0x700u
#define V3D_TFU_ICFG 0x708u
#define V3D_MMUC 0x1000u
#define V3D_MMU_CTL 0x1200u
#define V3D_MMU_PT 0x1204u
#define V3D_MMU_TRAP 0x1230u
#define V3D_MMU_FAULTS 0x08101000u
#define V3D_MMU_ON 0x060d0c03u
#define V3D_PAGE 4096u
#define V3D_TABLE_BYTES 0x400000u
#define V3D_SOURCE_VA 0x10000u
#define V3D_DEST_VA 0x20000u
#define V3D_CODE_VA 0x30000u
#define V3D_UNIFORM_VA 0x40000u
#define V3D_TILE_VA 0x50000u
#define V3D_TILE_STATE_VA 0x60000u
#define V3D_TILE_BYTES 16384u
#define V3D_DATA_BYTES 8192u
#define V3D_PTE_VALID 0x10000000u
#define V3D_PTE_WRITE 0x20000000u

typedef struct {
    void *Context;
    uint32_t (*Read)(void *, uint32_t unit, uint32_t offset);
    void (*Write)(void *, uint32_t unit, uint32_t offset, uint32_t value);
    void (*Delay)(void *);
    /* Reset value 0, or raw state after the sole owned dispatch's CSDDONE.
     * Never update this after a missing IRQ or failed completion wait. */
    uint32_t CsdIdle;
    /* Queue addresses are latches; QBA remains at the submitted start after
     * completion. Track our sole submission, and prove retirement using CA. */
    uint32_t RenderStart, RenderEnd;
    uint32_t BinStart, BinEnd;
} V3D_IO;
enum { V3dOk, V3dTimeout, V3dBusy, V3dInvalid };
int V3dPoll(const V3D_IO *, uint32_t unit, uint32_t offset,
            uint32_t mask, uint32_t expected);
int V3dIdle(const V3D_IO *);
int V3dDrain(const V3D_IO *);
int V3dSmsReset(const V3D_IO *);
int V3dMmuFlush(const V3D_IO *);
int V3dMmuStart(const V3D_IO *, uint64_t table, uint64_t trap);
uint32_t V3dPte(uint64_t address, uint32_t writable);
void V3dSubmitCopy(const V3D_IO *, uint32_t width, uint32_t height);
void V3dSubmitCopyAt(const V3D_IO *, uint32_t width, uint32_t height, uint32_t source, uint32_t destination);
int V3dInvalidate(const V3D_IO *);
void V3dSubmitCompute(const V3D_IO *, uint32_t revision);
/* Writes at most 128 bytes. Returns zero without changing output for invalid
 * dimensions/capacity. The destination and all command addresses are fixed. */
uint32_t V3dBuildClear(uint8_t *, uint32_t capacity, uint32_t width,
                       uint32_t height, uint32_t color);
void V3dSubmitRender(V3D_IO *, uint32_t bytes);
void V3dSubmitRenderAt(V3D_IO *, uint32_t start, uint32_t end);
void V3dSubmitBin(V3D_IO *);
void V3dSubmitBinAt(V3D_IO *, uint32_t start, uint32_t end, uint32_t tile,
                     uint32_t bytes, uint32_t state);
uint32_t V3dTriangleCodeWord(uint32_t index, uint32_t color);
uint32_t V3dTriangleSourceWord(uint32_t index);
uint32_t V3dTriangleUniformWord(uint32_t index);
uint32_t V3dTrianglePixel(uint32_t index, uint32_t color);
uint32_t V3dTriangleRclBytes(void);
uint32_t V3dTriangleBclBytes(void);
