import { describe, expect, it } from 'vitest';
import { defaultPreferences, defaultScene, defaultWindowSpatialization } from '../data/defaults';
import type { HeadPoseSampleV1 } from '../types/contracts';
import type { WireEngineStatusV1, WireSceneConfigV1 } from '../types/wire-contracts';
import {
  fromWireEngineStatus,
  fromWireSceneConfig,
  fromWireWindowSpatializationConfig,
  isWireSceneConfigV2,
  isWireWindowSpatializationConfigV1,
  packHeadPoseV1,
  quaternionToWire,
  migratePersistedConfig,
  toPersistedDesktopConfig,
  toWireEngineCommand,
  toWireSceneConfig,
  toWireWindowSpatializationConfig,
} from './contracts';

const legacyWireScene = (): WireSceneConfigV1 => {
  const current = toWireSceneConfig(defaultScene);
  return {
    schemaVersion: 1,
    audio: {
      captureProvider: current.audio.captureProvider,
      captureEndpointId: current.audio.captureEndpointId,
      outputDeviceId: current.audio.outputDeviceId,
      mode: current.audio.mode,
      sampleRate: current.audio.sampleRate,
      bufferFrames: current.audio.bufferFrames,
      bypass: current.audio.bypass,
      masterGainDb: current.audio.masterGainDb,
      roomMix: current.audio.roomMix,
    },
    tracking: current.tracking,
    listener: current.listener,
    speakers: [
      { channel: 'left', positionM: current.speakers[0].positionM, gainDb: current.speakers[0].gainDb },
      { channel: 'right', positionM: current.speakers[1].positionM, gainDb: current.speakers[1].gainDb },
    ],
    hrtf: current.hrtf,
    headphoneEq: current.headphoneEq,
    room: current.room,
  };
};

describe('SceneConfigV2 wire', () => {
  it('respecte le repère +Z avant et l’ordre de quaternion wxyz', () => {
    const scene = structuredClone(defaultScene);
    scene.listener.neutralPose = { x: 0.1, y: 0.2, z: 0.3, w: 0.9 };
    const wire = toWireSceneConfig(scene);
    expect(wire.speakers[0].positionM).toEqual([-1, 1.2, 1.732051]);
    expect(wire.speakers[1].positionM).toEqual([1, 1.2, 1.732051]);
    expect(wire.listener.neutralOrientation).toEqual([0.9, 0.1, 0.2, 0.3]);
    expect(quaternionToWire(scene.listener.neutralPose)).toEqual([0.9, 0.1, 0.2, 0.3]);
  });

  it('sérialise exactement les cinq canaux, les états enabled et le LFE séparé', () => {
    const scene = structuredClone(defaultScene);
    scene.inputLayout = '5.1-surround';
    scene.speakers[3].muted = true;
    scene.lfe = { gainDb: -4.5, muted: true };
    const wire = toWireSceneConfig(scene);
    expect(wire.schemaVersion).toBe(2);
    expect(wire.audio.inputLayout).toBe('5.1-surround');
    expect(wire.speakers.map((speaker) => speaker.channel)).toEqual([
      'front-left', 'front-right', 'front-center', 'surround-left', 'surround-right',
    ]);
    expect(wire.speakers.map((speaker) => speaker.enabled)).toEqual([true, true, true, false, true]);
    expect(wire.lfe).toEqual({ enabled: false, gainDb: -4.5 });
    expect(isWireSceneConfigV2(wire)).toBe(true);
  });

  it('sérialise les surfaces dans l’ordre -X,+X,-Z,+Z,sol,plafond', () => {
    const wire = toWireSceneConfig(defaultScene);
    expect(wire.room.surfaces.map((surface) => surface.materialId)).toEqual([
      defaultScene.room.surfaces.left.materialId,
      defaultScene.room.surfaces.right.materialId,
      defaultScene.room.surfaces.rear.materialId,
      defaultScene.room.surfaces.front.materialId,
      defaultScene.room.surfaces.floor.materialId,
      defaultScene.room.surfaces.ceiling.materialId,
    ]);
    expect(wire.room.surfaces[0].diffusion).toEqual(defaultScene.room.surfaces.left.diffusion);
    expect(wire.room.lateReverbEnabled).toBe(true);
  });

  it('préserve le préampli EQ et les champs canoniques au round-trip', () => {
    const scene = structuredClone(defaultScene);
    scene.headphoneEq.enabled = true;
    scene.headphoneEq.preampDb = -7.5;
    scene.captureProvider = 'external-render';
    scene.captureEndpointId = 'external-cable';
    const restored = fromWireSceneConfig(toWireSceneConfig(scene));
    expect(restored.headphoneEq.preampDb).toBe(-7.5);
    expect(restored.headphoneEq.enabled).toBe(true);
    expect(restored.captureProvider).toBe('external-render');
    expect(restored.captureEndpointId).toBe('external-cable');
  });

  it('persiste une enveloppe V3 avec la configuration des fenêtres', () => {
    const persisted = toPersistedDesktopConfig({
      schemaVersion: 3,
      scene: defaultScene,
      preferences: defaultPreferences,
      windowSpatialization: defaultWindowSpatialization,
    });
    expect(persisted.schemaVersion).toBe(3);
    expect(persisted.scene.schemaVersion).toBe(2);
    expect(persisted.windowSpatialization.schemaVersion).toBe(1);
  });

  it('désactive un ancien mode fenêtres incompatible avec une scène 5.1', () => {
    const scene = structuredClone(defaultScene);
    scene.captureProvider = 'external-render';
    scene.captureEndpointId = 'capture-5.1';
    scene.inputLayout = '5.1-surround';
    const persisted = toPersistedDesktopConfig({
      schemaVersion: 3,
      scene,
      preferences: defaultPreferences,
      windowSpatialization: {
        ...structuredClone(defaultWindowSpatialization),
        enabled: true,
      },
    });

    const migrated = migratePersistedConfig(persisted);

    expect(migrated?.scene.inputLayout).toBe('5.1-surround');
    expect(migrated?.windowSpatialization.enabled).toBe(false);
  });

  it('migre une scène V1 vers une scène V2 stéréo avec cinq enceintes bornées', () => {
    const legacy = legacyWireScene();
    delete legacy.audio.captureProvider;
    delete legacy.audio.captureEndpointId;
    const migrated = migratePersistedConfig({ schemaVersion: 1, scene: legacy, preferences: {} });
    expect(migrated?.schemaVersion).toBe(3);
    expect(migrated?.windowSpatialization).toEqual(defaultWindowSpatialization);
    expect(migrated?.scene.version).toBe(2);
    expect(migrated?.scene.inputLayout).toBe('stereo');
    expect(migrated?.scene.speakers.map((speaker) => speaker.channel)).toEqual(['L', 'R', 'C', 'LS', 'RS']);
    expect(migrated?.scene.speakers).toHaveLength(5);
    expect(migrated?.scene.lfe).toEqual({ gainDb: 0, muted: false });
    expect(migrated?.scene.captureProvider).toBe('native-driver');
    expect(migrated?.scene.captureEndpointId).toBeNull();
    expect(toWireSceneConfig(migrated!.scene).audio).toMatchObject({
      captureProvider: 'native-driver',
      captureEndpointId: null,
    });
  });

  it('préserve un choix externe encore incomplet sans réinitialiser la scène', () => {
    const incomplete = legacyWireScene();
    incomplete.audio.captureProvider = 'external-render';
    incomplete.audio.captureEndpointId = null;
    const migrated = migratePersistedConfig({ schemaVersion: 1, scene: incomplete, preferences: {} });
    expect(migrated?.scene.captureProvider).toBe('external-render');
    expect(migrated?.scene.captureEndpointId).toBeNull();
  });

  it('migre scattering scalaire vers diffusion trois bandes', () => {
    const legacy = legacyWireScene() as unknown as { room: { lateReverbEnabled?: boolean; surfaces: Array<Record<string, unknown>> } };
    delete legacy.room.lateReverbEnabled;
    legacy.room.surfaces = legacy.room.surfaces.map((surface) => {
      const { diffusion: _diffusion, ...rest } = surface;
      return { ...rest, scattering: 0.25 };
    });
    const migrated = migratePersistedConfig({ schemaVersion: 1, scene: legacy, preferences: {} });
    expect(migrated?.scene.room.lateReverbEnabled).toBe(true);
    expect(migrated?.scene.room.surfaces.left.diffusion).toEqual([0.25, 0.25, 0.25]);
  });
});

describe('commandes moteur', () => {
  it('sérialise la spatialisation stéréo par fenêtres selon le contrat V1', () => {
    const config = {
      ...structuredClone(defaultWindowSpatialization),
      enabled: true,
      emitterPlacementMode: 'window-edges' as const,
      displayCalibrations: [{
        displayId: 'display-right',
        center: { x: 0.8, y: 1.2, z: 0.9 },
        widthM: 0.62,
        heightM: 0.35,
        orientation: { x: 0, y: 0.2, z: 0, w: 0.98 },
      }],
      sourceRules: [{
        applicationId: 'player.exe',
        enabled: true,
        gainDb: -2,
        stereoSpread: 0.6,
        fallbackDisplayId: 'display-right',
      }],
    };
    const wire = toWireWindowSpatializationConfig(config);
    expect(wire).toMatchObject({
      schemaVersion: 1,
      enabled: true,
      maxSources: 8,
      emitterPlacementMode: 'window-edges',
      displayCalibrations: [{
        displayId: 'display-right',
        centerM: [0.8, 1.2, 0.9],
        orientation: [0.98, 0, 0.2, 0],
      }],
    });
    expect(isWireWindowSpatializationConfigV1(wire)).toBe(true);
    expect(fromWireWindowSpatializationConfig(wire)).toEqual(config);
    const legacyWire = { ...wire };
    delete legacyWire.emitterPlacementMode;
    expect(fromWireWindowSpatializationConfig(legacyWire).emitterPlacementMode).toBe('proportional');
    expect(toWireEngineCommand({
      version: 1,
      type: 'set-window-spatialization',
      config,
    })).toEqual({
      schemaVersion: 1,
      type: 'set-window-spatialization',
      config: wire,
    });
  });

  it('rejette les quaternions d’écran non unitaires et les identifiants dupliqués', () => {
    const calibration = {
      displayId: 'display-main',
      center: { x: 0, y: 1.2, z: 0.9 },
      widthM: 0.6,
      heightM: 0.34,
      orientation: { x: 0, y: 0, z: 0, w: 1 },
    };
    const invalidOrientation = toWireWindowSpatializationConfig({
      ...structuredClone(defaultWindowSpatialization),
      displayCalibrations: [{
        ...calibration,
        orientation: { x: 0, y: 0, z: 0, w: 0 },
      }],
    });
    expect(isWireWindowSpatializationConfigV1(invalidOrientation)).toBe(false);

    const duplicateDisplays = toWireWindowSpatializationConfig({
      ...structuredClone(defaultWindowSpatialization),
      displayCalibrations: [
        calibration,
        { ...structuredClone(calibration), displayId: 'DISPLAY-MAIN' },
      ],
    });
    expect(isWireWindowSpatializationConfigV1(duplicateDisplays)).toBe(false);

    const sourceRule = {
      applicationId: 'player.exe',
      enabled: true,
      gainDb: 0,
      stereoSpread: 0.7,
      fallbackDisplayId: null,
    };
    const duplicateRules = toWireWindowSpatializationConfig({
      ...structuredClone(defaultWindowSpatialization),
      sourceRules: [
        sourceRule,
        { ...structuredClone(sourceRule), applicationId: 'PLAYER.EXE' },
      ],
    });
    expect(isWireWindowSpatializationConfigV1(duplicateRules)).toBe(false);
  });

  it('sérialise calibrate-neutral-pose avec quaternion wxyz', () => {
    expect(toWireEngineCommand({
      version: 1,
      type: 'calibrate-neutral-pose',
      quaternion: { x: 0.1, y: 0.2, z: 0.3, w: 0.9 },
    })).toEqual({ schemaVersion: 1, type: 'calibrate-neutral-pose', quaternion: [0.9, 0.1, 0.2, 0.3] });
  });

  it('sérialise les noms de champs attendus par set-output-device et set-bypass', () => {
    expect(toWireEngineCommand({ version: 1, type: 'set-output-device', deviceId: 'device-1' })).toEqual({
      schemaVersion: 1, type: 'set-output-device', deviceId: 'device-1',
    });
    expect(toWireEngineCommand({ version: 1, type: 'set-bypass', enabled: true })).toEqual({
      schemaVersion: 1, type: 'set-bypass', enabled: true,
    });
  });

  it('sérialise atomiquement la source de capture et la sortie physique', () => {
    expect(toWireEngineCommand({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: 'external-cable',
      outputDeviceId: 'usb-headset',
    })).toEqual({
      schemaVersion: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: 'external-cable',
      outputDeviceId: 'usb-headset',
    });
    expect(toWireEngineCommand({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'native-driver',
      captureEndpointId: 'ignored',
      outputDeviceId: 'usb-headset',
    })).toMatchObject({ captureEndpointId: null });
  });

  it('sérialise explicitement les trois modes audio', () => {
    for (const mode of ['shared-low-latency', 'exclusive-pro', 'compatibility'] as const) {
      expect(toWireEngineCommand({ version: 1, type: 'set-audio-mode', mode })).toEqual({
        schemaVersion: 1,
        type: 'set-audio-mode',
        mode,
      });
    }
  });

  it('sérialise set-hrtf avec le chemin attendu par le moteur', () => {
    expect(toWireEngineCommand({
      version: 1,
      type: 'set-hrtf',
      profileId: 'personal-deadbeef',
      sofaPath: 'C:\\HRTF\\deadbeef.sofa',
    })).toEqual({
      schemaVersion: 1,
      type: 'set-hrtf',
      profileId: 'personal-deadbeef',
      sofaPath: 'C:\\HRTF\\deadbeef.sofa',
    });
  });

  it('préserve preampDb et les bandes de set-headphone-eq', () => {
    expect(toWireEngineCommand({
      version: 1,
      type: 'set-headphone-eq',
      eq: {
        enabled: true,
        preampDb: -7.5,
        profileName: 'Casque de référence',
        bands: [{ id: 'presence', enabled: true, type: 'peak', frequencyHz: 2_800, gainDb: -2, q: 1.1 }],
      },
    })).toEqual({
      schemaVersion: 1,
      type: 'set-headphone-eq',
      eq: {
        enabled: true,
        preampDb: -7.5,
        profileName: 'Casque de référence',
        bands: [{ id: 'presence', enabled: true, type: 'peak', frequencyHz: 2_800, gainDb: -2, q: 1.1 }],
      },
    });
  });
});

describe('HeadPoseSampleV1 binaire', () => {
  it('produit exactement 64 octets avec quaternion wxyz', () => {
    const sample: HeadPoseSampleV1 = {
      version: 1,
      sequence: 42,
      timestampQpc: '123456789',
      quaternion: { x: 0.1, y: 0.2, z: 0.3, w: 0.9 },
      angularVelocity: { x: 1, y: 2, z: 3 },
      confidence: 0.8,
      trackingState: 'tracked',
    };
    const packet = packHeadPoseV1(sample);
    const view = new DataView(packet.buffer);
    expect(packet).toHaveLength(64);
    expect(new TextDecoder().decode(packet.slice(0, 4))).toBe('SSP1');
    expect(view.getUint16(6, true)).toBe(2);
    expect(view.getBigUint64(8, true)).toBe(42n);
    expect(view.getFloat32(24, true)).toBeCloseTo(0.9);
    expect(view.getFloat32(28, true)).toBeCloseTo(0.1);
    expect(view.getUint32(60, true)).not.toBe(0);
  });
});

describe('EngineStatusV1 wire', () => {
  it('convertit frames, ratio et états en métriques UI', () => {
    const wire: WireEngineStatusV1 = {
      schemaVersion: 1,
      audioMode: 'exclusive-pro',
      renderSampleFormat: 'pcm-s32',
      inputLayout: '5.1-surround',
      captureChannels: 6,
      captureChannelMask: 0x60f,
      captureState: 'running', renderState: 'degraded', trackingState: 'tracking',
      captureSampleRate: 48_000, renderSampleRate: 48_000,
      capturePeriodFrames: 128, renderPeriodFrames: 256, fifoFillFrames: 512,
      xruns: 2, callbackCpuPercent: 21, trackingHz: 59,
      latencyP50Ms: 12, latencyP95Ms: 18.5, resampleRatio: 1.0001, potentiallyBinaural: true, lastError: '',
    };
    const status = fromWireEngineStatus(wire);
    expect(status.trackingActive).toBe(true);
    expect(status.trackingHz).toBe(59);
    expect(status.connection).toBe('degraded');
    expect(status.audioMode).toBe('exclusive-pro');
    expect(status.renderSampleFormat).toBe('pcm-s32');
    expect(status.inputLayout).toBe('5.1-surround');
    expect(status.captureChannels).toBe(6);
    expect(status.captureChannelMask).toBe(0x60f);
    expect(status.capturePeriodMs).toBeCloseTo(2.6667, 3);
    expect(status.renderPeriodMs).toBeCloseTo(5.3333, 3);
    expect(status.fifoFillFrames).toBe(512);
    expect(status.clockDriftPpm).toBeCloseTo(100, 4);
    expect(status.motionToSoundLatencyMs.p95).toBe(18.5);
    expect(status.potentiallyBinaural).toBe(true);
    expect(status.lastError).toBeNull();
  });

  it('applique les valeurs stéréo par défaut avec un ancien moteur', () => {
    const wire: WireEngineStatusV1 = {
      schemaVersion: 1,
      audioMode: 'shared-low-latency',
      captureState: 'running', renderState: 'running', trackingState: 'tracking',
      captureSampleRate: 48_000, renderSampleRate: 48_000,
      capturePeriodFrames: 128, renderPeriodFrames: 128, fifoFillFrames: 256,
      xruns: 0, callbackCpuPercent: 10, trackingHz: 60,
      latencyP50Ms: 10, latencyP95Ms: 15, resampleRatio: 1, potentiallyBinaural: false, lastError: '',
    };
    expect(fromWireEngineStatus(wire)).toMatchObject({
      inputLayout: 'stereo',
      spatialInputMode: 'endpoint-mix',
      captureChannels: 2,
      captureChannelMask: 0x3,
      windowAudio: {
        supported: false,
        running: false,
        fifoOverruns: 0,
        fifoUnderruns: 0,
        displays: [],
        windowSources: [],
      },
    });
  });

  it('adapte la télémétrie optionnelle des écrans et sources L/R', async () => {
    const wire: WireEngineStatusV1 = {
      schemaVersion: 1,
      audioMode: 'shared-low-latency',
      spatialInputMode: 'process-windows',
      requestedSpatialInputMode: 'process-windows',
      captureState: 'running', renderState: 'running', trackingState: 'tracking',
      captureSampleRate: 48_000, renderSampleRate: 48_000,
      capturePeriodFrames: 128, renderPeriodFrames: 128, fifoFillFrames: 256,
      xruns: 0, callbackCpuPercent: 10, trackingHz: 60,
      latencyP50Ms: 10, latencyP95Ms: 15, resampleRatio: 1,
      potentiallyBinaural: false, lastError: '',
      windowAudio: {
        supported: true,
        running: true,
        sourceCount: 1,
        sequence: 3,
        diagnostics: { fifoOverruns: 3, fifoUnderruns: 5, lastError: '' },
        displays: [{
          id: 'right',
          name: 'Écran droit',
          isPrimary: false,
          boundsPx: { left: 1920, top: 0, right: 3840, bottom: 1080 },
          centerM: [0.8, 1.2, 0.9],
          widthM: 0.6,
          heightM: 0.34,
          orientation: [1, 0, 0, 0],
        }],
        windowSources: [{
          sourceId: 'source-1',
          applicationId: 'player.exe',
          applicationName: 'Lecteur',
          windowTitle: 'Musique',
          processId: 42,
          displayId: 'right',
          active: true,
          leftPositionM: [0.6, 1.2, 0.9],
          rightPositionM: [1, 1.2, 0.9],
          gainDb: 0,
          sampleRate: 48_000,
          channelCount: 2,
          captureState: 'capturing',
        }],
      },
    };
    const { isWireEngineStatusV1 } = await import('./contracts');
    expect(isWireEngineStatusV1(wire)).toBe(true);
    expect(fromWireEngineStatus(wire)).toMatchObject({
      spatialInputMode: 'process-windows',
      requestedSpatialInputMode: 'process-windows',
      windowAudio: {
        supported: true,
        running: true,
        sourceCount: 1,
        fifoOverruns: 3,
        fifoUnderruns: 5,
        displays: [{ center: { x: 0.8, y: 1.2, z: 0.9 } }],
        windowSources: [{
          applicationId: 'player.exe',
          leftPosition: { x: 0.6, y: 1.2, z: 0.9 },
          rightPosition: { x: 1, y: 1.2, z: 0.9 },
        }],
      },
    });

    const invalidCounters = structuredClone(wire);
    invalidCounters.windowAudio!.diagnostics.fifoUnderruns = -1;
    expect(isWireEngineStatusV1(invalidCounters)).toBe(false);
  });

  it('distingue le mode fenêtres demandé du repli endpoint effectif', () => {
    const wire: WireEngineStatusV1 = {
      schemaVersion: 1,
      audioMode: 'shared-low-latency',
      spatialInputMode: 'endpoint-mix',
      requestedSpatialInputMode: 'process-windows',
      captureState: 'running', renderState: 'running', trackingState: 'tracking',
      captureSampleRate: 48_000, renderSampleRate: 48_000,
      capturePeriodFrames: 128, renderPeriodFrames: 128, fifoFillFrames: 256,
      xruns: 0, callbackCpuPercent: 10, trackingHz: 60,
      latencyP50Ms: 10, latencyP95Ms: 15, resampleRatio: 1,
      potentiallyBinaural: false, lastError: '',
    };

    expect(fromWireEngineStatus(wire)).toMatchObject({
      spatialInputMode: 'endpoint-mix',
      requestedSpatialInputMode: 'process-windows',
    });
  });

  it('accepte la télémétrie multicanale nulle tant que le moteur est arrêté', async () => {
    const { isWireEngineStatusV1 } = await import('./contracts');
    expect(isWireEngineStatusV1({
      schemaVersion: 1,
      audioMode: 'shared-low-latency',
      inputLayout: '5.1-surround',
      captureChannels: 0,
      captureChannelMask: 0,
      captureState: 'stopped', renderState: 'stopped', trackingState: 'lost',
      captureSampleRate: 0, renderSampleRate: 0,
      capturePeriodFrames: 0, renderPeriodFrames: 0, fifoFillFrames: 0,
      xruns: 0, callbackCpuPercent: 0, trackingHz: 0,
      latencyP50Ms: 0, latencyP95Ms: 0, resampleRatio: 1, potentiallyBinaural: false, lastError: '',
    })).toBe(true);
  });

  it('rejette un statut partiel ou non fini avant adaptation', async () => {
    const { isWireEngineStatusV1 } = await import('./contracts');
    expect(isWireEngineStatusV1({ schemaVersion: 1, captureState: 'running' })).toBe(false);
    expect(isWireEngineStatusV1({
      schemaVersion: 1,
      audioMode: 'shared-low-latency',
      captureState: 'running', renderState: 'running', trackingState: 'tracking',
      captureSampleRate: 48_000, renderSampleRate: 48_000,
      capturePeriodFrames: 128, renderPeriodFrames: 128, fifoFillFrames: 256,
      xruns: 0, callbackCpuPercent: Number.NaN, trackingHz: 60,
      latencyP50Ms: 10, latencyP95Ms: 15, resampleRatio: 1, potentiallyBinaural: false, lastError: '',
    })).toBe(false);
  });
});
