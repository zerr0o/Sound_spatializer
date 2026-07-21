# `HeadPoseSampleV1`

All integer values are little-endian and all floating-point values are IEEE-754.
The packet is exactly 64 bytes and starts with the ASCII magic `SSP1`.

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `char[4]` | Magic `SSP1` |
| 4 | `uint16` | Schema version (`1`) |
| 6 | `uint16` | Tracking state: `0` lost, `1` acquiring, `2` tracking |
| 8 | `uint64` | Monotonic sequence |
| 16 | `int64` | Native QPC timestamp |
| 24 | `float[4]` | Quaternion `(w, x, y, z)` |
| 40 | `float[3]` | Angular velocity `(x, y, z)` in rad/s |
| 52 | `float` | Confidence `[0, 1]` |
| 56 | `uint32` | Flags, reserved and currently zero |
| 60 | `uint32` | CRC32 of bytes 0–59, or zero on trusted local transports |

The quaternion is unit length and ordered `(w, x, y, z)`. It rotates vectors from
head-local coordinates into the calibrated world frame; neutral pose is
`(1, 0, 0, 0)`. The engine therefore applies its inverse when transforming a
world-fixed speaker direction into the listener frame.
