# BCM2712 multimedia IOMMU

`Pi5Iommu.sys` manages the BCM2712 multimedia IOMMUs and their shared
translation cache. It provides guarded DMA buffers, permission-controlled
mappings and bounded DMA windows to kernel clients. Its translation resources
are separate from the V3D GPU's internal MMU.

It requires matching firmware exposing the MMU0 resources and their
noncoherent DMA contract. Consumers retain ownership of their device, clock
and power resources while using a translation window.
