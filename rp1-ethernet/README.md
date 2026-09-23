# RP1 Ethernet driver

Experimental Windows ARM64 NDIS 6.30 miniport for the RP1 Ethernet controller
exposed by the matching firmware as `ACPI\RPI0001`.

RX and TX completions use the shared, level-triggered GIC interrupt from ACPI.
The miniport leases RP1 local source 6 from `Pi5Rp1.sys`, acknowledges GEM's
own interrupt status, and services the descriptor rings in an NDIS DPC. It
returns unclaimed shared interrupts to the other RP1 clients. Receive batches
are bounded and respect NDIS receive throttling; interrupts stay masked until
the batch is drained, with pending hardware events preserved when rearming.

There is no periodic packet polling or polling fallback. A passive worker
checks PHY negotiation and hardware error counters once per second and detects
stalled sends after five seconds. DMA errors and stalled sends stop/drain the
adapter and reinitialize its rings. Pause, power-down and halt mask interrupts
and wait for active DPC indications before reclaiming buffers.

## Requirements

Install the RP1 interrupt service version 0.3.0.0 or newer and matching firmware
with `ETH0._DEP` referencing `IRQ0`. Initialization fails if the provider cannot
grant the route; it never enables PCIe forwarding itself. Install the provider
before Ethernet, or use `install-rp1.ps1` for the complete RP1 bundle.

The driver supports 10/100/1000 Mbps full-duplex autonegotiation, a 1500-byte
MTU, multicast filtering and network-address override. It currently uses one
queue with copied DMA buffers and synchronous receive indications. RSS,
checksum/segmentation offloads, jumbo frames, configurable interrupt moderation
and wake-on-LAN are not implemented.
The copied single-queue receive path can exhaust buffers under concurrent
saturating traffic; monitor the standard discard counters and `DiagRxResourceLow` /
`DiagRxOverrunLow`. Successful TCP transfers do not imply zero packet drops.

See the [repository README](../README.md) for shared build prerequisites and
signing instructions. Run the commands below from the repository root.

## Build

```powershell
.\build.ps1 -Driver rp1-service -Configuration Debug -Analyze
.\build.ps1 -Driver rp1-ethernet -Configuration Debug -Analyze
```

The package is written to `Build/rp1-ethernet`, with `Pi5Ethernet.sys` and
`pi5ethernet.inf`.

## Diagnostics

The adapter's network class registry key contains `DiagInterruptMode` (1 for
hardware interrupts), `DiagInterruptRouteStatus`, `DiagInterruptClearOnRead`,
`DiagInterrupts`, `DiagInterruptDpcs`, `DiagRxInterrupts`, `DiagTxInterrupts`,
`DiagRecoveries` and `DiagLastRecovery`. Counters are refreshed every ten seconds;
interrupt counts are the low 32 bits of per-adapter totals. Recovery reasons
are GEM fatal interrupt bits, bit 31 for a stalled send, or bit 30 for a TX
ownership invariant failure. Standard NDIS statistics contain packet/byte/error
totals. Debug builds retain the same datapath with optimization disabled.

## Primary references

- [Microsoft NDIS interrupt registration](https://learn.microsoft.com/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismregisterinterruptex)
- [Microsoft receive throttling contract](https://learn.microsoft.com/windows-hardware/drivers/ddi/ndis/ns-ndis-_ndis_receive_throttle_parameters)
- [Raspberry Pi Linux GEM driver](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/net/ethernet/cadence/macb_main.c)
- [GEM register definitions](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/net/ethernet/cadence/macb.h)
