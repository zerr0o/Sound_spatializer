# Engine protocol V1

The per-user, per-session control/status byte pipe is
`\\.\pipe\SoundSpatializer.Engine.v1.<SID>.<SessionId>`. Low-latency head poses
use the independent, inbound-only pipe
`\\.\pipe\SoundSpatializer.Pose.v1.<SID>.<SessionId>`, so a JSON command or a
status write can never delay pose delivery. Both components derive
`<SID>` from their current process token and `<SessionId>` with
`ProcessIdToSessionId`; neither value comes from untrusted command-line or JSON
input. The server creates the first instance only, accepts one local client,
rejects remote clients and grants access only to the SID of the engine process
owner. PCM audio never uses this transport.

## Framing

The pose pipe carries only consecutive, fixed-size 64-byte
`HeadPoseSampleV1` packets. The engine pipe uses four-byte framing:

- `SSP1` starts the remaining 60 bytes of a legacy `HeadPoseSampleV1` packet;
- any other little-endian `uint32` is the UTF-8 JSON payload length, from 1 byte
  through 1 MiB inclusive.

The legacy pose framing remains accepted during the V1 migration. A single
monotonic sequence gate is shared by both inputs, so a delayed legacy packet
cannot overwrite a newer packet received on the dedicated pose pipe.

Every engine-to-client status is UTF-8 JSON preceded by its little-endian
`uint32` byte length and uses the same 1 MiB limit. An invalid length, pose CRC,
schema version or non-finite pose closes or rejects the current message without
applying partial state.

## Commands

All commands contain `{"schemaVersion":1,"type":"..."}` plus the fields below.

| Type | Additional fields |
| --- | --- |
| `start`, `stop` | none |
| `set-bypass` | `enabled: boolean` |
| `set-output-device` | `deviceId: string` |
| `set-audio-route` | `captureProvider: native-driver \| external-render`, `captureEndpointId: string\|null`, `outputDeviceId: string` |
| `set-audio-mode` | `mode: shared-low-latency \| exclusive-pro \| compatibility` |
| `calibrate-neutral-pose` | `quaternion: [w,x,y,z]` |
| `set-scene` | `scene: SceneConfigV2` |
| `set-hrtf` | `profileId: string`, `sofaPath: string|null` |
| `set-headphone-eq` | `eq: {enabled,preampDb,bands}` |

`set-audio-route` applies the capture provider, capture endpoint and physical
output as one transaction. `native-driver` requires a null capture endpoint and
discovers the Sound Spatializer endpoint from its vendor marker and contract
version. `external-render` requires a non-empty WASAPI render endpoint ID chosen
explicitly by the user; the engine opens that endpoint through event-driven
WASAPI loopback. It never guesses an external endpoint from its display name.
The source and physical output must resolve to different endpoints. An invalid,
missing or looped route is rejected without partially persisting or activating
it and without silently falling back to another endpoint.

The V2 scene carries `audio.inputLayout`, five ordered positional speakers and a
separate LFE object. Speaker and LFE activation is transported as `enabled`.
`5.1-surround` requires an explicit `external-render` endpoint; activation is
refused unless that endpoint exposes exactly six channels with mask `0x3F` or
`0x60F`. No stereo upmix is performed. V1 remains readable only for migration;
its optional capture fields default to `native-driver` and `null`. The
`audio.mode` value `compatibility` remains only the 256-frame buffer target.

The Tauri host resolves a bundled profile ID to a verified local SOFA path before
sending `set-scene` or `set-hrtf`. The native engine does not trust a display name
as proof that a profile is installed.

## Status

`EngineStatusV1` is a flat JSON object containing `schemaVersion: 1`, capture and
render states, tracking state, the effective `audioMode`, the effective
`renderSampleFormat` (`unknown`, `float32` or `pcm-s32`), `inputLayout`,
`captureChannels`, `captureChannelMask`, both sample rates and periods, FIFO fill, xrun
count, callback load, tracking rate, software latency percentiles, ASRC ratio and
the last diagnostic error. It also carries `potentiallyBinaural`, a bounded,
hysteretic warning heuristic; it never changes routing or enables bypass without
an explicit user command. Stream states are `stopped`, `starting`, `running`,
`degraded` or `failed`; tracking is `lost`, `tracking`, `held` or
`returning-to-neutral`.

`audioMode` is the mode that is actually open on the physical endpoint, not
merely the last requested value. If an exclusive opening is rejected but the
bounded shared-mode recovery succeeds, it is `shared-low-latency` and
`lastError` contains the stable non-fatal diagnostic prefix
`AUDIO_MODE_FALLBACK `. The streams remain active and are reported as
`degraded` so clients can persist the effective mode without mistaking the
capability fallback for a disconnection.

Software timestamps help decompose latency but are not a qualification result.
Motion-to-sound acceptance is measured with the external rig described in
`docs/VALIDATION.md`.

An external render endpoint is reported as a compatibility route, not as a
qualified Sound Spatializer driver. Neither this protocol nor the application
installs, downloads or redistributes third-party virtual-audio software.
