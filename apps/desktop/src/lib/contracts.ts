import { defaultPreferences, defaultScene } from '../data/defaults';
import type {
  EngineCommandV1,
  EngineStatusV1,
  HeadPoseSampleV1,
  PersistedAppConfigV2,
  Quaternion,
  SceneConfigV2,
  SpeakerConfig,
  SpeakerSet,
  SurfaceAcoustics,
} from '../types/contracts';
import type {
  PersistedDesktopConfigV2,
  WireEngineStatusV1,
  WireQuaternionWxyz,
  WireSceneConfigV1,
  WireSceneConfigV2,
  WireSpeakerChannelV2,
  WireSurface,
} from '../types/wire-contracts';

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
const finite = (value: unknown, fallback: number) => (typeof value === 'number' && Number.isFinite(value) ? value : fallback);
const vec = (value: readonly number[]): { x: number; y: number; z: number } => ({ x: value[0], y: value[1], z: value[2] });
const toVec = (value: { x: number; y: number; z: number }): [number, number, number] => [value.x, value.y, value.z];
const isFiniteTuple = (value: unknown, length: number): value is number[] =>
  Array.isArray(value) && value.length === length && value.every((item) => typeof item === 'number' && Number.isFinite(item));

const CHANNELS = ['L', 'R', 'C', 'LS', 'RS'] as const;
const WIRE_CHANNELS: Record<(typeof CHANNELS)[number], WireSpeakerChannelV2> = {
  L: 'front-left',
  R: 'front-right',
  C: 'front-center',
  LS: 'surround-left',
  RS: 'surround-right',
};

export const quaternionToWire = (q: Quaternion): WireQuaternionWxyz => [q.w, q.x, q.y, q.z];
export const quaternionFromWire = (q: WireQuaternionWxyz): Quaternion => ({ x: q[1], y: q[2], z: q[3], w: q[0] });

const toWireSurface = (surface: SurfaceAcoustics): WireSurface => ({
  materialId: surface.materialId,
  absorption: surface.absorption.map((value) => clamp(value, 0, 1)) as [number, number, number],
  diffusion: surface.diffusion.map((value) => clamp(value, 0, 1)) as [number, number, number],
});

const fromWireSurface = (surface: WireSurface): SurfaceAcoustics => ({
  materialId: surface.materialId,
  absorption: surface.absorption,
  diffusion: surface.diffusion,
});

export const toWireSceneConfig = (scene: SceneConfigV2): WireSceneConfigV2 => ({
  schemaVersion: 2,
  audio: {
    captureProvider: scene.captureProvider,
    captureEndpointId: scene.captureProvider === 'external-render' ? scene.captureEndpointId : null,
    outputDeviceId: scene.physicalOutputDeviceId,
    mode: scene.audioMode,
    inputLayout: scene.inputLayout,
    sampleRate: 48_000,
    bufferFrames: scene.audioMode === 'compatibility' ? 256 : 128,
    bypass: scene.bypass,
    masterGainDb: clamp(scene.masterGainDb, -60, 6),
    roomMix: clamp(scene.directRoomMix, 0, 1),
  },
  tracking: { enabled: true, cameraDeviceId: null, minimumFps: 60, predictionLimitMs: 20 },
  listener: {
    positionM: toVec(scene.listener.position),
    neutralOrientation: quaternionToWire(scene.listener.neutralPose),
  },
  speakers: CHANNELS.map((channel) => {
    const speaker = scene.speakers.find((entry) => entry.channel === channel) as SpeakerConfig;
    return {
      channel: WIRE_CHANNELS[channel],
      positionM: toVec(speaker.position),
      gainDb: clamp(speaker.gainDb, -60, 12),
      enabled: !speaker.muted,
    };
  }) as WireSceneConfigV2['speakers'],
  lfe: { enabled: !scene.lfe.muted, gainDb: clamp(scene.lfe.gainDb, -60, 12) },
  hrtf: { profileId: scene.hrtfProfileId, sofaPath: scene.importedSofaPath },
  headphoneEq: {
    enabled: scene.headphoneEq.enabled,
    preampDb: clamp(scene.headphoneEq.preampDb, -24, 0),
    filters: scene.headphoneEq.bands.filter((band) => band.enabled).slice(0, 16).map((band) => ({
      type: band.type === 'peak' ? 'peaking' : band.type,
      frequencyHz: clamp(band.frequencyHz, 10, 24_000),
      q: clamp(band.q, 0.01, 30),
      gainDb: clamp(band.gainDb, -24, 24),
    })),
  },
  room: {
    enabled: scene.room.enabled,
    lateReverbEnabled: scene.room.lateReverbEnabled,
    dimensionsM: [scene.room.dimensions.width, scene.room.dimensions.height, scene.room.dimensions.length],
    surfaces: [
      toWireSurface(scene.room.surfaces.left),
      toWireSurface(scene.room.surfaces.right),
      toWireSurface(scene.room.surfaces.rear),
      toWireSurface(scene.room.surfaces.front),
      toWireSurface(scene.room.surfaces.floor),
      toWireSurface(scene.room.surfaces.ceiling),
    ],
    earlyReflectionOrder: scene.room.earlyReflectionOrder,
    earlyReflectionLimitMs: clamp(scene.room.earlyWindowMs, 0, 80),
  },
});

export const isWireSceneConfigV1 = (value: unknown): value is WireSceneConfigV1 => {
  if (!value || typeof value !== 'object') return false;
  const scene = value as Partial<WireSceneConfigV1>;
  const surfaces = scene.room?.surfaces;
  const filters = scene.headphoneEq?.filters;
  const captureProvider = scene.audio?.captureProvider ?? 'native-driver';
  const captureEndpointId = scene.audio?.captureEndpointId ?? null;
  return (
    scene.schemaVersion === 1 &&
    scene.audio?.sampleRate === 48_000 && ['shared-low-latency', 'exclusive-pro', 'compatibility'].includes(scene.audio.mode ?? '') &&
    [64, 128, 256].includes(scene.audio.bufferFrames ?? 0) && typeof scene.audio.bypass === 'boolean' &&
    typeof scene.audio.masterGainDb === 'number' && Number.isFinite(scene.audio.masterGainDb) &&
    typeof scene.audio.roomMix === 'number' && Number.isFinite(scene.audio.roomMix) &&
    ['native-driver', 'external-render'].includes(captureProvider) &&
    (captureEndpointId === null || (typeof captureEndpointId === 'string' && captureEndpointId.length > 0)) &&
    (scene.audio.outputDeviceId === null || typeof scene.audio.outputDeviceId === 'string') &&
    isFiniteTuple(scene.listener?.positionM, 3) && isFiniteTuple(scene.listener?.neutralOrientation, 4) &&
    Array.isArray(scene.speakers) && scene.speakers.length === 2 &&
    scene.speakers[0]?.channel === 'left' && scene.speakers[1]?.channel === 'right' &&
    scene.speakers.every((speaker) => isFiniteTuple(speaker.positionM, 3) && Number.isFinite(speaker.gainDb)) &&
    typeof scene.hrtf?.profileId === 'string' && scene.hrtf.profileId.length > 0 &&
    (scene.hrtf.sofaPath === null || typeof scene.hrtf.sofaPath === 'string') &&
    typeof scene.headphoneEq?.enabled === 'boolean' && typeof scene.headphoneEq.preampDb === 'number' &&
    Number.isFinite(scene.headphoneEq.preampDb) && Array.isArray(filters) && filters.length <= 16 &&
    filters.every((filter) => typeof filter.type === 'string' && Number.isFinite(filter.frequencyHz) && Number.isFinite(filter.q) && Number.isFinite(filter.gainDb)) &&
    typeof scene.room?.enabled === 'boolean' && typeof scene.room.lateReverbEnabled === 'boolean' &&
    isFiniteTuple(scene.room.dimensionsM, 3) && Array.isArray(surfaces) && surfaces.length === 6 &&
    surfaces.every((surface) => typeof surface.materialId === 'string' && isFiniteTuple(surface.absorption, 3) && isFiniteTuple(surface.diffusion, 3)) &&
    [0, 1, 2].includes(scene.room.earlyReflectionOrder ?? -1) && Number.isFinite(scene.room.earlyReflectionLimitMs)
  );
};

const sceneFromWireCommon = (
  wire: WireSceneConfigV1 | WireSceneConfigV2,
  speakers: SpeakerSet,
): SceneConfigV2 => {
  const base = structuredClone(defaultScene);
  const surfaces = wire.room.surfaces;
  const captureProvider = wire.audio.captureProvider === 'external-render' ? 'external-render' : 'native-driver';
  return {
    ...base,
    speakers,
    listener: { position: vec(wire.listener.positionM), neutralPose: quaternionFromWire(wire.listener.neutralOrientation) },
    hrtfProfileId: wire.hrtf.profileId,
    importedSofaPath: wire.hrtf.sofaPath,
    headphoneEq: {
      ...base.headphoneEq,
      enabled: wire.headphoneEq.enabled,
      preampDb: clamp(finite(wire.headphoneEq.preampDb, -6), -24, 0),
      bands: wire.headphoneEq.filters.map((filter, index) => ({
        id: `wire-${index}`,
        enabled: true,
        type: filter.type === 'peaking' || filter.type === 'low-pass' || filter.type === 'high-pass' ? 'peak' : filter.type,
        frequencyHz: filter.frequencyHz,
        gainDb: filter.gainDb,
        q: filter.q,
      })),
    },
    audioMode: wire.audio.mode,
    captureProvider,
    captureEndpointId: captureProvider === 'external-render' ? wire.audio.captureEndpointId ?? null : null,
    physicalOutputDeviceId: wire.audio.outputDeviceId,
    masterGainDb: wire.audio.masterGainDb,
    directRoomMix: wire.audio.roomMix,
    bypass: wire.audio.bypass,
    room: {
      ...base.room,
      enabled: wire.room.enabled,
      lateReverbEnabled: wire.room.lateReverbEnabled,
      dimensions: { width: wire.room.dimensionsM[0], height: wire.room.dimensionsM[1], length: wire.room.dimensionsM[2] },
      earlyReflectionOrder: wire.room.earlyReflectionOrder,
      earlyWindowMs: wire.room.earlyReflectionLimitMs,
      surfaces: {
        left: fromWireSurface(surfaces[0]),
        right: fromWireSurface(surfaces[1]),
        rear: fromWireSurface(surfaces[2]),
        front: fromWireSurface(surfaces[3]),
        floor: fromWireSurface(surfaces[4]),
        ceiling: fromWireSurface(surfaces[5]),
      },
    },
  };
};

const clampMigratedPosition = (
  position: { x: number; y: number; z: number },
  room: SceneConfigV2['room'],
): { x: number; y: number; z: number } => ({
  x: clamp(position.x, -room.dimensions.width / 2 + 0.1, room.dimensions.width / 2 - 0.1),
  y: clamp(position.y, 0.4, room.dimensions.height - 0.1),
  z: clamp(position.z, -room.dimensions.length / 2 + 0.1, room.dimensions.length / 2 - 0.1),
});

const migratedSupplementarySpeakers = (
  listener: SceneConfigV2['listener'],
  room: SceneConfigV2['room'],
): [SpeakerConfig<'C'>, SpeakerConfig<'LS'>, SpeakerConfig<'RS'>] => {
  const makePosition = (azimuth: number) => {
    const radians = azimuth * Math.PI / 180;
    return clampMigratedPosition({
      x: listener.position.x + Math.sin(radians) * 2,
      y: listener.position.y,
      z: listener.position.z + Math.cos(radians) * 2,
    }, room);
  };
  return [
    { ...structuredClone(defaultScene.speakers[2]), position: makePosition(0) },
    { ...structuredClone(defaultScene.speakers[3]), position: makePosition(-110) },
    { ...structuredClone(defaultScene.speakers[4]), position: makePosition(110) },
  ];
};

export const fromWireSceneConfigV1 = (wire: WireSceneConfigV1): SceneConfigV2 => {
  const base = structuredClone(defaultScene);
  const listener = { position: vec(wire.listener.positionM), neutralPose: quaternionFromWire(wire.listener.neutralOrientation) };
  const room = {
    ...base.room,
    enabled: wire.room.enabled,
    lateReverbEnabled: wire.room.lateReverbEnabled,
    dimensions: { width: wire.room.dimensionsM[0], height: wire.room.dimensionsM[1], length: wire.room.dimensionsM[2] },
  };
  const [center, surroundLeft, surroundRight] = migratedSupplementarySpeakers(listener, room);
  const speakers: SpeakerSet = [
    { ...base.speakers[0], position: vec(wire.speakers[0].positionM), gainDb: wire.speakers[0].gainDb, muted: wire.speakers[0].gainDb <= -60 },
    { ...base.speakers[1], position: vec(wire.speakers[1].positionM), gainDb: wire.speakers[1].gainDb, muted: wire.speakers[1].gainDb <= -60 },
    center,
    surroundLeft,
    surroundRight,
  ];
  return { ...sceneFromWireCommon(wire, speakers), version: 2, inputLayout: 'stereo', lfe: { gainDb: 0, muted: false } };
};

export const isWireSceneConfigV2 = (value: unknown): value is WireSceneConfigV2 => {
  if (!value || typeof value !== 'object') return false;
  const scene = value as Partial<WireSceneConfigV2>;
  const speakers = scene.speakers;
  const expectedChannels: WireSpeakerChannelV2[] = [
    'front-left', 'front-right', 'front-center', 'surround-left', 'surround-right',
  ];
  const surfaces = scene.room?.surfaces;
  const filters = scene.headphoneEq?.filters;
  return scene.schemaVersion === 2 &&
    scene.audio?.sampleRate === 48_000 && ['shared-low-latency', 'exclusive-pro', 'compatibility'].includes(scene.audio.mode ?? '') &&
    ['stereo', '5.1-surround'].includes(scene.audio.inputLayout ?? '') &&
    [64, 128, 256].includes(scene.audio.bufferFrames ?? 0) && typeof scene.audio.bypass === 'boolean' &&
    Number.isFinite(scene.audio.masterGainDb) && Number.isFinite(scene.audio.roomMix) &&
    ['native-driver', 'external-render'].includes(scene.audio.captureProvider ?? '') &&
    (scene.audio.captureEndpointId === null || typeof scene.audio.captureEndpointId === 'string') &&
    (scene.audio.outputDeviceId === null || typeof scene.audio.outputDeviceId === 'string') &&
    isFiniteTuple(scene.listener?.positionM, 3) && isFiniteTuple(scene.listener?.neutralOrientation, 4) &&
    Array.isArray(speakers) && speakers.length === 5 && speakers.every((speaker, index) =>
      speaker.channel === expectedChannels[index] && isFiniteTuple(speaker.positionM, 3) &&
      Number.isFinite(speaker.gainDb) && typeof speaker.enabled === 'boolean') &&
    Number.isFinite(scene.lfe?.gainDb) && typeof scene.lfe?.enabled === 'boolean' &&
    typeof scene.hrtf?.profileId === 'string' && scene.hrtf.profileId.length > 0 &&
    (scene.hrtf.sofaPath === null || typeof scene.hrtf.sofaPath === 'string') &&
    typeof scene.headphoneEq?.enabled === 'boolean' && Number.isFinite(scene.headphoneEq.preampDb) &&
    Array.isArray(filters) && filters.length <= 16 && filters.every((filter) =>
      typeof filter.type === 'string' && Number.isFinite(filter.frequencyHz) && Number.isFinite(filter.q) && Number.isFinite(filter.gainDb)) &&
    typeof scene.room?.enabled === 'boolean' && typeof scene.room.lateReverbEnabled === 'boolean' &&
    isFiniteTuple(scene.room.dimensionsM, 3) && Array.isArray(surfaces) && surfaces.length === 6 &&
    surfaces.every((surface) => typeof surface.materialId === 'string' && isFiniteTuple(surface.absorption, 3) && isFiniteTuple(surface.diffusion, 3)) &&
    [0, 1, 2].includes(scene.room.earlyReflectionOrder ?? -1) && Number.isFinite(scene.room.earlyReflectionLimitMs);
};

export const fromWireSceneConfig = (wire: WireSceneConfigV2): SceneConfigV2 => {
  const base = structuredClone(defaultScene);
  const speakers = wire.speakers.map((speaker, index) => ({
    ...base.speakers[index],
    position: vec(speaker.positionM),
    gainDb: speaker.gainDb,
    muted: !speaker.enabled,
  })) as SpeakerSet;
  return {
    ...sceneFromWireCommon(wire, speakers),
    version: 2,
    inputLayout: wire.audio.inputLayout,
    lfe: { gainDb: wire.lfe.gainDb, muted: !wire.lfe.enabled },
  };
};

export const toPersistedDesktopConfig = (config: PersistedAppConfigV2): PersistedDesktopConfigV2 => ({
  schemaVersion: 2,
  scene: toWireSceneConfig(config.scene),
  preferences: config.preferences,
});

export const migratePersistedConfig = (value: unknown): PersistedAppConfigV2 | null => {
  if (!value || typeof value !== 'object') return null;
  const candidate = value as { schemaVersion?: unknown; scene?: unknown; preferences?: unknown };
  if (candidate.schemaVersion !== 1 && candidate.schemaVersion !== 2) return null;
  const rawPreferences = candidate.preferences && typeof candidate.preferences === 'object'
    ? candidate.preferences as Partial<typeof defaultPreferences>
    : {};
  const preferences = {
    language: rawPreferences.language === 'en' ? 'en' as const : 'fr' as const,
    onboardingComplete: typeof rawPreferences.onboardingComplete === 'boolean' ? rawPreferences.onboardingComplete : defaultPreferences.onboardingComplete,
    onboardingStep: typeof rawPreferences.onboardingStep === 'number' && Number.isInteger(rawPreferences.onboardingStep)
      ? clamp(rawPreferences.onboardingStep, 0, 3) : defaultPreferences.onboardingStep,
    startWithWindows: typeof rawPreferences.startWithWindows === 'boolean' ? rawPreferences.startWithWindows : defaultPreferences.startWithWindows,
    minimizeToTray: typeof rawPreferences.minimizeToTray === 'boolean' ? rawPreferences.minimizeToTray : defaultPreferences.minimizeToTray,
    showCameraPreview: typeof rawPreferences.showCameraPreview === 'boolean' ? rawPreferences.showCameraPreview : defaultPreferences.showCameraPreview,
  };
  if (candidate.schemaVersion === 2 && isWireSceneConfigV2(candidate.scene)) {
    return { schemaVersion: 2, scene: fromWireSceneConfig(candidate.scene), preferences };
  }
  if (candidate.schemaVersion === 1 && isWireSceneConfigV1(candidate.scene)) {
    return { schemaVersion: 2, scene: fromWireSceneConfigV1(candidate.scene), preferences };
  }

  // Migration des scènes wire écrites avant l'introduction de la diffusion par bande.
  if (candidate.scene && typeof candidate.scene === 'object') {
    const migratedWire = structuredClone(candidate.scene) as Record<string, unknown>;
    const room = migratedWire.room as { lateReverbEnabled?: unknown; surfaces?: unknown[] } | undefined;
    if (room && Array.isArray(room.surfaces)) {
      room.lateReverbEnabled ??= true;
      room.surfaces = room.surfaces.map((entry) => {
        if (!entry || typeof entry !== 'object') return entry;
        const surface = entry as { scattering?: unknown; diffusion?: unknown };
        if (!Array.isArray(surface.diffusion) && typeof surface.scattering === 'number' && Number.isFinite(surface.scattering)) {
          surface.diffusion = [surface.scattering, surface.scattering, surface.scattering];
        }
        delete surface.scattering;
        return surface;
      });
      if (candidate.schemaVersion === 1 && isWireSceneConfigV1(migratedWire)) {
        return { schemaVersion: 2, scene: fromWireSceneConfigV1(migratedWire), preferences };
      }
    }
  }

  // Migration du format interne utilisé par les premières préversions locales.
  const legacy = candidate.scene as Record<string, unknown> | undefined;
  if (legacy?.version === 1 && Array.isArray(legacy.speakers) && legacy.speakers.length === 2) {
    const base = structuredClone(defaultScene);
    const legacySpeakers = structuredClone(legacy.speakers) as SpeakerConfig[];
    // L'ancien aperçu utilisait -Z pour l'avant ; renverse uniquement les scènes clairement anciennes.
    if (legacySpeakers.every((speaker) => speaker.position.z < 0)) {
      for (const speaker of legacySpeakers) speaker.position.z = -speaker.position.z;
    }
    const listener = legacy.listener && typeof legacy.listener === 'object'
      ? legacy.listener as SceneConfigV2['listener']
      : base.listener;
    const room = legacy.room && typeof legacy.room === 'object' ? legacy.room as SceneConfigV2['room'] : base.room;
    const [center, surroundLeft, surroundRight] = migratedSupplementarySpeakers(listener, room);
    const captureProvider = legacy.captureProvider === 'external-render' ? 'external-render' : 'native-driver';
    const scene: SceneConfigV2 = {
      ...base,
      ...legacy,
      version: 2,
      inputLayout: 'stereo',
      listener,
      room,
      speakers: [
        { ...base.speakers[0], ...legacySpeakers[0], id: 'L', channel: 'L' },
        { ...base.speakers[1], ...legacySpeakers[1], id: 'R', channel: 'R' },
        center,
        surroundLeft,
        surroundRight,
      ],
      lfe: { gainDb: 0, muted: false },
      importedSofaPath: typeof legacy.importedSofaPath === 'string' ? legacy.importedSofaPath : null,
      captureProvider,
      captureEndpointId: captureProvider === 'external-render' && typeof legacy.captureEndpointId === 'string'
      ? legacy.captureEndpointId
      : null,
    };
    return { schemaVersion: 2, scene, preferences };
  }
  return null;
};

export const toWireEngineCommand = (command: EngineCommandV1): unknown => {
  if (command.type === 'set-scene') return { schemaVersion: 1, type: command.type, scene: toWireSceneConfig(command.scene) };
  if (command.type === 'calibrate-neutral-pose') return { schemaVersion: 1, type: 'calibrate-neutral-pose', quaternion: quaternionToWire(command.quaternion) };
  if (command.type === 'set-bypass') return { schemaVersion: 1, type: command.type, enabled: command.enabled };
  if (command.type === 'set-output-device') return { schemaVersion: 1, type: command.type, deviceId: command.deviceId };
  if (command.type === 'set-audio-route') {
    return {
      schemaVersion: 1,
      type: command.type,
      captureProvider: command.captureProvider,
      captureEndpointId: command.captureProvider === 'external-render' ? command.captureEndpointId : null,
      outputDeviceId: command.outputDeviceId,
    };
  }
  if (command.type === 'set-audio-mode') return { schemaVersion: 1, type: command.type, mode: command.mode };
  if (command.type === 'set-hrtf') return { schemaVersion: 1, type: command.type, profileId: command.profileId, sofaPath: command.sofaPath };
  if (command.type === 'set-headphone-eq') {
    return {
      schemaVersion: 1,
      type: command.type,
      eq: {
        enabled: command.eq.enabled,
        preampDb: clamp(command.eq.preampDb, -24, 0),
        profileName: command.eq.profileName,
        bands: command.eq.bands.slice(0, 16).map((band) => ({
          id: band.id,
          enabled: band.enabled,
          type: band.type,
          frequencyHz: clamp(band.frequencyHz, 10, 24_000),
          gainDb: clamp(band.gainDb, -24, 24),
          q: clamp(band.q, 0.01, 30),
        })),
      },
    };
  }
  return { schemaVersion: 1, type: command.type };
};

export const isWireEngineStatusV1 = (value: unknown): value is WireEngineStatusV1 => {
  if (!value || typeof value !== 'object') return false;
  const status = value as Partial<WireEngineStatusV1>;
  const streamStates = new Set(['stopped', 'starting', 'running', 'degraded', 'failed']);
  const trackingStates = new Set(['lost', 'tracking', 'held', 'returning-to-neutral']);
  const sampleFormats = new Set(['unknown', 'float32', 'pcm-s32']);
  const inputLayouts = new Set(['stereo', '5.1-surround']);
  const numericKeys: (keyof WireEngineStatusV1)[] = [
    'captureSampleRate', 'renderSampleRate', 'capturePeriodFrames', 'renderPeriodFrames',
    'fifoFillFrames', 'xruns', 'callbackCpuPercent', 'trackingHz', 'latencyP50Ms',
    'latencyP95Ms', 'resampleRatio',
  ];
  return status.schemaVersion === 1 &&
    typeof status.audioMode === 'string' && ['shared-low-latency', 'exclusive-pro', 'compatibility'].includes(status.audioMode) &&
    (status.renderSampleFormat === undefined ||
      (typeof status.renderSampleFormat === 'string' && sampleFormats.has(status.renderSampleFormat))) &&
    (status.inputLayout === undefined || (typeof status.inputLayout === 'string' && inputLayouts.has(status.inputLayout))) &&
    (status.captureChannels === undefined || (Number.isInteger(status.captureChannels) && status.captureChannels >= 0)) &&
    (status.captureChannelMask === undefined || (Number.isInteger(status.captureChannelMask) && status.captureChannelMask >= 0)) &&
    typeof status.captureState === 'string' && streamStates.has(status.captureState) &&
    typeof status.renderState === 'string' && streamStates.has(status.renderState) &&
    typeof status.trackingState === 'string' && trackingStates.has(status.trackingState) &&
    numericKeys.every((key) => typeof status[key] === 'number' && Number.isFinite(status[key])) &&
    typeof status.potentiallyBinaural === 'boolean' &&
    typeof status.lastError === 'string';
};

export const fromWireEngineStatus = (wire: WireEngineStatusV1): EngineStatusV1 => {
  const captureRate = wire.captureSampleRate || 48_000;
  const renderRate = wire.renderSampleRate || captureRate;
  const captureRunning = wire.captureState === 'running' || wire.captureState === 'degraded';
  const renderRunning = wire.renderState === 'running' || wire.renderState === 'degraded';
  const failed = wire.captureState === 'failed' || wire.renderState === 'failed';
  const degraded = wire.captureState === 'degraded' || wire.renderState === 'degraded';
  const starting = wire.captureState === 'starting' || wire.renderState === 'starting';
  const fifoReference = Math.max(1, wire.renderPeriodFrames * 4);
  return {
    version: 1,
    audioMode: wire.audioMode,
    renderSampleFormat: wire.renderSampleFormat ?? 'unknown',
    inputLayout: wire.inputLayout ?? 'stereo',
    captureChannels: wire.captureChannels ?? 2,
    captureChannelMask: wire.captureChannelMask ?? 0x3,
    connection: failed ? 'error' : degraded ? 'degraded' : captureRunning && renderRunning ? 'ready' : starting ? 'connecting' : 'offline',
    captureActive: captureRunning,
    renderActive: renderRunning,
    trackingActive: wire.trackingState === 'tracking' || wire.trackingState === 'held',
    trackingHz: finite(wire.trackingHz, 0),
    physicalOutputName: null,
    sampleRate: renderRate,
    blockFrames: wire.renderPeriodFrames || 128,
    capturePeriodMs: (wire.capturePeriodFrames / captureRate) * 1000,
    renderPeriodMs: (wire.renderPeriodFrames / renderRate) * 1000,
    fifoFillPercent: clamp((wire.fifoFillFrames / fifoReference) * 100, 0, 100),
    fifoFillFrames: wire.fifoFillFrames,
    xruns: wire.xruns,
    callbackCpuPercent: finite(wire.callbackCpuPercent, 0),
    engineCpuPercent: finite(wire.callbackCpuPercent, 0),
    motionToSoundLatencyMs: { p50: finite(wire.latencyP50Ms, 0), p95: finite(wire.latencyP95Ms, 0) },
    audioPipelineLatencyMs: ((wire.capturePeriodFrames / captureRate) + (wire.renderPeriodFrames / renderRate)) * 1000,
    clockDriftPpm: (finite(wire.resampleRatio, 1) - 1) * 1_000_000,
    uptimeSeconds: 0,
    potentiallyBinaural: wire.potentiallyBinaural,
    lastError: wire.lastError || null,
  };
};

const crc32 = (bytes: Uint8Array, length: number): number => {
  let crc = 0xffffffff;
  for (let i = 0; i < length; i += 1) {
    crc ^= bytes[i];
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
};

export const packHeadPoseV1 = (sample: HeadPoseSampleV1): Uint8Array => {
  const buffer = new ArrayBuffer(64);
  const bytes = new Uint8Array(buffer);
  const view = new DataView(buffer);
  bytes.set([0x53, 0x53, 0x50, 0x31], 0);
  view.setUint16(4, 1, true);
  const state = sample.trackingState === 'tracked' ? 2 : sample.trackingState === 'lost' ? 0 : 1;
  view.setUint16(6, state, true);
  view.setBigUint64(8, BigInt(sample.sequence), true);
  view.setBigInt64(16, BigInt(sample.timestampQpc), true);
  const q = quaternionToWire(sample.quaternion);
  q.forEach((value, index) => view.setFloat32(24 + index * 4, value, true));
  view.setFloat32(40, sample.angularVelocity.x, true);
  view.setFloat32(44, sample.angularVelocity.y, true);
  view.setFloat32(48, sample.angularVelocity.z, true);
  view.setFloat32(52, clamp(finite(sample.confidence, 0), 0, 1), true);
  view.setUint32(56, 0, true);
  view.setUint32(60, crc32(bytes, 60), true);
  return bytes;
};
