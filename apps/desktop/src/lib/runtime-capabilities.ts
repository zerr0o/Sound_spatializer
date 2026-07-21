import type { AudioDeviceSummary, CaptureProvider, SceneConfigV1 } from '../types/contracts';

export type AudioRouteIssue =
  | 'desktop-runtime-required'
  | 'native-endpoint-unavailable'
  | 'capture-endpoint-required'
  | 'capture-endpoint-unavailable'
  | 'capture-endpoint-is-native'
  | 'output-endpoint-required'
  | 'output-endpoint-unavailable'
  | 'output-endpoint-is-native'
  | 'capture-equals-output';

export interface AudioRouteSelection {
  captureProvider: CaptureProvider;
  captureEndpointId: string | null;
  physicalOutputDeviceId: string | null;
}

export interface RuntimeCapabilities {
  driverEndpointAvailable: boolean;
  audioRouteReady: boolean;
  previewMode: boolean;
  routeIssue: AudioRouteIssue | null;
  captureEndpoint: AudioDeviceSummary | null;
  outputEndpoint: AudioDeviceSummary | null;
}

const normalizedEndpointId = (value: string | null | undefined): string => value?.trim().toLocaleLowerCase('en-US') ?? '';

export const sameAudioEndpoint = (left: string | null | undefined, right: string | null | undefined): boolean => {
  const normalizedLeft = normalizedEndpointId(left);
  return normalizedLeft.length > 0 && normalizedLeft === normalizedEndpointId(right);
};

const findEndpoint = (devices: AudioDeviceSummary[], id: string | null): AudioDeviceSummary | null => {
  if (!id) return null;
  return devices.find((device) => sameAudioEndpoint(device.id, id)) ?? null;
};

export const routeSelectionFromScene = (
  scene: Pick<SceneConfigV1, 'captureProvider' | 'captureEndpointId' | 'physicalOutputDeviceId'>,
): AudioRouteSelection => ({
  captureProvider: scene.captureProvider,
  captureEndpointId: scene.captureEndpointId,
  physicalOutputDeviceId: scene.physicalOutputDeviceId,
});

export const isAudioRouteStructurallyComplete = (selection: AudioRouteSelection): boolean => {
  if (selection.captureProvider === 'native-driver') return Boolean(selection.physicalOutputDeviceId);
  return Boolean(
    selection.captureEndpointId
    && selection.physicalOutputDeviceId
    && !sameAudioEndpoint(selection.captureEndpointId, selection.physicalOutputDeviceId),
  );
};

export const deriveRuntimeCapabilities = (
  tauriRuntime: boolean,
  devices: AudioDeviceSummary[],
  selection: AudioRouteSelection,
): RuntimeCapabilities => {
  const nativeEndpoint = devices.find((device) => device.isSoundSpatializerEndpoint) ?? null;
  const driverEndpointAvailable = nativeEndpoint !== null;
  const captureEndpoint = selection.captureProvider === 'native-driver'
    ? nativeEndpoint
    : findEndpoint(devices, selection.captureEndpointId);
  const outputEndpoint = findEndpoint(devices, selection.physicalOutputDeviceId);

  let routeIssue: AudioRouteIssue | null = null;
  if (!tauriRuntime) routeIssue = 'desktop-runtime-required';
  else if (selection.captureProvider === 'native-driver' && !nativeEndpoint) routeIssue = 'native-endpoint-unavailable';
  else if (selection.captureProvider === 'external-render' && !selection.captureEndpointId) routeIssue = 'capture-endpoint-required';
  else if (selection.captureProvider === 'external-render' && !captureEndpoint) routeIssue = 'capture-endpoint-unavailable';
  else if (selection.captureProvider === 'external-render' && captureEndpoint?.isSoundSpatializerEndpoint) routeIssue = 'capture-endpoint-is-native';
  else if (!selection.physicalOutputDeviceId) routeIssue = 'output-endpoint-required';
  else if (!outputEndpoint) routeIssue = 'output-endpoint-unavailable';
  else if (outputEndpoint.isSoundSpatializerEndpoint) routeIssue = 'output-endpoint-is-native';
  else if (sameAudioEndpoint(captureEndpoint?.id, outputEndpoint.id)) routeIssue = 'capture-equals-output';

  const audioRouteReady = routeIssue === null;
  return {
    driverEndpointAvailable,
    audioRouteReady,
    previewMode: !audioRouteReady,
    routeIssue,
    captureEndpoint,
    outputEndpoint,
  };
};

export const shouldAutoStartAudio = (
  capabilities: RuntimeCapabilities,
  onboardingComplete: boolean,
): boolean => capabilities.audioRouteReady && onboardingComplete;
