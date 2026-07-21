# Third-party build tooling

The installer project restores WiX Toolset SDK 5.0.2 from NuGet at build time.
WiX 5.0.2 is licensed under the Microsoft Reciprocal License (MS-RL). The WiX
tool binaries are not packaged into Sound Spatializer.

- Project: https://github.com/wixtoolset/wix/tree/v5.0.2
- License: https://github.com/wixtoolset/wix/blob/v5.0.2/LICENSE.TXT

WiX 6 and later are intentionally not selected because those binary releases
participate in the Open Source Maintenance Fee. Upgrading requires an explicit
license review and, for WiX 7+, explicit EULA acceptance.
