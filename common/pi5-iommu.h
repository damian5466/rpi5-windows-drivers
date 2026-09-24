/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

#define PI5_IOMMU_VERSION 1u
#define PI5_IOMMU_NAME L"\\Device\\Pi5Iommu"
#define PI5_IOMMU_PATH L"\\\\.\\Pi5Iommu"
#define IOCTL_PI5_IOMMU_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x890, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_PI5_IOMMU_BEGIN CTL_CODE(FILE_DEVICE_UNKNOWN, 0x891, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_IOMMU_ALLOCATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x892, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_IOMMU_FREE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x893, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_IOMMU_SYNC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x894, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_IOMMU_END CTL_CODE(FILE_DEVICE_UNKNOWN, 0x895, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_IOMMU_WALK CTL_CODE(FILE_DEVICE_UNKNOWN, 0x896, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_IOMMU_EXECUTE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x897, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_IOMMU_RETIRE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x898, METHOD_BUFFERED, FILE_WRITE_DATA)

/* Kernel-only contract: unit 4, at PASSIVE_LEVEL. The returned IOVA is usable
 * ONLY during the Execute callback below; otherwise translation is disabled.
 * CPU accesses must be naturally aligned scalar operations because
 * ACPI _CCA=0 common buffers are Device Memory on Windows ARM64. One file
 * owns a session; it must retain the file and buffers while using CpuAddress.
 * Begin/End/Sync use zero Pages/Writable/Token/Address. Allocate uses Pages
 * (1..16) and Writable (0/1). Free and Retire use Token only. Retire removes
 * the IOVA mapping and completes invalidation but retains the allocation and
 * CpuAddress until Free; a retired slot cannot be reassigned. Walk uses Address and
 * Writable (requested access). Free/End require the consumer to stop all CPU
 * accesses. End refuses live buffers. Cleanup retires unpublished buffers;
 * uncertain invalidation retains all memory and blocks reuse until reboot. */
typedef struct {
    uint32_t Version, Unit, Pages, Writable;
    uint64_t Token, Address;
} PI5_IOMMU_REQUEST;

typedef struct {
    uint64_t Token, Iova, DmaAddress, CpuAddress;
    uint32_t Bytes, Writable, StagedOnly, Reserved;
} PI5_IOMMU_BUFFER;

typedef struct {
    uint32_t Control, TableBase, AddressCap, BypassStart, BypassEnd;
    uint32_t Misc, IllegalPage, ViolationAddress, DebugInfo;
} PI5_IOMMU_REGISTERS;

typedef struct {
    uint32_t Version, Debug, Online, HardwareWrites;
    PI5_IOMMU_REGISTERS Units[3]; /* IOMMU2, IOMMU4, IOMMU5 */
    uint32_t CacheControl;
    uint32_t Owner, Staged, Fault, ActiveBuffers;
    uint32_t AllocatedBytes, Flushes, Failures, LastStatus;
    uint32_t Allocations, Frees, Cleanups, GuardChecks;
} PI5_IOMMU_STATUS;

typedef struct {
    uint32_t Status, PrepareStatus, TransferStatus, QuiesceStatus;
    uint32_t Active, FaultFlags, ViolationAddress, TrapWordsChanged;
    uint32_t Hits, Misses, Stalls, Executions;
} PI5_IOMMU_EXECUTION_RESULT;

#ifdef _KERNEL_MODE
/* Synchronous DMA window, on the session's owning file. Callbacks run at
 * PASSIVE_LEVEL under the provider lock; they must be bounded, nonpageable,
 * and must NOT call this provider recursively. Their code and Context must
 * live until this IOCTL completes. Only STATUS_SUCCESS means completed;
 * STATUS_PENDING and other informational results are rejected. No callback
 * is retained after completion.
 *
 * The caller owns its ACPI resources, clocks and power lifetime and serializes
 * every submission through this window. Prepare verifies exclusive ownership,
 * idle masters and preservation of other clients' below-40-GiB bypass traffic;
 * it must not submit DMA or modify mappings. Transfer may then use this
 * session's IOVAs. Quiesce runs after every activation attempt, even on error,
 * and must stop/drain all aperture users AND remove their programmed IOVAs
 * before returning success. It must not reset or reclock unrelated clients.
 *
 * The caller must pin its hardware and veto stop/remove until quiescence is
 * proven. An unsuccessful Quiesce permanently retains translation, adapter and
 * pages until reboot; the caller must also retain its resources and clocks.
 * File cleanup can retire buffers only outside a window with no latched fault.
 * Hardware faults are captured, then contained after a proven stop; even when
 * translation was disabled successfully the allocations remain quarantined.
 * Accepted executions return SUCCESS with their actual outcome in Result.Status.
 * StagedOnly remains 1 outside Transfer; it never grants a persistent DMA lease.
 */
typedef NTSTATUS (*PI5_IOMMU_CONSUMER_CALLBACK)(PVOID context);
typedef struct {
    uint32_t Version, Unit;
    PVOID Context;
    PI5_IOMMU_CONSUMER_CALLBACK Prepare, Transfer, Quiesce;
} PI5_IOMMU_EXECUTION;
#endif
