export const CONTRACT_VERSION = 1 as const;

export type ViewId = 'assistant' | 'scene' | 'profiles' | 'diagnostics';
export type Channel = 'L' | 'R';
export type TrackingState = 'tracked' | 'held' | 'returning' | 'lost';
export type AudioMode = 'shared-low-latency' | 'exclusive-pro' | 'compatibility';
export type CaptureProvider = 'native-driver' | 'external-render';
export type EngineConnectionState = 'offline' | 'connecting' | 'ready' | 'degraded' | 'error';

export interface Vector3 {
  x: number;
  y: number;
  z: number;
}

export interface Quaternion {
  x: number;
  y: number;
  z: number;
  w: number;
}

export interface EulerAngles {
  yaw: number;
  pitch: number;
  roll: number;
}

export interface HeadPoseSampleV1 {
  version: typeof CONTRACT_VERSION;
  sequence: number;
  timestampQpc: string;
  quaternion: Quaternion;
  angularVelocity: Vector3;
  confidence: number;
  trackingState: TrackingState;
}

export interface SpeakerConfig {
  id: Channel;
  channel: Channel;
  label: string;
  position: Vector3;
  gainDb: number;
  muted: boolean;
}

export interface ListenerConfig {
  position: Vector3;
  neutralPose: Quaternion;
}

export interface SurfaceAcoustics {
  materialId: string;
  absorption: [number, number, number];
  diffusion: [number, number, number];
}

export interface RoomConfig {
  enabled: boolean;
  dimensions: { width: number; length: number; height: number };
  earlyReflectionOrder: 0 | 1 | 2;
  earlyWindowMs: number;
  lateReverbEnabled: boolean;
  surfaces: {
    floor: SurfaceAcoustics;
    ceiling: SurfaceAcoustics;
    front: SurfaceAcoustics;
    rear: SurfaceAcoustics;
    left: SurfaceAcoustics;
    right: SurfaceAcoustics;
  };
}

export interface HeadphoneEqBand {
  id: string;
  enabled: boolean;
  type: 'peak' | 'low-shelf' | 'high-shelf';
  frequencyHz: number;
  gainDb: number;
  q: number;
}

export interface HeadphoneEqConfig {
  enabled: boolean;
  preampDb: number;
  profileName: string | null;
  bands: HeadphoneEqBand[];
}

export interface SceneConfigV1 {
  version: typeof CONTRACT_VERSION;
  speakers: [SpeakerConfig, SpeakerConfig];
  listener: ListenerConfig;
  hrtfProfileId: string;
  importedSofaHash: string | null;
  importedSofaPath: string | null;
  headphoneEq: HeadphoneEqConfig;
  audioMode: AudioMode;
  captureProvider: CaptureProvider;
  captureEndpointId: string | null;
  physicalOutputDeviceId: string | null;
  masterGainDb: number;
  directRoomMix: number;
  bypass: boolean;
  room: RoomConfig;
}

export interface AudioDeviceSummary {
  id: string;
  name: string;
  isDefault: boolean;
  isSoundSpatializerEndpoint: boolean;
  transport: 'usb' | 'jack' | 'bluetooth' | 'hdmi' | 'unknown';
  sampleRate: number;
}

export interface EngineStatusV1 {
  version: typeof CONTRACT_VERSION;
  /** Mode réellement ouvert par WASAPI, qui peut différer du mode demandé après un fallback. */
  audioMode: AudioMode;
  /** Représentation réellement écrite dans le tampon du périphérique physique. */
  renderSampleFormat: 'unknown' | 'float32' | 'pcm-s32';
  connection: EngineConnectionState;
  captureActive: boolean;
  renderActive: boolean;
  trackingActive: boolean;
  trackingHz: number;
  physicalOutputName: string | null;
  sampleRate: number;
  blockFrames: number;
  capturePeriodMs: number;
  renderPeriodMs: number;
  fifoFillPercent: number;
  fifoFillFrames: number;
  xruns: number;
  callbackCpuPercent: number;
  engineCpuPercent: number;
  motionToSoundLatencyMs: { p50: number; p95: number };
  audioPipelineLatencyMs: number;
  clockDriftPpm: number;
  uptimeSeconds: number;
  potentiallyBinaural: boolean;
  lastError: string | null;
}

export type EngineCommandV1 =
  | { version: 1; type: 'start' }
  | { version: 1; type: 'stop' }
  | { version: 1; type: 'set-bypass'; enabled: boolean }
  | { version: 1; type: 'set-output-device'; deviceId: string }
  | {
      version: 1;
      type: 'set-audio-route';
      captureProvider: CaptureProvider;
      captureEndpointId: string | null;
      outputDeviceId: string;
    }
  | { version: 1; type: 'set-audio-mode'; mode: AudioMode }
  | { version: 1; type: 'calibrate-neutral-pose'; quaternion: Quaternion }
  | { version: 1; type: 'set-scene'; scene: SceneConfigV1 }
  | { version: 1; type: 'set-hrtf'; profileId: string; sofaPath: string | null }
  | { version: 1; type: 'set-headphone-eq'; eq: HeadphoneEqConfig };

export interface TrackingMetrics {
  state: 'idle' | 'requesting' | 'starting' | 'running' | 'unavailable' | 'error';
  fps: number;
  processingMs: number;
  cameraLabel: string | null;
  cameraFrameRate: number | null;
  permission: 'prompt' | 'granted' | 'denied';
  pose: HeadPoseSampleV1 | null;
  euler: EulerAngles;
  error: string | null;
}

export interface AppPreferences {
  language: 'fr' | 'en';
  onboardingComplete: boolean;
  onboardingStep: number;
  startWithWindows: boolean;
  minimizeToTray: boolean;
  showCameraPreview: boolean;
}

export interface PersistedAppConfigV1 {
  schemaVersion: 1;
  scene: SceneConfigV1;
  preferences: AppPreferences;
}

export interface QpcSnapshot {
  ticks: string;
  frequency: string;
}

export interface ImportedSofa {
  fileName: string;
  hash: string;
  localPath: string;
}

export interface ImportedHeadphoneEq {
  fileName: string;
  format: 'sound-spatializer-json-v1' | 'equalizer-apo';
  profileName: string;
  preampDb: number;
  bands: HeadphoneEqBand[];
}
