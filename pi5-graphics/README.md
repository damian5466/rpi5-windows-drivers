# VideoCore VII Direct3D and HDMI graphics

`Pi5Graphics.sys` supplies the Windows display and rendering driver for the
Pi 5's VideoCore VII GPU. `Pi5D3D.dll` translates Direct3D rendering commands
and shaders for the GPU. The package supports the native HDMI outputs,
monitor EDID, display mode changes and hotplug. `Pi5GraphicsPower.sys` keeps
the GPU and display providers powered in the required order.

It depends on [pi5-v3d](../pi5-v3d/README.md) 0.1.0.22 or newer,
[pi5-fclk](../pi5-fclk/README.md) 0.1.0.9 or newer,
[pi5-mailbox](../pi5-mailbox/README.md) 0.1.0.16 or newer, and matching
rpi5-uefi firmware exposing `ACPI\RPI1001` and the native display resources.
The V3D provider additionally requires the graph and reset-gate services.

The display framework retains its Microsoft Public License in
`display/LICENSE.txt`. EDID timing-table attribution is in
`display/EDID-LICENSE.txt`; both notices accompany the binary package.
