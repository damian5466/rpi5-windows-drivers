/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "hardware.h"
#include "triangle.h"

int V3dPoll(const V3D_IO *io, uint32_t unit, uint32_t off,
            uint32_t mask, uint32_t expected)
{
    uint32_t i;
    for (i = 0; i < 10000; ++i) {
        if ((io->Read(io->Context, unit, off) & mask) == expected) return V3dOk;
        io->Delay(io->Context); /* 10 us: no register wait exceeds 100 ms. */
    }
    return V3dTimeout;
}

int V3dIdle(const V3D_IO *io)
{
    uint32_t tfu = io->Read(io->Context, V3dHub, V3D_TFU_CS);
    /* No CLE queues, current/queued CSD work, or TFU conversion may survive
     * ownership acquisition or a completed transfer. */
    if ((tfu & 1u) || (tfu & 0x3f00u) != 0x2000u ||
        io->Read(io->Context, V3dCore, V3D_CSD_STATUS) != io->CsdIdle ||
        io->Read(io->Context, V3dCore, 0x100) ||
        io->Read(io->Context, V3dCore, 0x110) != io->BinEnd ||
        io->Read(io->Context, V3dCore, 0x108) != io->BinEnd ||
        io->Read(io->Context, V3dCore, 0x104) ||
        io->Read(io->Context, V3dCore, 0x114) != io->Read(io->Context, V3dCore, 0x10c) ||
        io->Read(io->Context, V3dCore, 0x10c) != io->RenderEnd ||
        io->Read(io->Context, V3dCore, 0x160) != io->BinStart ||
        io->Read(io->Context, V3dCore, 0x168) != io->BinEnd ||
        io->Read(io->Context, V3dCore, 0x164) != io->RenderStart ||
        io->Read(io->Context, V3dCore, 0x16c) != io->RenderEnd)
        return V3dBusy;
    return V3dOk;
}

int V3dDrain(const V3D_IO *io)
{
    int result = V3dIdle(io);
    if (result) return result;
    io->Write(io->Context, V3dCore, 0x34, 0);
    io->Write(io->Context, V3dCore, 0x38, UINT32_MAX);
    io->Write(io->Context, V3dCore, 0x30, 0x100); /* TMU write combiner */
    result = V3dPoll(io, V3dCore, 0x30, 0x100, 0);
    if (result) return result;
    io->Write(io->Context, V3dCore, 0x30, 5); /* Whole L2T clean */
    result = V3dPoll(io, V3dCore, 0x30, 1, 0);
    if (result) return result;
    io->Write(io->Context, V3dHub, V3D_GMP_CONFIG, 2); /* Stop new AXI */
    return V3dPoll(io, V3dHub, V3D_GMP_STATUS, V3D_GMP_BUSY, 0);
}

int V3dSmsReset(const V3D_IO *io)
{
    io->Write(io->Context, V3dSms, 0, 4);
    return V3dPoll(io, V3dSms, 0, 15, 0);
}

int V3dMmuFlush(const V3D_IO *io)
{
    int result;
    io->Write(io->Context, V3dHub, V3D_MMUC, 3);
    result = V3dPoll(io, V3dHub, V3D_MMUC, 4, 0);
    if (result) return result;
    io->Write(io->Context, V3dHub, V3D_MMU_CTL, V3D_MMU_ON | 4);
    return V3dPoll(io, V3dHub, V3D_MMU_CTL, 0x80, 0);
}

uint32_t V3dPte(uint64_t address, uint32_t writable)
{
    if (writable > 1 || (address & 4095) || address >= (UINT64_C(1) << 36)) return 0;
    return (uint32_t)(address >> 12) | V3D_PTE_VALID | (writable ? V3D_PTE_WRITE : 0);
}

int V3dMmuStart(const V3D_IO *io, uint64_t table, uint64_t trap)
{
    if (!V3dPte(table, 0) || !V3dPte(trap, 0) ||
        table > (UINT64_C(1) << 36) - V3D_TABLE_BYTES) return V3dInvalid;
    io->Write(io->Context, V3dHub, V3D_MMU_PT, (uint32_t)(table >> 12));
    io->Write(io->Context, V3dHub, V3D_MMU_TRAP, (uint32_t)(trap >> 12) | 0x80000000u);
    io->Write(io->Context, V3dHub, V3D_MMU_CTL, V3D_MMU_ON);
    return V3dMmuFlush(io);
}

void V3dSubmitCopy(const V3D_IO *io, uint32_t width, uint32_t height)
{
    V3dSubmitCopyAt(io, width, height, V3D_SOURCE_VA, V3D_DEST_VA);
}

void V3dSubmitCopyAt(const V3D_IO *io, uint32_t width, uint32_t height, uint32_t source, uint32_t destination)
{
    /* V3D 7.1 supports raster output. R32F (29) is the unfiltered generic
     * 32-bit copy format; pitches are in pixels, dimensions are literal. */
    io->Write(io->Context, V3dHub, 0x704, 0);
    io->Write(io->Context, V3dHub, 0x70c, source);
    io->Write(io->Context, V3dHub, 0x710, 0);
    io->Write(io->Context, V3dHub, 0x714, width);
    io->Write(io->Context, V3dHub, 0x718, 0);
    io->Write(io->Context, V3dHub, 0x71c, width << 16);
    io->Write(io->Context, V3dHub, 0x720, destination);
    io->Write(io->Context, V3dHub, 0x724, (height << 16) | width);
    io->Write(io->Context, V3dHub, 0x728, 0);
    io->Write(io->Context, V3dHub, 0x72c, 0);
    io->Write(io->Context, V3dHub, 0x730, 0);
    io->Write(io->Context, V3dHub, 0x734, 0);
    io->Write(io->Context, V3dHub, V3D_TFU_ICFG, (29u << 16) | 1);
}

int V3dInvalidate(const V3D_IO *io)
{
    int result = V3dIdle(io);
    if (result) return result;
    /* Previous GPU writes have already been cleaned by the completed job.
     * Invalidate outside-in before fetching replaced code, uniforms or data. */
    io->Write(io->Context, V3dCore, 0x34, 0);
    io->Write(io->Context, V3dCore, 0x38, UINT32_MAX);
    io->Write(io->Context, V3dCore, 0x30, 1); /* Whole L2T flush */
    result = V3dPoll(io, V3dCore, 0x30, 1, 0);
    if (result) return result;
    io->Write(io->Context, V3dCore, 0x24, 0x0f0f0f0f); /* Texture, uniform, instruction */
    return V3dOk;
}

void V3dSubmitCompute(const V3D_IO *io, uint32_t revision)
{
    /* Exactly one 16-item workgroup/supergroup/batch, no overlap or shared
     * storage. Fixed shader has thread switches and uses four-thread mode.
     * 7.1.6+ counts batches literally; earlier revisions subtract one. */
    io->Write(io->Context, V3dCore, 0x934, 1u << 16);
    io->Write(io->Context, V3dCore, 0x938, 1u << 16);
    io->Write(io->Context, V3dCore, 0x93c, 0x110);
    io->Write(io->Context, V3dCore, 0x940, revision >= 6 ? 1 : 0);
    io->Write(io->Context, V3dCore, 0x944, V3D_CODE_VA | 1);
    io->Write(io->Context, V3dCore, 0x948, V3D_UNIFORM_VA);
    io->Write(io->Context, V3dCore, 0x94c, 0);
    io->Write(io->Context, V3dCore, 0x930, 1u << 16); /* Launch last */
}

/* Explicit little-endian packet payloads. Facts are from the V3D 7.1 packet
 * definitions; the private test compares every field with Mesa's XML. */
static uint32_t Packet(uint8_t *dst, uint32_t at, uint32_t opcode,
                       uint64_t payload, uint32_t bytes)
{
    uint32_t i;
    dst[at++] = (uint8_t)opcode;
    for (i = 0; i < bytes; ++i) dst[at++] = (uint8_t)(payload >> (i * 8));
    return at;
}

uint32_t V3dBuildClear(uint8_t *dst, uint32_t capacity, uint32_t width,
                       uint32_t height, uint32_t color)
{
    uint32_t n = 0, i;
    if (!dst || capacity < 128 || !width || width > 64 || !height || height > 32) return 0;
    /* One 64x32 tile, no depth, samples, shader, binning or sublist branches. */
    n = Packet(dst, n, 121, ((uint64_t)width << 8) | ((uint64_t)height << 24) |
        (UINT64_C(1) << 44) | (UINT64_C(1) << 46) |
        (UINT64_C(3) << 52) | (UINT64_C(2) << 55), 8);
    /* RGBA8, 32 bpp, two-row stride 32 units of 128 bits, base zero. */
    n = Packet(dst, n, 121, 2 | (UINT64_C(31) << 18) | (UINT64_C(8) << 27) |
        ((uint64_t)color << 32), 8);
    n = Packet(dst, n, 121, 1, 8); /* ZS values terminate configuration */
    /* GFXH-1742 requires two dummy stores after internal type/size changes. */
    for (i = 0; i < 2; ++i) {
        n = Packet(dst, n, 124, 0, 3);
        n = Packet(dst, n, 26, 0, 0);
        n = Packet(dst, n, 29, 8, 8); /* STORE NONE, followed by address */
        n = Packet(dst, n, 0, 0, 3);
        if (!i) n = Packet(dst, n, 25, 0, 0);
        n = Packet(dst, n, 27, 0, 0);
    }
    n = Packet(dst, n, 19, 0, 0);
    n = Packet(dst, n, 124, 0, 3);
    n = Packet(dst, n, 26, 0, 0);
    n = Packet(dst, n, 29, (UINT64_C(27) << 12) | ((uint64_t)(width * 4) << 28), 8);
    n = Packet(dst, n, V3D_DEST_VA & 255, V3D_DEST_VA >> 8, 3);
    n = Packet(dst, n, 27, 0, 0);
    return Packet(dst, n, 13, 0, 0);
}

void V3dSubmitRender(V3D_IO *io, uint32_t bytes)
{
    V3dSubmitRenderAt(io, V3D_CODE_VA, V3D_CODE_VA + bytes);
}

void V3dSubmitRenderAt(V3D_IO *io, uint32_t start, uint32_t end)
{
    io->RenderStart = start; io->RenderEnd = end;
    io->Write(io->Context, V3dCore, 0x164, start);
    io->Write(io->Context, V3dCore, 0x16c, end);
}

uint32_t V3dTriangleRclBytes(void) { return sizeof(TriangleRcl); }
uint32_t V3dTriangleBclBytes(void) { return sizeof(TriangleBcl); }

uint32_t V3dTriangleCodeWord(uint32_t index, uint32_t color)
{
    uint32_t offset = index * 4, word = 0, j;
    const uint8_t *data = 0;
    uint32_t length = 0, base = 0;
    if (index >= V3D_PAGE / 4) return 0xbadc0ffeu;
    if (offset >= 0x400 && offset < 0x400 + sizeof(TriangleCs))
        return (uint32_t)(TriangleCs[(offset - 0x400) / 8] >> ((index & 1) * 32));
    if (offset >= 0x600 && offset < 0x600 + sizeof(TriangleVs))
        return (uint32_t)(TriangleVs[(offset - 0x600) / 8] >> ((index & 1) * 32));
    if (offset >= 0x800 && offset < 0x800 + sizeof(TriangleFs))
        return (uint32_t)(TriangleFs[(offset - 0x800) / 8] >> ((index & 1) * 32));
    if (offset < sizeof(TriangleRcl)) { data = TriangleRcl; length = sizeof(TriangleRcl); }
    else if (offset >= 0x100 && offset < 0x100 + sizeof(TriangleBcl))
        { data = TriangleBcl; length = sizeof(TriangleBcl); base = 0x100; }
    else if (offset >= 0x200 && offset < 0x200 + sizeof(TriangleGeneric))
        { data = TriangleGeneric; length = sizeof(TriangleGeneric); base = 0x200; }
    else if (offset >= 0x300 && offset < 0x300 + sizeof(TriangleState))
        { data = TriangleState; length = sizeof(TriangleState); base = 0x300; }
    if (!data) return 0xbadc0ffeu;
    for (j = 0; j < 4; ++j) {
        uint32_t at = offset + j, value = 0;
        if (at >= 14 && at < 18) value = (color >> ((at - 14) * 8)) & 255;
        else if (at - base < length) value = data[at - base];
        word |= value << (j * 8);
    }
    return word;
}

uint32_t V3dTriangleSourceWord(uint32_t index)
{
    /* Clip XYZW, then signed .6 screen XY relative to viewport centre,
     * float depth and reciprocal W. Screen vertices: (8,8),(55,8),(8,24).
     * Attribute fetch uses raw integer words, preserving all float bits. */
    static const uint32_t vertices[] = {
        0xbf400000,0xbf000000,0,0x3f800000,0xfffffa00,0xfffffe00,0x3f000000,0x3f800000,
        0x3f380000,0xbf000000,0,0x3f800000,0x000005c0,0xfffffe00,0x3f000000,0x3f800000,
        0xbf400000,0x3f000000,0,0x3f800000,0xfffffa00,0x00000200,0x3f000000,0x3f800000
    };
    return index < sizeof(vertices) / sizeof(vertices[0]) ? vertices[index] : 0xbadc0ffeu;
}

uint32_t V3dTriangleUniformWord(uint32_t index)
{
    static const uint32_t uniforms[] = {0,0x3f800000,0,0x3f800000,0xffffff3f};
    return index < sizeof(uniforms) / sizeof(uniforms[0]) ? uniforms[index] : 0xbadc0ffeu;
}

uint32_t V3dTrianglePixel(uint32_t index, uint32_t color)
{
    uint32_t x = index % 64, y = index / 64;
    /* No pixel centre lies on the sloped edge: its equation at centres has
     * a half-integer value. This avoids ambiguity from the edge tie rule. */
    return x >= 8 && y >= 8 && 16 * (x - 8) + 47 * (y - 8) <= 720 ? 0xff00ff00 : color;
}

void V3dSubmitBin(V3D_IO *io)
{
    V3dSubmitBinAt(io, V3D_CODE_VA + 0x100, V3D_CODE_VA + 0x100 + sizeof(TriangleBcl),
                     V3D_TILE_VA, V3D_TILE_BYTES, V3D_TILE_STATE_VA);
}

void V3dSubmitBinAt(V3D_IO *io, uint32_t start, uint32_t end, uint32_t tile,
                     uint32_t bytes, uint32_t state)
{
    io->BinStart = start; io->BinEnd = end;
    io->Write(io->Context, V3dCore, 0x30c, 0); /* No overflow allocation */
    io->Write(io->Context, V3dCore, 0x170, tile);
    io->Write(io->Context, V3dCore, 0x174, bytes);
    io->Write(io->Context, V3dCore, 0x15c, state | 2);
    io->Write(io->Context, V3dCore, 0x160, io->BinStart);
    io->Write(io->Context, V3dCore, 0x168, io->BinEnd);
}
