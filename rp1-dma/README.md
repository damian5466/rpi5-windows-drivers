# RP1 DMA copy service

`Pi5Dma.sys` binds to `ACPI\RPI0004`. It provides a bounded memory-to-memory
copy foundation on RP1's eight-channel AXI DMAC. The current implementation
owns the controller and uses channel 0 only; it refuses a boot-active channel
or an unrecognized component revision. Tested RP1 reports `COMPVER=0x3130332a`
(1.03a), although the Linux compatible string identifies the 1.01a interface.

Install RP1 interrupt service 0.4.0.0, clock service 0.3.0.0, and matching
firmware with DMA dependencies on both providers. DMA holds source 40 and
clock leases while active. The tested firmware's existing 100 MHz clock is
preserved. No external wiring is required for memory copies.

The service allocates a 140 KiB noncached HAL common buffer through the ACPI
PDO and uses its logical DMA addresses. Descriptor, source, destination and
guard regions are separate. A transfer is limited to 64 KiB and 500 ms;
interrupt completion is followed by a bounded channel halt. If both halt and
controller reset fail, the driver fails the device and retains the allocation
until reboot to prevent DMA into freed memory.

`common/rp1-dma.h` defines the private cooperating-driver interface:

- `IOCTL_RP1_DMA_COPY`: kernel callers submit a versioned byte payload and
  receive the copied bytes. DMA addresses remain private to the service.
- `IOCTL_RP1_DMA_QUERY`: read-only controller state and counters.
- `IOCTL_RP1_DMA_SELFTEST`: an administrator can request 24 fixed copies,
  lengths 1–65,536, with aligned/misaligned endpoints and guard checks.

The `\\.\Pi5Dma` endpoint permits administrators and SYSTEM only. User-mode
copy submissions and undersized outputs are rejected. The self-test exercises
the same DMA engine used by the copy endpoint; an external kernel consumer
has not yet been integrated.

Debug hardware validation passed 24 transfers, three PnP restarts followed by
the same checks, and 288 transfers alongside a 128 MiB Ethernet/SD round trip.
There were no data, guard, or network errors. Host sanitizers cover descriptor
bounds, alignment, overflow, register bounds and bounded halt/reset paths.

This is not a Windows DMA-controller extension, a streaming API, or a
peripheral handshake implementation. SPI/I2S/PIO DMA clients, channel
arbitration, suspend/resume and performance tuning remain future work.
I2S audio additionally requires a real endpoint device and wiring.

Build: `.\build.ps1 -Driver rp1-dma -Configuration Debug -Analyze`.

Register facts were checked against the [RP1 peripheral specification](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf)
and Raspberry Pi's `dw-axi-dmac` reference driver. This implementation is
original BSD-2-Clause-Patent code, not a port of the Linux implementation.
