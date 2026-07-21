import { describe, expect, it } from 'vitest';
import type { AudioDeviceSummary } from '../types/contracts';
import { deriveRuntimeCapabilities, isAudioRouteStructurallyComplete, sameAudioEndpoint, shouldAutoStartAudio } from './runtime-capabilities';

const physical: AudioDeviceSummary = {
  id: 'headphones',
  name: 'Casque USB',
  isDefault: true,
  isSoundSpatializerEndpoint: false,
  transport: 'usb',
  sampleRate: 48_000,
};

const native: AudioDeviceSummary = {
  id: 'sound-spatializer',
  name: 'Sound Spatializer',
  isDefault: false,
  isSoundSpatializerEndpoint: true,
  transport: 'unknown',
  sampleRate: 48_000,
};

const external: AudioDeviceSummary = {
  id: 'external-cable',
  name: 'Câble audio externe',
  isDefault: false,
  isSoundSpatializerEndpoint: false,
  transport: 'unknown',
  sampleRate: 48_000,
};

const nativeRoute = {
  captureProvider: 'native-driver' as const,
  captureEndpointId: null,
  physicalOutputDeviceId: physical.id,
};

describe('capacités audio locales', () => {
  it('active le chemin natif quand le pilote et une sortie distincte sont disponibles', () => {
    expect(deriveRuntimeCapabilities(true, [physical, native], nativeRoute)).toMatchObject({
      driverEndpointAvailable: true,
      audioRouteReady: true,
      previewMode: false,
      routeIssue: null,
    });
  });

  it('active une source de rendu externe explicitement choisie sans pilote natif', () => {
    const capabilities = deriveRuntimeCapabilities(true, [physical, external], {
      captureProvider: 'external-render',
      captureEndpointId: external.id,
      physicalOutputDeviceId: physical.id,
    });
    expect(capabilities).toMatchObject({
      driverEndpointAvailable: false,
      audioRouteReady: true,
      previewMode: false,
      routeIssue: null,
    });
    expect(capabilities.captureEndpoint?.id).toBe(external.id);
  });

  it('ne choisit jamais automatiquement un endpoint externe', () => {
    expect(deriveRuntimeCapabilities(true, [physical, external], nativeRoute).routeIssue).toBe('native-endpoint-unavailable');
    expect(deriveRuntimeCapabilities(true, [physical, external], {
      captureProvider: 'external-render',
      captureEndpointId: null,
      physicalOutputDeviceId: physical.id,
    }).routeIssue).toBe('capture-endpoint-required');
  });

  it('refuse une source disparue, le pilote natif comme source externe et une sortie native', () => {
    expect(deriveRuntimeCapabilities(true, [physical], {
      captureProvider: 'external-render', captureEndpointId: 'missing', physicalOutputDeviceId: physical.id,
    }).routeIssue).toBe('capture-endpoint-unavailable');
    expect(deriveRuntimeCapabilities(true, [physical, native], {
      captureProvider: 'external-render', captureEndpointId: native.id, physicalOutputDeviceId: physical.id,
    }).routeIssue).toBe('capture-endpoint-is-native');
    expect(deriveRuntimeCapabilities(true, [physical, native], {
      ...nativeRoute, physicalOutputDeviceId: native.id,
    }).routeIssue).toBe('output-endpoint-is-native');
  });

  it('bloque la boucle source-sortie même si la casse de l’ID diffère', () => {
    expect(sameAudioEndpoint('External-Cable', 'external-cable')).toBe(true);
    expect(deriveRuntimeCapabilities(true, [external], {
      captureProvider: 'external-render',
      captureEndpointId: 'EXTERNAL-CABLE',
      physicalOutputDeviceId: 'external-cable',
    }).routeIssue).toBe('capture-equals-output');
    expect(isAudioRouteStructurallyComplete({
      captureProvider: 'external-render', captureEndpointId: 'SAME', physicalOutputDeviceId: 'same',
    })).toBe(false);
  });

  it('n’envoie pas une scène externe incomplète au moteur', () => {
    expect(isAudioRouteStructurallyComplete({
      captureProvider: 'external-render', captureEndpointId: null, physicalOutputDeviceId: physical.id,
    })).toBe(false);
    expect(isAudioRouteStructurallyComplete(nativeRoute)).toBe(true);
    expect(isAudioRouteStructurallyComplete({
      captureProvider: 'native-driver', captureEndpointId: null, physicalOutputDeviceId: null,
    })).toBe(false);
  });

  it('conserve le mode Preview dans un navigateur et interdit l’auto-start incomplet', () => {
    const browser = deriveRuntimeCapabilities(false, [physical, native], nativeRoute);
    expect(browser.routeIssue).toBe('desktop-runtime-required');
    expect(browser.previewMode).toBe(true);
    expect(shouldAutoStartAudio(browser, true)).toBe(false);

    const ready = deriveRuntimeCapabilities(true, [physical, native], nativeRoute);
    expect(shouldAutoStartAudio(ready, false)).toBe(false);
    expect(shouldAutoStartAudio(ready, true)).toBe(true);
  });
});
