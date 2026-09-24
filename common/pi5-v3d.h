/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

#define PI5_V3D_NAME L"\\Device\\Pi5V3d"
#define PI5_V3D_PATH L"\\\\.\\Pi5V3d"
#define PI5_V3D_VERSION 1u
#define PI5_V3D_WORDS 2048u
#define IOCTL_PI5_V3D_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a0, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_PI5_V3D_BEGIN CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a1, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_END CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a2, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_COPY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a3, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_RESET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a4, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_INTERRUPT_CHECK CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a5, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_COMPUTE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a6, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_COMPUTE_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a7, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_PI5_V3D_RENDER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a8, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_RENDER_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8a9, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_PI5_V3D_TRIANGLE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8aa, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_MEMORY_BEGIN CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8ab, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_BUFFER_CREATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8ac, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_BUFFER_DESTROY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8ad, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_BUFFER_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8ae, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_BUFFER_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8af, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_BUFFER_COPY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8b0, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_V3D_SUBMIT_CL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8b1, METHOD_BUFFERED, FILE_WRITE_DATA)
#define PI5_V3D_COMPUTE_LANES 16u
#define PI5_V3D_XOR_ADD 0u
#define PI5_V3D_XOR_SUB 1u

/* Only QUERY, COMPUTE_QUERY and RENDER_QUERY are available to user mode. Other calls require a kernel
 * requestor at PASSIVE_LEVEL. BEGIN owns a session on this file; END releases
 * it after proven quiescence. Operations are synchronous and serialized.
 * COPY uses only driver-owned DMA memory and returns raw 32-bit pixels;
 * Width=1..64, Height=1..32, tightly packed. The fixed command session accepts
 * no addresses, shaders, command lists or user pointers. QUERY reads cached
 * hardware values outside a session; Online only reports device readiness,
 * not an active GPU or a WDDM adapter. DMA addresses are diagnostics of the
 * last session, and do not grant a mapping or indicate retained allocation. */
typedef struct { uint32_t Version, Reserved; } PI5_V3D_REQUEST;
typedef struct {
    uint32_t Version, Width, Height, Reserved;
    uint32_t Pixels[PI5_V3D_WORDS];
} PI5_V3D_IMAGE;
typedef struct {
    uint32_t Version, Debug, Online, Owner, Fault, Phase, LastStatus;
    uint32_t HubIdent[4], CoreIdent[3], MmuInfo, TechVersion, PaBits, VaBits;
    uint32_t SmsInitial, SmsRee, SmsTee, Tfu, Csd, Ct0, Ct1, Gmp;
    uint32_t MmuControl, MmuCache, MmuVioId, MmuVioAddress;
    uint32_t HubInterrupts, CoreInterrupts, HubPending, CorePending;
    uint32_t Copies, Resets, InterruptChecks, Failures, AllocatedBytes;
    uint32_t MmuHits, MmuMisses, GuardFailures, ScratchWrites;
    uint64_t TableDma, SourceDma, DestinationDma;
} PI5_V3D_STATUS;

/* One fixed 16-lane CSD workgroup. Values[i] becomes (Values[i] ^ XorValue)
 * +/- Operand modulo 2^32. Operation selects XOR_ADD or XOR_SUB. Input Fence
 * must be zero; successful output Fence is the completed submission number.
 * Fences are synchronous, monotonically increasing during this device's
 * lifetime (including sessions/resets), and invalid across PnP restart/reboot.
 * Completion includes the real CSD IRQ, idle engines, GPU cache clean,
 * AXI drain, CPU visibility and backing-memory verification. */
typedef struct {
    uint32_t Version, Operation, XorValue, Operand;
    uint64_t Fence;
    uint32_t Values[PI5_V3D_COMPUTE_LANES];
} PI5_V3D_COMPUTE;
typedef struct {
    uint32_t Version, Jobs, Invalidations, IpRevision;
    /* Raw 7.1 status observations, not the legacy 4.2 counter bit layout. */
    uint32_t CsdBefore, CsdAfter, CodeBytes, Reserved;
    uint64_t Submitted, Completed, CodeDma, UniformDma;
} PI5_V3D_COMPUTE_STATUS;

/* Kernel-only, fixed single-tile RGBA8 clear through the render command
 * processor. Width=1..64, Height=1..32; input Fence must be zero. No client
 * command bytes or addresses are accepted. Pixels and Fence are outputs.
 * Render fences have their own sequence, with the COMPUTE lifetime rules. */
typedef struct {
    uint32_t Version, Width, Height, Color;
    uint64_t Fence;
    uint32_t Pixels[PI5_V3D_WORDS];
} PI5_V3D_RENDER;
typedef struct {
    uint32_t Version, Jobs, CommandBytes, Invalidations;
    uint32_t CtBefore, CtAfter, Current, End, QueueCurrent, QueueEnd;
    uint32_t FramesBefore, FramesAfter;
    uint64_t Submitted, Completed;
    uint32_t BinBytes, BinBefore, BinAfter, BinCurrent, BinEnd;
    uint32_t MismatchIndex, Observed, Expected;
} PI5_V3D_RENDER_STATUS;

#define PI5_V3D_BUFFER_WRITABLE 1u
#define PI5_V3D_BUFFER_LIMIT 256u
#define PI5_V3D_BUFFER_MAX_BYTES (64u * 1024u * 1024u)
#define PI5_V3D_MEMORY_MAX_BYTES (256u * 1024u * 1024u)
#define PI5_V3D_TRANSFER_MAX_BYTES 65536u

/* MEMORY_BEGIN selects a kernel-only buffer session instead of the fixed
 * command session. Addresses are assigned by the provider; callers never
 * supply physical pages or CPU pointers. CREATE takes zero Handle/Address;
 * DESTROY takes only Version/Handle. READ takes this transfer header and
 * returns the header followed by Bytes data; WRITE takes header plus data.
 * Transfers are aligned 32-bit words, at most 64 KiB per request. Handles
 * cannot be reused during the device lifetime. END proves quiescence before
 * freeing any allocation, including abandoned allocations on file cleanup. */
typedef struct {
    uint32_t Version, Handle, Address, Bytes, Flags, Reserved;
} PI5_V3D_BUFFER;
typedef struct {
    uint32_t Version, Handle, Offset, Bytes;
} PI5_V3D_TRANSFER;
typedef struct {
    uint32_t Version, Source, Destination, Width, Height, SourceOffset, DestinationOffset, Reserved;
    uint64_t Fence;
} PI5_V3D_BUFFER_COPY;

/* Trusted kernel consumer only, inside MEMORY_BEGIN. Command lists must be
 * wholly inside GPU read-only objects; tile storage must be writable. All
 * addresses belong to the current session. Embedded commands and shader
 * programs remain the kernel consumer's responsibility, so this interface
 * must never be forwarded to an untrusted client without validation. A zero
 * BclStart/BclEnd disables binning and requires zero tile storage fields.
 * Fence is zero on input, completed after both engines and memory retire. */
typedef struct {
    uint32_t Version, BclStart, BclEnd, RclStart, RclEnd;
    uint32_t TileAddress, TileBytes, StateAddress, StateBytes, Reserved;
    uint64_t Fence;
} PI5_V3D_SUBMIT_CL;
