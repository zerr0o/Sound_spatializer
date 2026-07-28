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

Every engine-to-client status or command result is UTF-8 JSON preceded by its
little-endian `uint32` byte length and uses the same 1 MiB limit. An invalid
length, pose CRC, schema version or non-finite pose closes or rejects the
current message without applying partial state.

## Commands

All commands contain `{"schemaVersion":1,"commandId":N,"type":"..."}` plus the
fields below. `commandId` is assigned by the desktop host; legacy local callers
may omit it and implicitly use zero.

| Type | Additional fields |
| --- | --- |
| `start`, `stop`, `shutdown` | none |
| `set-bypass` | `enabled: boolean` |
| `set-output-device` | `deviceId: string` |
| `set-audio-route` | `captureProvider: native-driver \| external-render`, `captureEndpointId: string\|null`, `outputDeviceId: string` |
| `set-audio-mode` | `mode: shared-low-latency \| exclusive-pro \| compatibility` |
| `calibrate-neutral-pose` | `quaternion: [w,x,y,z]` |
| `set-scene` | `scene: SceneConfigV2` |
| `set-hrtf` | `profileId: string`, `sofaPath: string|null` |
| `set-headphone-eq` | `eq: {enabled,preampDb,bands}` |
| `set-window-spatialization` | `config: WindowSpatializationConfigV1` |

`shutdown` releases the audio devices and ends the engine process. It exists
because the engine can outlive the interface: it may have been started by the
session or by an earlier run, so a host that is quitting cannot rely on
terminating a child process it does not necessarily own. The engine
acknowledges the command before tearing its IPC server down. A host that owns
the process should still wait a bounded time for the exit and terminate it as a
last resort.

Every parsed command whose non-zero `commandId` can be recovered produces
exactly one response on the duplex pipe:
`{"schemaVersion":1,"kind":"command-result","commandId":N,"accepted":boolean,"persisted":boolean,"error":string}`.
The desktop host resolves the matching pending invocation from this frame and
does not treat successful pipe writing as successful engine application. A
rejected scene or window mode is therefore rolled back in the UI and is not
persisted as if it were active.

`accepted:true,persisted:false` means the live engine already uses the new
state but its native atomic configuration write failed. The desktop keeps the
applied state, reports the persistence failure and retains its own canonical
configuration for deterministic replay. A command whose result times out is
reported as indeterminate, never as rejected: the desired state is retained
locally for the next deterministic synchronization. The generic transport
never retries a timed-out command with a fresh ID because a newer `stop`,
`start`, or route change could otherwise be overtaken. Legacy commands with
an omitted/zero ID retain their historical one-way behavior and receive no
result frame, so an older client cannot mistake an ACK for a status object.

Enabling `set-window-spatialization` requires an ACK-capable engine. A desktop
host connected to a surviving legacy engine refuses that activation explicitly
and asks for a complete application restart instead of silently treating the
legacy engine's rejection as success. Sending the disabled state may retain the
legacy one-way form because an engine without the feature is already
semantically disabled.

The WebView bridge serializes commands through one FIFO. Multi-command route,
replay and configuration changes acquire that FIFO as an indivisible
transaction, and publish their canonical store update before releasing it.
This prevents a supervised replay from interleaving between `set-scene`,
`set-audio-route` and `start`. A configuration timeout releases the global
transaction before any bounded retry so a fail-safe `stop` can proceed.

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

`set-window-spatialization` switches between the endpoint mix and a bounded
process-loopback bank. Candidates originate from audio sessions, then sessions
sharing a PID and child processes already included by a parent capture are
deduplicated into process trees. Each retained tree keeps independent
left/right channels; the two emitters are projected onto its associated
calibrated window rectangle. The global
endpoint mix remains the fail-safe render until every currently observed active
session tree is assigned and ready. An active system, excluded, disabled,
overflowing, failed or still-activating source keeps the complete endpoint path
authoritative. Only a completely covered process set may replace it, so the
engine never renders a known partial set that would mute uncovered sources or
duplicate covered ones. Runtime PID/HWND associations are advisory and may
require an explicit application rule when several windows share a process. UI
application names are labels for these session/process-tree captures, not
guaranteed one-to-one audio-source identities.

## Status

`EngineStatusV1` is a flat JSON object containing `schemaVersion: 1`, capture and
render states, tracking state, `commandAckVersion: 1`, the effective `audioMode`, the effective
`renderSampleFormat` (`unknown`, `float32` or `pcm-s32`), `inputLayout`,
`captureChannels`, `captureChannelMask`, both sample rates and periods, FIFO fill, xrun
count, callback load, tracking rate, software latency percentiles, ASRC ratio and
the last diagnostic error. It also carries `potentiallyBinaural`, a bounded,
hysteretic warning heuristic; it never changes routing or enables bypass without
an explicit user command. Stream states are `stopped`, `starting`, `running`,
`degraded` or `failed`; tracking is `lost`, `tracking`, `held` or
`returning-to-neutral`.

`requestedSpatialInputMode` describes the configured endpoint/process-window
choice. `spatialInputMode` describes the path actually rendered by the latest
audio callback. During process-loopback activation, incomplete active-session
coverage or after all process captures disappear, these fields are respectively
`process-windows` and `endpoint-mix`; diagnostics can therefore show the
audible fallback instead of claiming a partial per-application matrix.

The optional `windowAudio.diagnostics.fifoOverruns` and `fifoUnderruns`
counters aggregate discontinuities from the independent process-loopback
capture FIFOs. Missing fields mean zero for compatibility with an older engine;
present values must be non-negative integers.

The desktop host adds `desktopConnectionGeneration` when returning a native
status to the WebView. It is monotonic for the host lifetime and changes after a
pipe reconnection. The UI uses it to replay its canonical scene, window mode and
audio route exactly once after a supervised engine restart.

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
