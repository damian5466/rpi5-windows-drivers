/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif
#include <wmistr.h>
#include <wmidata.h>
#include <string.h>
#include "temperature.h"

int fan_temperature_parse(const void *buffer, size_t length, int32_t *temperature)
{
    static const GUID thermalGuid = MSAcpi_ThermalZoneTemperatureGuid;
    const unsigned char *bytes = buffer;
    int32_t hottest = INT32_MIN;
    size_t at = 0;
    if (!temperature) return 0;
    *temperature = INT32_MIN;
    if (!buffer) return 0;
    for (;;) {
        WNODE_ALL_DATA node;
        size_t size, tableEnd, stride = 0;
        ULONG i;
        if (length - at < sizeof(node)) return 0;
        memcpy(&node, bytes + at, sizeof(node));
        size = node.WnodeHeader.BufferSize;
        if (size < sizeof(node) || size > length - at ||
            !(node.WnodeHeader.Flags & WNODE_FLAG_ALL_DATA) ||
            (node.WnodeHeader.Flags & WNODE_FLAG_TOO_SMALL) ||
            memcmp(&node.WnodeHeader.Guid, &thermalGuid, sizeof(GUID))) return 0;
        tableEnd = sizeof(node);
        if (node.WnodeHeader.Flags & WNODE_FLAG_FIXED_INSTANCE_SIZE) {
            if (node.InstanceCount) {
                if (node.FixedInstanceSize < sizeof(MSAcpi_ThermalZoneTemperature) ||
                    node.DataBlockOffset < tableEnd || node.DataBlockOffset > size)
                    return 0;
                /* WMI pads every fixed-size instance to a QUADWORD boundary. */
                stride = ((size_t)node.FixedInstanceSize + 7) & ~(size_t)7;
                if (node.InstanceCount - 1 > (size - node.DataBlockOffset) / stride)
                    return 0;
            }
        } else {
            tableEnd = FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength);
            if (node.InstanceCount > (size - tableEnd) / sizeof(OFFSETINSTANCEDATAANDLENGTH))
                return 0;
            tableEnd += (size_t)node.InstanceCount * sizeof(OFFSETINSTANCEDATAANDLENGTH);
        }
        for (i = 0; i < node.InstanceCount; ++i) {
            MSAcpi_ThermalZoneTemperature data;
            size_t offset, dataSize;
            int32_t celsius;
            if (node.WnodeHeader.Flags & WNODE_FLAG_FIXED_INSTANCE_SIZE) {
                offset = node.DataBlockOffset + (size_t)i * stride;
                dataSize = node.FixedInstanceSize;
            } else {
                OFFSETINSTANCEDATAANDLENGTH entry;
                memcpy(&entry, bytes + at +
                    FIELD_OFFSET(WNODE_ALL_DATA, OffsetInstanceDataAndLength) +
                    (size_t)i * sizeof(entry), sizeof(entry));
                offset = entry.OffsetInstanceData;
                dataSize = entry.LengthInstanceData;
            }
            if (offset < tableEnd || offset > size || (offset & 7) ||
                dataSize < sizeof(data) || dataSize > size - offset) return 0;
            memcpy(&data, bytes + at + offset, sizeof(data));
            /* Allow half a 0.1 K quantum at the sensor's -40..125 C limits.
             * Reject 0 K, overflow and sentinel values before converting. */
            if (data.CurrentTemperature < 2331 || data.CurrentTemperature > 3982)
                continue;
            celsius = (int32_t)data.CurrentTemperature * 100 - 273150;
            if (celsius < -40000) celsius = -40000;
            if (celsius > 125000) celsius = 125000;
            if (celsius > hottest) hottest = celsius;
        }
        if (!node.WnodeHeader.Linkage) break;
        if (node.WnodeHeader.Linkage < size ||
            node.WnodeHeader.Linkage > length - at) return 0;
        at += node.WnodeHeader.Linkage;
    }
    *temperature = hottest;
    return hottest != INT32_MIN;
}
