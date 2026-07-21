import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { defaultPreferences, defaultScene, emptyTrackingMetrics } from '../data/defaults';
import { desktopBridge } from '../lib/tauri-bridge';
import { useAppStore } from '../store/app-store';
import type { AudioDeviceSummary, CaptureProvider } from '../types/contracts';
import { AssistantPage } from './AssistantPage';

vi.mock('../tracking/TrackingProvider', () => ({
  useTrackingController: () => ({
    start: vi.fn().mockResolvedValue(undefined),
    stop: vi.fn(),
    calibrate: vi.fn().mockResolvedValue(true),
    running: false,
    videoElement: null,
  }),
}));

const physicalDevice: AudioDeviceSummary = {
  id: 'physical-headphones',
  name: 'Casque USB de test',
  isDefault: true,
  isSoundSpatializerEndpoint: false,
  transport: 'usb',
  sampleRate: 48_000,
};

const externalDevice: AudioDeviceSummary = {
  id: 'external-render',
  name: 'Câble virtuel de test',
  isDefault: false,
  isSoundSpatializerEndpoint: false,
  transport: 'unknown',
  sampleRate: 48_000,
};

const alternativeExternalDevice: AudioDeviceSummary = {
  id: 'external-render-alternative',
  name: 'Câble virtuel alternatif',
  isDefault: false,
  isSoundSpatializerEndpoint: false,
  transport: 'unknown',
  sampleRate: 48_000,
};

const nativeDevice: AudioDeviceSummary = {
  id: 'sound-spatializer',
  name: 'Sound Spatializer',
  isDefault: false,
  isSoundSpatializerEndpoint: true,
  transport: 'unknown',
  sampleRate: 48_000,
};

const setAssistantState = (
  step: number,
  devices: AudioDeviceSummary[],
  captureProvider: CaptureProvider = 'native-driver',
) => {
  const scene = structuredClone(defaultScene);
  scene.captureProvider = captureProvider;
  scene.captureEndpointId = null;
  useAppStore.setState({
    activeView: 'assistant',
    initialized: true,
    previewMode: true,
    driverEndpointAvailable: devices.some((device) => device.isSoundSpatializerEndpoint),
    audioRouteReady: false,
    audioRouteIssue: null,
    scene,
    preferences: { ...defaultPreferences, onboardingStep: step, onboardingComplete: false },
    tracking: structuredClone(emptyTrackingMetrics),
    audioDevices: devices,
    toasts: [],
  });
};

describe('assistant de routage audio', () => {
  beforeEach(() => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { configurable: true, value: {} });
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
    Reflect.deleteProperty(window, '__TAURI_INTERNALS__');
  });

  it('conserve un aperçu explicite quand le pilote natif est absent', async () => {
    setAssistantState(1, [physicalDevice]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    render(<AssistantPage />);

    expect(screen.getAllByText('Pilote natif indisponible').length).toBeGreaterThan(0);
    fireEvent.click(screen.getByRole('button', { name: /Sortie : Casque USB de test/ }));
    expect(useAppStore.getState().scene.physicalOutputDeviceId).toBe(physicalDevice.id);
    expect(sendCommand).not.toHaveBeenCalled();

    act(() => useAppStore.getState().patchPreferences({ onboardingStep: 3 }));
    cleanup();
    render(<AssistantPage />);
    fireEvent.click(screen.getByRole('button', { name: /Terminer/ }));
    await waitFor(() => expect(useAppStore.getState().activeView).toBe('scene'));
    expect(sendCommand).not.toHaveBeenCalled();
    expect(useAppStore.getState().toasts.at(-1)?.title).toBe('Aperçu sans audio système');
  });

  it('configure explicitement un endpoint externe et un casque distinct dans une seule commande', async () => {
    setAssistantState(1, [externalDevice, physicalDevice]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    render(<AssistantPage />);

    fireEvent.click(screen.getByRole('radio', { name: /Endpoint de rendu externe/ }));
    fireEvent.click(screen.getByRole('button', { name: /Source : Câble virtuel de test/ }));
    expect(screen.getByRole('button', { name: /Sortie : Câble virtuel de test — Déjà utilisé comme source/ })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: /Sortie : Casque USB de test/ }));

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: externalDevice.id,
      outputDeviceId: physicalDevice.id,
    }));
    expect(useAppStore.getState().scene).toMatchObject({
      captureProvider: 'external-render',
      captureEndpointId: externalDevice.id,
      physicalOutputDeviceId: physicalDevice.id,
    });
    expect(screen.getByText(/choisissez « Câble virtuel de test » comme sortie/)).toBeInTheDocument();
  });

  it('applique aussi le pilote natif par la commande de route atomique', async () => {
    setAssistantState(1, [nativeDevice, physicalDevice]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    render(<AssistantPage />);

    fireEvent.click(screen.getByRole('button', { name: /Sortie : Casque USB de test/ }));
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'native-driver',
      captureEndpointId: null,
      outputDeviceId: physicalDevice.id,
    }));
  });

  it('ne persiste pas une route complète refusée par le moteur', async () => {
    setAssistantState(1, [nativeDevice, physicalDevice]);
    vi.spyOn(desktopBridge, 'sendCommand').mockRejectedValue(new Error('endpoint occupé'));
    render(<AssistantPage />);

    fireEvent.click(screen.getByRole('button', { name: /Sortie : Casque USB de test/ }));
    await waitFor(() => expect(useAppStore.getState().toasts.at(-1)?.title).toBe('Routage audio non appliqué'));
    expect(useAppStore.getState().scene.physicalOutputDeviceId).toBeNull();
  });

  it('réapplique la route externe avant de démarrer à la fin de l’assistant', async () => {
    setAssistantState(3, [externalDevice, physicalDevice], 'external-render');
    useAppStore.getState().patchScene({
      captureEndpointId: externalDevice.id,
      physicalOutputDeviceId: physicalDevice.id,
    });
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    render(<AssistantPage />);

    fireEvent.click(screen.getByRole('button', { name: /Terminer/ }));
    await waitFor(() => expect(useAppStore.getState().activeView).toBe('scene'));
    expect(sendCommand).toHaveBeenNthCalledWith(1, expect.objectContaining({
      version: 1,
      type: 'set-scene',
    }));
    expect(sendCommand).toHaveBeenNthCalledWith(2, {
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: externalDevice.id,
      outputDeviceId: physicalDevice.id,
    });
    expect(sendCommand).toHaveBeenNthCalledWith(3, { version: 1, type: 'start' });
  });

  it('verrouille les choix pendant une commande atomique afin d’éviter deux routes concurrentes', async () => {
    setAssistantState(1, [externalDevice, alternativeExternalDevice, physicalDevice], 'external-render');
    useAppStore.getState().patchScene({
      captureEndpointId: externalDevice.id,
      physicalOutputDeviceId: physicalDevice.id,
    });
    let resolveRoute!: () => void;
    const pendingRoute = new Promise<void>((resolve) => { resolveRoute = resolve; });
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockReturnValue(pendingRoute);
    render(<AssistantPage />);

    fireEvent.click(screen.getByRole('button', { name: /Source : Câble virtuel alternatif/ }));
    const conflictingOutput = await screen.findByRole('button', { name: /Sortie : Câble virtuel alternatif — Application du routage en cours/ });
    expect(conflictingOutput).toBeDisabled();
    fireEvent.click(conflictingOutput);
    expect(sendCommand).toHaveBeenCalledTimes(1);

    await act(async () => {
      resolveRoute();
      await pendingRoute;
    });
    expect(useAppStore.getState().scene).toMatchObject({
      captureEndpointId: alternativeExternalDevice.id,
      physicalOutputDeviceId: physicalDevice.id,
    });
    expect(screen.getByRole('button', { name: /Source : Câble virtuel alternatif/ })).toHaveAttribute('aria-pressed', 'true');
  });
});
