import { describe, expect, it } from 'vitest';
import type { AudioDeviceSummary } from '../types/contracts';
import {
  deriveRuntimeCapabilities,
  isAudioRouteStructurallyComplete,
  sameAudioEndpoint,
  shouldAutoStartAudio,
  supportsSurround5_1,
} from './runtime-capabilities';

const device = (
  id: string,
  overrides: Partial<AudioDeviceSummary> = {},
): AudioDeviceSummary => ({
  id,
  name: id,
  isDefault: false,
  isSoundSpatializerEndpoint: false,
  transport: 'unknown',
  sampleRate: 48_000,
  channelCount: 2,
  channelMask: 0x3,
  ...overrides,
});

const physical = device('headphones', { name: 'Casque USB', isDefault: true, transport: 'usb' });
const native = device('sound-spatializer', { name: 'Sound Spatializer', isSoundSpatializerEndpoint: true });
const external = device('external-cable', { name: 'Câble audio externe' });
const surround = device('external-5.1', { channelCount: 6, channelMask: 0x60f });

const nativeRoute = {
  captureProvider: 'native-driver' as const,
  captureEndpointId: null,
  physicalOutputDeviceId: physical.id,
  inputLayout: 'stereo' as const,
};

const externalRoute = {
  captureProvider: 'external-render' as const,
  captureEndpointId: external.id,
  physicalOutputDeviceId: physical.id,
  inputLayout: 'stereo' as const,
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
    const capabilities = deriveRuntimeCapabilities(true, [physical, external], externalRoute);
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
      ...externalRoute,
      captureEndpointId: null,
    }).routeIssue).toBe('capture-endpoint-required');
  });

  it('refuse une source disparue, le pilote natif comme source externe et une sortie native', () => {
    expect(deriveRuntimeCapabilities(true, [physical], {
      ...externalRoute,
      captureEndpointId: 'missing',
    }).routeIssue).toBe('capture-endpoint-unavailable');
    expect(deriveRuntimeCapabilities(true, [physical, native], {
      ...externalRoute,
      captureEndpointId: native.id,
    }).routeIssue).toBe('capture-endpoint-is-native');
    expect(deriveRuntimeCapabilities(true, [physical, native], {
      ...nativeRoute,
      physicalOutputDeviceId: native.id,
    }).routeIssue).toBe('output-endpoint-is-native');
  });

  it('bloque la boucle source-sortie même si la casse de l’ID diffère', () => {
    expect(sameAudioEndpoint('External-Cable', 'external-cable')).toBe(true);
    expect(deriveRuntimeCapabilities(true, [external], {
      ...externalRoute,
      captureEndpointId: 'EXTERNAL-CABLE',
      physicalOutputDeviceId: 'external-cable',
    }).routeIssue).toBe('capture-equals-output');
    expect(isAudioRouteStructurallyComplete({
      ...externalRoute,
      captureEndpointId: 'SAME',
      physicalOutputDeviceId: 'same',
    })).toBe(false);
  });

  it('n’envoie pas une scène externe incomplète au moteur', () => {
    expect(isAudioRouteStructurallyComplete({ ...externalRoute, captureEndpointId: null })).toBe(false);
    expect(isAudioRouteStructurallyComplete(nativeRoute)).toBe(true);
    expect(isAudioRouteStructurallyComplete({ ...nativeRoute, physicalOutputDeviceId: null })).toBe(false);
  });

  it('autorise le 5.1 uniquement sur une capture externe 6 canaux avec masque canonique', () => {
    const surroundRoute = {
      ...externalRoute,
      captureEndpointId: surround.id,
      inputLayout: '5.1-surround' as const,
    };
    expect(deriveRuntimeCapabilities(true, [physical, surround], surroundRoute).routeIssue).toBeNull();
    expect(supportsSurround5_1(surround)).toBe(true);
    expect(supportsSurround5_1({ ...surround, channelMask: 0x3f })).toBe(true);

    for (const invalid of [
      { ...surround, channelCount: 2, channelMask: 0x3 },
      { ...surround, channelCount: 8 },
      { ...surround, channelMask: 0x63f },
    ]) {
      expect(supportsSurround5_1(invalid)).toBe(false);
      expect(deriveRuntimeCapabilities(true, [physical, invalid], surroundRoute).routeIssue)
        .toBe('capture-layout-unsupported');
    }
    expect(deriveRuntimeCapabilities(true, [physical, native], {
      ...nativeRoute,
      inputLayout: '5.1-surround',
    }).routeIssue).toBe('capture-layout-unsupported');
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
