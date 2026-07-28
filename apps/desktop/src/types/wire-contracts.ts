import type {
  AppPreferences,
  AudioMode,
  CaptureProvider,
  InputLayout,
  SpatialInputMode,
} from './contracts';

export type WireVec3 = [number, number, number];
export type WireQuaternionWxyz = [number, number, number, number];

export interface WireBiquad {
  type: 'peaking' | 'low-shelf' | 'high-shelf' | 'low-pass' | 'high-pass';
  frequencyHz: number;
  q: number;
  gainDb: number;
}

export interface WireSurface {
  materialId: string;
  absorption: [number, number, number];
  diffusion: [number, number, number];
}

/** Représentation exacte de contracts/scene-config-v1.schema.json. */
export interface WireSceneConfigV1 {
  schemaVersion: 1;
  audio: {
    /** Optionnel uniquement pour relire les configurations V1 antérieures. */
    captureProvider?: CaptureProvider;
    /** Optionnel uniquement pour relire les configurations V1 antérieures. */
    captureEndpointId?: string | null;
    outputDeviceId: string | null;
    mode: AudioMode;
    sampleRate: 48_000;
    bufferFrames: 64 | 128 | 256;
    bypass: boolean;
    masterGainDb: number;
    roomMix: number;
  };
  tracking: {
    enabled: boolean;
    cameraDeviceId: string | null;
    minimumFps: number;
    predictionLimitMs: number;
  };
  listener: {
    positionM: WireVec3;
    neutralOrientation: WireQuaternionWxyz;
  };
  speakers: [
    { channel: 'left'; positionM: WireVec3; gainDb: number },
    { channel: 'right'; positionM: WireVec3; gainDb: number },
  ];
  hrtf: { profileId: string; sofaPath: string | null };
  headphoneEq: { enabled: boolean; preampDb: number; filters: WireBiquad[] };
  room: {
    enabled: boolean;
    lateReverbEnabled: boolean;
    dimensionsM: WireVec3;
    surfaces: [WireSurface, WireSurface, WireSurface, WireSurface, WireSurface, WireSurface];
    earlyReflectionOrder: 0 | 1 | 2;
    earlyReflectionLimitMs: number;
  };
}

export interface PersistedDesktopConfigV1 {
  schemaVersion: 1;
  scene: WireSceneConfigV1;
  preferences: AppPreferences;
}

export type WireSpeakerChannelV2 =
  | 'front-left'
  | 'front-right'
  | 'front-center'
  | 'surround-left'
  | 'surround-right';

export interface WireSpeakerV2 {
  channel: WireSpeakerChannelV2;
  positionM: WireVec3;
  gainDb: number;
  enabled: boolean;
}

/** Représentation exacte de contracts/scene-config-v2.schema.json. */
export interface WireSceneConfigV2 {
  schemaVersion: 2;
  audio: {
    captureProvider: CaptureProvider;
    captureEndpointId: string | null;
    outputDeviceId: string | null;
    mode: AudioMode;
    inputLayout: InputLayout;
    sampleRate: 48_000;
    bufferFrames: 64 | 128 | 256;
    bypass: boolean;
    masterGainDb: number;
    roomMix: number;
  };
  tracking: WireSceneConfigV1['tracking'];
  listener: WireSceneConfigV1['listener'];
  speakers: [WireSpeakerV2, WireSpeakerV2, WireSpeakerV2, WireSpeakerV2, WireSpeakerV2];
  lfe: { enabled: boolean; gainDb: number };
  hrtf: WireSceneConfigV1['hrtf'];
  headphoneEq: WireSceneConfigV1['headphoneEq'];
  room: WireSceneConfigV1['room'];
}

export interface PersistedDesktopConfigV2 {
  schemaVersion: 2;
  scene: WireSceneConfigV2;
  preferences: AppPreferences;
}

export interface WireDisplaySpatialCalibration {
  displayId: string;
  centerM: WireVec3;
  widthM: number;
  heightM: number;
  orientation: WireQuaternionWxyz;
}

export interface WireWindowSourceRule {
  applicationId: string;
  enabled: boolean;
  gainDb: number;
  stereoSpread: number;
  fallbackDisplayId: string | null;
}

/** Représentation exacte de contracts/window-spatialization-v1.schema.json. */
export interface WireWindowSpatializationConfigV1 {
  schemaVersion: 1;
  enabled: boolean;
  maxSources: number;
  stereoSpread: number;
  /** Optional for configs written before per-border placement was introduced. */
  emitterPlacementMode?: 'proportional' | 'window-edges';
  followWindowPosition: boolean;
  displayCalibrations: WireDisplaySpatialCalibration[];
  sourceRules: WireWindowSourceRule[];
}

export interface PersistedDesktopConfigV3 {
  schemaVersion: 3;
  scene: WireSceneConfigV2;
  preferences: AppPreferences;
  windowSpatialization: WireWindowSpatializationConfigV1;
}

export type WireStreamState = 'stopped' | 'starting' | 'running' | 'degraded' | 'failed';
export type WireTrackingState = 'lost' | 'tracking' | 'held' | 'returning-to-neutral';
export type WireAudioSampleFormat = 'unknown' | 'float32' | 'pcm-s32';

export interface WireDisplayRuntimeInfo {
  id: string;
  name: string;
  isPrimary: boolean;
  boundsPx: { left: number; top: number; right: number; bottom: number };
  centerM: WireVec3;
  widthM: number;
  heightM: number;
  orientation: WireQuaternionWxyz;
}

export interface WireWindowAudioSourceInfo {
  sourceId: string;
  applicationId: string;
  applicationName: string;
  windowTitle: string;
  processId: number;
  displayId: string;
  active: boolean;
  leftPositionM: WireVec3;
  rightPositionM: WireVec3;
  gainDb: number;
  sampleRate: number;
  channelCount: number;
  captureState: 'inactive' | 'activating' | 'capturing' | 'unsupported-format' | 'failed';
}

export interface WireEngineStatusV1 {
  schemaVersion: 1;
  /** Added by the desktop host, not serialized by the native engine. */
  desktopConnectionGeneration?: number;
  audioMode: AudioMode;
  /** Optional during a rolling upgrade from engines predating PCM32 telemetry. */
  renderSampleFormat?: WireAudioSampleFormat;
  /** Optional while interoperating with an engine predating multichannel capture. */
  inputLayout?: InputLayout;
  /** Optional while interoperating with an engine predating per-process capture. */
  spatialInputMode?: SpatialInputMode;
  /** Requested mode, distinct from the effective endpoint fallback. */
  requestedSpatialInputMode?: SpatialInputMode;
  captureChannels?: number;
  captureChannelMask?: number;
  windowAudio?: {
    supported: boolean;
    running: boolean;
    sourceCount: number;
    sequence: number;
    displays: WireDisplayRuntimeInfo[];
    windowSources: WireWindowAudioSourceInfo[];
    diagnostics: {
      discoveryPasses?: number;
      sessionsSeen?: number;
      captureStartFailures?: number;
      uncoveredActiveSessions?: number;
      fifoOverruns?: number;
      fifoUnderruns?: number;
      requiredActiveCaptures?: number;
      readyActiveCaptures?: number;
      coverageComplete?: boolean;
      endpointFallbackRequested?: boolean;
      coverageDetail?: string;
      lastError: string;
    };
  };
  captureState: WireStreamState;
  renderState: WireStreamState;
  trackingState: WireTrackingState;
  captureSampleRate: number;
  renderSampleRate: number;
  capturePeriodFrames: number;
  renderPeriodFrames: number;
  fifoFillFrames: number;
  xruns: number;
  callbackCpuPercent: number;
  trackingHz: number;
  latencyP50Ms: number;
  latencyP95Ms: number;
  resampleRatio: number;
  potentiallyBinaural: boolean;
  lastError: string;
}
