# BCM2712 multimedia IOMMU provider

Owns the MMU0 ACPI resources for the BCM2712 multimedia IOMMUs and their shared
translation cache. Kernel clients use IOMMU4 page tables backed by
HAL-allocated common buffers, with invalid guard pages, access permissions,
serialized cache invalidation, and explicit buffer/session lifetimes.
Translation is enabled only inside a synchronous DMA window whose client
verifies ownership and drains its hardware before returning. Outside that
window, buffers remain staged with translation disabled. Clients can retire
an IOVA while keeping its backing allocation. Register snapshots are available
to administrators. Device faults, uncertain stops or invalidation, and damaged
buffer guards quarantine the allocations until reboot.

The interface is defined in [pi5-iommu.h](../common/pi5-iommu.h). It depends on
the matching Raspberry Pi 5 UEFI MMU0 resource description and its noncoherent
DMA declaration (`_CCA=0`). A DMA consumer must own its device resources,
retain the relevant clock and power services, and preserve other IOMMU4
clients using the below-40-GiB bypass range.
