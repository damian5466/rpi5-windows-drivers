# VideoCore firmware clocks

`Pi5Fclk.sys` binds to `ACPI\RPI1030` and uses the Pi 5 mailbox service to
query the firmware state, rate, and limits of the V3D, core, and display clocks.
It depends on `Pi5Mailbox.sys`.

Kernel clients can hold leases on clocks 4, 5, 13, 14, and 16. The shared core
and display leases preserve the firmware configuration for the current display
owner. A sole V3D client may change clock 5 through the firmware; releasing its
lease restores the clock's original state and rate. The interface is declared in
[`common/pi5-fclk.h`](../common/pi5-fclk.h).

The reported `State` is the firmware's raw clock-state query. A successful
V3D enable request can leave that value at 0; the
`PI5_FCLK_FLAG_STATE_REQUESTED` flag records a matching firmware reply to the
current lease's enable request. It does not prove that the physical clock or
V3D power domain is on.

A V3D owner must stop its hardware work before explicitly releasing the lease.
If a modified lease closes abruptly, the provider retains the mailbox claim and
reports uncertain state until reboot.

Clients that hold a lease through a remote WDF I/O target must veto that
target's query-remove until they have explicitly released the lease. The
service likewise vetoes query-remove of its open mailbox target to preserve
firmware clock ownership.
