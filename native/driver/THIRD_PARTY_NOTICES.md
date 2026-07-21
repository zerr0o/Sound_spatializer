# Third-party notices

The build restores the Microsoft `Windows-driver-samples` repository at the
commit recorded by `scripts/Prepare-Sysvad.ps1`. The WaveRT/PortCls base is the
official SysVAD sample and is licensed under the MIT License published in that
repository:

- <https://github.com/microsoft/Windows-driver-samples>
- <https://github.com/microsoft/Windows-driver-samples/blob/main/LICENSE>

`overlay/` and `patches/0001-sysvad-render-history-loopback.patch` are adaptations of
that sample. They retain the same notice obligations. The upstream repository
is fetched into the ignored build dependency directory and is not represented
as original Sound Spatializer code.
