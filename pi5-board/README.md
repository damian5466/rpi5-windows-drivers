# Pi 5 board controls

`Pi5Board.sys` binds `RPI1025` with firmware resource revision 2. It requires
BCM2712 GPIO 0.1.0.3 or later and RP1 GPIO 0.3.0.0 or later. It rejects the
older grouped GPIO resource layout.

Wi-Fi enable, SD power and Ethernet reset are held high. A 150 ms Wi-Fi
settling delay follows power-up. This driver does not implement WLAN, camera
capture or an Ethernet reset protocol.

Seven resource indices are defined in `board.h`:

| Index | Signal | GPIO | Active level | Client access |
| --- | --- | --- | --- | --- |
| 0 | Wi-Fi enable | BCM main 28 | High | Always enabled |
| 1 | SD power | BCM AON 4 | High | Always enabled |
| 2 | Activity LED | BCM AON 9 | Low | Administrator or kernel |
| 3 | Ethernet PHY reset | RP1 32 | Low | Held deasserted |
| 4 | Camera 0 enable | RP1 34 | High | Kernel only |
| 5 | Power LED | RP1 44 | Low | Administrator or kernel |
| 6 | Camera 1 enable | RP1 46 | High | Kernel only |

Open `\\.\Pi5Board` with read access for `IOCTL_PI5_BOARD_QUERY`, and write
access for `SET`/`RELEASE`. Each LED or camera connection belongs exclusively
to its open file handle. Closing the handle or releasing the connection
restores the GPIO controller's startup state. Camera clients must keep their
handle open for the duration of use. Active leases prevent a normal PnP stop
or removal. Device access is restricted to administrators and SYSTEM.

`QUERY` returns physical GPIO readback for configured connections; `SET`
takes logical assertion, so 1 lights an LED. Wi-Fi, SD and Ethernet writes
are denied. The service never claims the SD-voltage select or fan pins.

```powershell
.\build.ps1 -Driver pi5-board -Configuration Debug -Analyze
.\Pi5BoardTool.exe status
.\Pi5BoardTool.exe led activity on 5
.\Pi5BoardTool.exe led power off 5
```

The LED command holds the selected state for the requested seconds and then
restores its startup state. Run the tool from an elevated terminal.
