# NVMe compatibility patch

This package adapts the Windows ARM64 NVMe driver's DMA allocations and PRP
list cache maintenance to the Pi 5's noncoherent PCIe interface. It can bind
to a selected PCI controller or the standard NVMe PCI class.

The patch recipe requires original Microsoft `stornvme.sys` version
10.0.26100.9539 and its matching `stornvme.inf`. Exact input and output hashes
are checked by `build.py`. Those Microsoft inputs are not stored in this
source repository.
