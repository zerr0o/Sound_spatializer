# Component contracts

The desktop host and the real-time engine are separate processes. Audio samples
never cross the UI boundary. Control and status messages use length-prefixed JSON
over a per-user named pipe; head-pose updates use the fixed binary packet described
in `head-pose-v1.md`.

`scene-config-v1.schema.json` is the persisted and wire-level scene contract. Any
future incompatible change must introduce a new schema version and an explicit
migration instead of silently reinterpreting existing values.

`audio.captureProvider` and `audio.captureEndpointId` are optional V1 additions.
Their absence is exactly equivalent to `native-driver` and `null`, so an older
canonical scene remains valid. `external-render` always carries a non-empty,
explicitly selected WASAPI render endpoint ID; `native-driver` never carries an
external ID. Source provider, source endpoint and physical output are changed
atomically by `set-audio-route`, and an invalid or looped tuple must not be
partially applied or persisted.

Capture routing is independent from `audio.mode`. In particular,
`mode: compatibility` means only the 256-frame buffer target; it does not enable
`captureProvider: external-render`. External endpoints are never inferred from
a friendly name, installed or redistributed by this project, and remain an
unqualified compatibility path until they pass `docs/VALIDATION.md`.

`headphoneEq.preampDb` is persisted even while the EQ is disabled. It is applied
only when the EQ becomes active; the factory value is -6 dB so enabling a
positive filter does not silently remove the safety headroom.

World coordinates are right-handed: X points right, Y points up and Z points
forward. The room floor is Y=0 and its X/Z footprint is centred on the origin.
The six room surfaces always use this wire order: `-X`, `+X`, `-Z`, `+Z`,
`-Y` (floor), `+Y` (ceiling). Implementations must not infer surface identity
from a material name.
