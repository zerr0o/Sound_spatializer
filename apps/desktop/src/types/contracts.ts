export const CONTRACT_VERSION = 1 as const;
export const SCENE_CONFIG_VERSION = 2 as const;
export const WINDOW_SPATIALIZATION_CONFIG_VERSION = 1 as const;

export type ViewId = 'assistant' | 'scene' | 'profiles' | 'diagnostics';
export type Channel = 'L' | 'R' | 'C' | 'LS' | 'RS';
export type InputLayout = 'stereo' | '5.1-surround';
export type SpatialInputMode = 'endpoint-mix' | 'process-windows';
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

export interface SpeakerConfig<C extends Channel = Channel> {
  id: C;
  channel: C;
  label: string;
  position: Vector3;
  gainDb: number;
  muted: boolean;
}

export type SpeakerSet = [
  SpeakerConfig<'L'>,
  SpeakerConfig<'R'>,
  SpeakerConfig<'C'>,
  SpeakerConfig<'LS'>,
  SpeakerConfig<'RS'>,
];

export interface LfeConfig {
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

export interface SceneConfigV2 {
  version: typeof SCENE_CONFIG_VERSION;
  inputLayout: InputLayout;
  speakers: SpeakerSet;
  lfe: LfeConfig;
  listener: ListenerConfig;
  hrtfProfileId: string;
  /** Removes the comb two coherent virtual emitters impose on correlated content. */
  phantomCentreCompensation: boolean;
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

export interface DisplayBoundsPx {
  x: number;
  y: number;
  width: number;
  height: number;
}

/**
 * Écran découvert par le moteur Win32. Les coordonnées physiques sont celles
 * effectivement utilisées pour placer les sources dans le monde HRTF.
 */
export interface DisplayRuntimeInfo {
  displayId: string;
  name: string;
  isPrimary: boolean;
  boundsPx: DisplayBoundsPx;
  rotationDegrees: 0 | 90 | 180 | 270;
  center: Vector3;
  widthM: number;
  heightM: number;
  orientation: Quaternion;
  calibrated: boolean;
}

/** Une session audio stéréo associée à la fenêtre choisie par le moteur. */
export interface WindowAudioSourceInfo {
  sourceId: string;
  applicationId: string;
  applicationName: string;
  windowTitle: string;
  processId: number;
  displayId: string | null;
  active: boolean;
  leftPosition: Vector3;
  rightPosition: Vector3;
  gainDb: number;
  sampleRate: number;
  channelCount: number;
  captureState: 'inactive' | 'activating' | 'capturing' | 'unsupported-format' | 'failed';
}

export interface WindowAudioRuntimeStatus {
  supported: boolean;
  running: boolean;
  sourceCount: number;
  /** Paquets perdus parce qu'une FIFO de capture process-loopback était pleine. */
  fifoOverruns: number;
  /** Lectures sans assez de PCM dans une FIFO de capture process-loopback. */
  fifoUnderruns: number;
  displays: DisplayRuntimeInfo[];
  windowSources: WindowAudioSourceInfo[];
  lastError: string | null;
}

export interface DisplaySpatialCalibration {
  displayId: string;
  center: Vector3;
  widthM: number;
  heightM: number;
  orientation: Quaternion;
}

export interface WindowSourceRule {
  applicationId: string;
  enabled: boolean;
  gainDb: number;
  stereoSpread: number;
  fallbackDisplayId: string | null;
}

export type WindowEmitterPlacementMode = 'proportional' | 'window-edges';

export interface WindowSpatializationConfigV1 {
  version: typeof WINDOW_SPATIALIZATION_CONFIG_VERSION;
  enabled: boolean;
  /** Nombre maximal de sessions simultanées ; le moteur borne cette valeur à huit. */
  maxSources: number;
  /** Largeur stéréo globale, de mono centré (0) à toute la largeur de la fenêtre (1). */
  stereoSpread: number;
  /** Placement pondéré par la largeur ou exactement sur les bords gauche/droit. */
  emitterPlacementMode: WindowEmitterPlacementMode;
  followWindowPosition: boolean;
  displayCalibrations: DisplaySpatialCalibration[];
  sourceRules: WindowSourceRule[];
}

export interface AudioDeviceSummary {
  id: string;
  name: string;
  isDefault: boolean;
  isSoundSpatializerEndpoint: boolean;
  transport: 'usb' | 'jack' | 'bluetooth' | 'hdmi' | 'unknown';
  sampleRate: number;
  channelCount: number;
  channelMask: number;
}

export interface EngineStatusV1 {
  version: typeof CONTRACT_VERSION;
  /** Monotonic desktop pipe generation used to replay state after engine restart. */
  connectionGeneration: number;
  /** Mode réellement ouvert par WASAPI, qui peut différer du mode demandé après un fallback. */
  audioMode: AudioMode;
  /** Représentation réellement écrite dans le tampon du périphérique physique. */
  renderSampleFormat: 'unknown' | 'float32' | 'pcm-s32';
  inputLayout: InputLayout;
  /** Path used by the latest audio callback. */
  spatialInputMode: SpatialInputMode;
  /** User-requested path, even while process capture is still activating. */
  requestedSpatialInputMode: SpatialInputMode;
  captureChannels: number;
  captureChannelMask: number;
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
  windowAudio: WindowAudioRuntimeStatus;
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
  | { version: 1; type: 'set-window-spatialization'; config: WindowSpatializationConfigV1 }
  | { version: 1; type: 'calibrate-neutral-pose'; quaternion: Quaternion }
  | { version: 1; type: 'set-scene'; scene: SceneConfigV2 }
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

export interface PersistedAppConfigV2 {
  schemaVersion: 2;
  scene: SceneConfigV2;
  preferences: AppPreferences;
}

export interface PersistedAppConfigV3 {
  schemaVersion: 3;
  scene: SceneConfigV2;
  preferences: AppPreferences;
  windowSpatialization: WindowSpatializationConfigV1;
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
