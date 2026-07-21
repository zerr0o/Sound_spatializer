import { describe, expect, it } from 'vitest';
import { defaultScene } from '../data/defaults';
import type { HeadPoseSampleV1 } from '../types/contracts';
import type { WireEngineStatusV1 } from '../types/wire-contracts';
import {
  fromWireEngineStatus,
  fromWireSceneConfig,
  packHeadPoseV1,
  quaternionToWire,
  migratePersistedConfig,
  toWireEngineCommand,
  toWireSceneConfig,
} from './contracts';

describe('SceneConfigV1 wire', () => {
  it('respecte le repère +Z avant et l’ordre de quaternion wxyz', () => {
    const scene = structuredClone(defaultScene);
    scene.listener.neutralPose = { x: 0.1, y: 0.2, z: 0.3, w: 0.9 };
    const wire = toWireSceneConfig(scene);
    expect(wire.speakers[0].positionM).toEqual([-1, 1.2, 1.732051]);
    expect(wire.speakers[1].positionM).toEqual([1, 1.2, 1.732051]);
    expect(wire.listener.neutralOrientation).toEqual([0.9, 0.1, 0.2, 0.3]);
    expect(quaternionToWire(scene.listener.neutralPose)).toEqual([0.9, 0.1, 0.2, 0.3]);
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

  it('migre une scène V1 antérieure vers le pilote natif et écrit les champs canoniques', () => {
    const legacy = toWireSceneConfig(defaultScene);
    delete legacy.audio.captureProvider;
    delete legacy.audio.captureEndpointId;
    const migrated = migratePersistedConfig({ schemaVersion: 1, scene: legacy, preferences: {} });
    expect(migrated?.scene.captureProvider).toBe('native-driver');
    expect(migrated?.scene.captureEndpointId).toBeNull();
    expect(toWireSceneConfig(migrated!.scene).audio).toMatchObject({
      captureProvider: 'native-driver',
      captureEndpointId: null,
    });
  });

  it('préserve un choix externe encore incomplet sans réinitialiser la scène', () => {
    const incomplete = toWireSceneConfig({
      ...structuredClone(defaultScene),
      captureProvider: 'external-render',
      captureEndpointId: null,
    });
    const migrated = migratePersistedConfig({ schemaVersion: 1, scene: incomplete, preferences: {} });
    expect(migrated?.scene.captureProvider).toBe('external-render');
    expect(migrated?.scene.captureEndpointId).toBeNull();
  });

  it('migre scattering scalaire vers diffusion trois bandes', () => {
    const legacy = toWireSceneConfig(defaultScene) as unknown as { room: { lateReverbEnabled?: boolean; surfaces: Array<Record<string, unknown>> } };
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
    expect(status.capturePeriodMs).toBeCloseTo(2.6667, 3);
    expect(status.renderPeriodMs).toBeCloseTo(5.3333, 3);
    expect(status.fifoFillFrames).toBe(512);
    expect(status.clockDriftPpm).toBeCloseTo(100, 4);
    expect(status.motionToSoundLatencyMs.p95).toBe(18.5);
    expect(status.potentiallyBinaural).toBe(true);
    expect(status.lastError).toBeNull();
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
