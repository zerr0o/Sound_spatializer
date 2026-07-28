# Component contracts

The desktop host and the real-time engine are separate processes. Audio samples
never cross the UI boundary. Control and status messages use length-prefixed JSON
over a per-user named pipe; head-pose updates use the fixed binary packet described
in `head-pose-v1.md`.

`scene-config-v2.schema.json` is the current persisted and wire-level scene
contract. `scene-config-v1.schema.json` remains immutable for migration. V2 adds
the explicit `stereo`/`5.1-surround` input layout, five positional channels in
canonical order, and a separate non-positional LFE channel.

`window-spatialization-v1.schema.json` is an independent, optional runtime
contract. It selects process/window capture without changing the speaker-bed
scene schema, caps the dynamic source bank at eight stereo audio sessions
deduplicated by process tree, and stores only durable display
calibrations/application rules. An application label is therefore not a
one-to-one promise: a capture may include its child processes. PIDs and HWNDs
are runtime telemetry and are never persisted. The canonical initial
`stereoSpread` is `0.72`.

`audio.captureProvider` and `audio.captureEndpointId` are optional V1 additions.
Their absence is exactly equivalent to `native-driver` and `null`, so an older
canonical scene remains valid. `external-render` always carries a non-empty,
explicitly selected WASAPI render endpoint ID; `native-driver` never carries an
external ID. Source provider, source endpoint and physical output are changed
atomically by `set-audio-route`, and an invalid or looped tuple must not be
partially applied or persisted.

V2 wire speakers use `front-left`, `front-right`, `front-center`,
`surround-left`, `surround-right` and an explicit `enabled` flag. The LFE object
also uses `enabled`; UI models may expose a mute switch but must serialize it as
`enabled: !muted`. A 5.1 scene requires `external-render` with a non-empty
capture endpoint. Runtime activation additionally requires exactly six capture
channels and a Windows channel mask of `0x3F` or `0x60F`; no upmix is inferred.

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
