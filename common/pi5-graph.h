/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

#define PI5_GRAPH_VERSION 1u
#define PI5_GRAPH_NAME L"\\Device\\Pi5Graph"
#define PI5_GRAPH_PATH 1024u
#define PI5_GRAPH_PROPERTY_NAME 256u
#define PI5_GRAPH_CHUNK 4096u
#define PI5_GRAPH_CELLS 16u
#define PI5_GRAPH_NONE UINT32_MAX

/* Buffered, read-only requests. Kernel consumers open PI5_GRAPH_NAME with
 * GENERIC_READ and FILE_SHARE_READ | FILE_SHARE_WRITE at PASSIVE_LEVEL. */
#define IOCTL_PI5_GRAPH_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x870, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_PI5_GRAPH_NODE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x871, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_PI5_GRAPH_PROPERTY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x872, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_PI5_GRAPH_FIND CTL_CODE(FILE_DEVICE_UNKNOWN, 0x873, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_PI5_GRAPH_RESOLVE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x874, METHOD_BUFFERED, FILE_READ_ACCESS)

typedef struct {
    uint32_t Version, Schema, Layout, Nodes, Properties, PropertyBytes, CacheBytes;
} PI5_GRAPH_INFO;
typedef struct { uint32_t Version, Node, Index, Offset; } PI5_GRAPH_KEY;
typedef struct {
    uint32_t Version, Index, Parent, Properties, Phandle;
    char Path[PI5_GRAPH_PATH], Owner[PI5_GRAPH_PATH];
} PI5_GRAPH_NODE;
typedef struct {
    uint32_t Version, Node, Index, Offset, Total, Length;
    char Name[PI5_GRAPH_PROPERTY_NAME];
    uint8_t Data[PI5_GRAPH_CHUNK];
} PI5_GRAPH_PROPERTY;
#define PI5_GRAPH_FIND_PATH 1u
#define PI5_GRAPH_FIND_OWNER 2u
#define PI5_GRAPH_FIND_COMPATIBLE 3u
#define PI5_GRAPH_FIND_PHANDLE 4u
typedef struct {
    uint32_t Version, Start, Kind, Phandle;
    char Text[PI5_GRAPH_PATH];
} PI5_GRAPH_FIND;
typedef struct {
    uint32_t Version, Node, Index, Reserved;
    char Property[PI5_GRAPH_PROPERTY_NAME];
    /* Provider's #*-cells property, or empty for a plain phandle list. */
    char CellsProperty[PI5_GRAPH_PROPERTY_NAME];
} PI5_GRAPH_RESOLVE;
typedef struct {
    uint32_t Version, Provider, Phandle, Count;
    uint32_t Cells[PI5_GRAPH_CELLS];
} PI5_GRAPH_REFERENCE;

/* Strings are NUL terminated. All numeric fields are native little endian;
 * PROPERTY.Data retains the original DT bytes (big-endian cells). Indices
 * describe this boot only. Data grants no MMIO/DMA/clock/reset ownership.
 * FIND Start permits enumerating every node with a grouped ACPI owner.
 * RESOLVE Index selects a specifier; zero phandles count as empty entries.
 * Consumers must also inspect status and acquire the actual provider driver. */
