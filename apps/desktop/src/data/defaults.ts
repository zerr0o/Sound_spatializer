import type {
  AppPreferences,
  EngineStatusV1,
  SceneConfigV1,
  SurfaceAcoustics,
  TrackingMetrics,
} from '../types/contracts';

const material = (
  materialId: string,
  absorption: [number, number, number],
  diffusion: [number, number, number],
): SurfaceAcoustics => ({ materialId, absorption, diffusion });

export const defaultScene: SceneConfigV1 = {
  version: 1,
  speakers: [
    {
      id: 'L',
      channel: 'L',
      label: 'Enceinte gauche',
      position: { x: -1, y: 1.2, z: 1.732051 },
      gainDb: 0,
      muted: false,
    },
    {
      id: 'R',
      channel: 'R',
      label: 'Enceinte droite',
      position: { x: 1, y: 1.2, z: 1.732051 },
      gainDb: 0,
      muted: false,
    },
  ],
  listener: {
    position: { x: 0, y: 1.2, z: 0 },
    neutralPose: { x: 0, y: 0, z: 0, w: 1 },
  },
  hrtfProfileId: 'sadie-d2-kemar',
  importedSofaHash: null,
  importedSofaPath: null,
  headphoneEq: {
    enabled: false,
    preampDb: -6,
    profileName: null,
    bands: [],
  },
  audioMode: 'shared-low-latency',
  captureProvider: 'native-driver',
  captureEndpointId: null,
  physicalOutputDeviceId: null,
  masterGainDb: -6,
  directRoomMix: 0.18,
  bypass: false,
  room: {
    enabled: false,
    dimensions: { width: 4.8, length: 6.2, height: 2.7 },
    earlyReflectionOrder: 2,
    earlyWindowMs: 80,
    lateReverbEnabled: true,
    surfaces: {
      floor: material('wood', [0.15, 0.11, 0.1], [0.1, 0.1, 0.1]),
      ceiling: material('plaster', [0.1, 0.05, 0.04], [0.1, 0.1, 0.1]),
      front: material('plaster', [0.1, 0.05, 0.04], [0.05, 0.05, 0.05]),
      rear: material('curtain', [0.35, 0.55, 0.72], [0.05, 0.05, 0.05]),
      left: material('plaster', [0.1, 0.05, 0.04], [0.05, 0.05, 0.05]),
      right: material('plaster', [0.1, 0.05, 0.04], [0.05, 0.05, 0.05]),
    },
  },
};

export const defaultPreferences: AppPreferences = {
  language: 'fr',
  onboardingComplete: false,
  onboardingStep: 0,
  startWithWindows: true,
  minimizeToTray: true,
  showCameraPreview: false,
};

export const emptyEngineStatus: EngineStatusV1 = {
  version: 1,
  audioMode: 'shared-low-latency',
  renderSampleFormat: 'unknown',
  connection: 'offline',
  captureActive: false,
  renderActive: false,
  trackingActive: false,
  trackingHz: 0,
  physicalOutputName: null,
  sampleRate: 48_000,
  blockFrames: 128,
  capturePeriodMs: 2.67,
  renderPeriodMs: 2.67,
  fifoFillPercent: 50,
  fifoFillFrames: 0,
  xruns: 0,
  callbackCpuPercent: 0,
  engineCpuPercent: 0,
  motionToSoundLatencyMs: { p50: 0, p95: 0 },
  audioPipelineLatencyMs: 0,
  clockDriftPpm: 0,
  uptimeSeconds: 0,
  potentiallyBinaural: false,
  lastError: null,
};

export const emptyTrackingMetrics: TrackingMetrics = {
  state: 'idle',
  fps: 0,
  processingMs: 0,
  cameraLabel: null,
  cameraFrameRate: null,
  permission: 'prompt',
  pose: null,
  euler: { yaw: 0, pitch: 0, roll: 0 },
  error: null,
};

export const demoAudioDevices = [
  {
    id: 'preview-usb',
    name: 'Casque USB (aperçu)',
    isDefault: true,
    isSoundSpatializerEndpoint: false,
    transport: 'usb' as const,
    sampleRate: 48_000,
  },
  {
    id: 'preview-external-render',
    name: 'Câble audio virtuel (aperçu)',
    isDefault: false,
    isSoundSpatializerEndpoint: false,
    transport: 'unknown' as const,
    sampleRate: 48_000,
  },
];
