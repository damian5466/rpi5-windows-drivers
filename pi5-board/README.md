# Pi 5 board controls

`Pi5Board.sys` holds the board's Wi-Fi enable, SD power and Ethernet reset
signals in their operating states. It provides exclusive LED and camera-enable
leases, restoring released signals to their startup states. It does not
implement Wi-Fi networking or camera capture.

It requires matching firmware with board resource revision 2,
[bcm2712-gpio](../bcm2712-gpio/README.md) 0.1.0.3 or newer, and
[rp1-gpio](../rp1-gpio/README.md) 0.3.0.0 or newer.
