import { act, cleanup, render, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import App from './App';
import { defaultPreferences, defaultScene, emptyEngineStatus } from './data/defaults';
import { desktopBridge } from './lib/tauri-bridge';
import { useAppStore } from './store/app-store';
import type { AudioDeviceSummary, PersistedAppConfigV1 } from './types/contracts';

vi.mock('./tracking/TrackingProvider', () => ({
  useTrackingController: () => ({ calibrate: vi.fn().mockResolvedValue(true) }),
}));

vi.mock('./pages/ScenePage', () => ({ ScenePage: () => <div>Scène test</div> }));

const physical: AudioDeviceSummary = {
  id: 'usb-headset',
  name: 'Casque USB',
  isDefault: false,
  isSoundSpatializerEndpoint: false,
  transport: 'usb',
  sampleRate: 48_000,
};

const external: AudioDeviceSummary = {
  id: 'external-render',
  name: 'Endpoint externe',
  isDefault: true,
  isSoundSpatializerEndpoint: false,
  transport: 'unknown',
  sampleRate: 48_000,
};

const externalConfig = (): PersistedAppConfigV1 => {
  const scene = structuredClone(defaultScene);
  scene.captureProvider = 'external-render';
  scene.captureEndpointId = external.id;
  scene.physicalOutputDeviceId = physical.id;
  return {
    schemaVersion: 1,
    scene,
    preferences: { ...defaultPreferences, onboardingComplete: true },
  };
};

describe('bootstrap du routage audio', () => {
  beforeEach(() => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { configurable: true, value: {} });
    useAppStore.setState({ initialized: false, audioDevices: [], toasts: [] });
    vi.spyOn(desktopBridge, 'startEngine').mockResolvedValue();
    vi.spyOn(desktopBridge, 'getEngineStatus').mockResolvedValue(structuredClone(emptyEngineStatus));
    vi.spyOn(desktopBridge, 'saveConfig').mockResolvedValue();
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
    Reflect.deleteProperty(window, '__TAURI_INTERNALS__');
  });

  it('applique une route externe avant l’auto-start, même sans endpoint du pilote natif', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    const commands = sendCommand.mock.calls.map(([command]) => command);
    expect(commands[0]).toMatchObject({ type: 'set-scene' });
    expect(commands[1]).toEqual({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: external.id,
      outputDeviceId: physical.id,
    });
    expect(commands[2]).toEqual({ version: 1, type: 'start' });
    expect(useAppStore.getState()).toMatchObject({
      driverEndpointAvailable: false,
      audioRouteReady: true,
      previewMode: false,
    });
  });

  it('échoue fermé quand la source externe sauvegardée a disparu', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith(expect.objectContaining({ type: 'set-scene' })));
    expect(sendCommand).not.toHaveBeenCalledWith(expect.objectContaining({ type: 'set-audio-route' }));
    expect(sendCommand).not.toHaveBeenCalledWith({ version: 1, type: 'start' });
    expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'stop' });
    expect(useAppStore.getState()).toMatchObject({
      audioRouteReady: false,
      previewMode: true,
      audioRouteIssue: 'capture-endpoint-unavailable',
    });
  });

  it('arrête le moteur si une source active disparaît après le bootstrap', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    sendCommand.mockClear();

    act(() => useAppStore.getState().setAudioDevices([physical]));

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'stop' }));
    expect(useAppStore.getState()).toMatchObject({
      audioRouteReady: false,
      previewMode: true,
      audioRouteIssue: 'capture-endpoint-unavailable',
    });
  });

  it('arrête l’ancienne route sans envoyer une scène externe incomplète', async () => {
    const incomplete = externalConfig();
    incomplete.scene.captureEndpointId = null;
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(incomplete);
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'stop' }));
    expect(sendCommand).not.toHaveBeenCalledWith(expect.objectContaining({ type: 'set-scene' }));
    expect(sendCommand).not.toHaveBeenCalledWith(expect.objectContaining({ type: 'set-audio-route' }));
  });

  it('réapplique puis redémarre une route redevenue disponible après une erreur d’énumération', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices')
      .mockRejectedValueOnce(new Error('énumération WASAPI temporairement indisponible'))
      .mockResolvedValue([external, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);
    await waitFor(() => expect(useAppStore.getState().initialized).toBe(true));
    expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'stop' });
    sendCommand.mockClear();

    act(() => window.dispatchEvent(new Event('focus')));

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith(expect.objectContaining({ type: 'set-scene' })));
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: external.id,
      outputDeviceId: physical.id,
    }));
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    const recoveredCommands = sendCommand.mock.calls.map(([command]) => command.type);
    expect(recoveredCommands.indexOf('set-scene')).toBeLessThan(recoveredCommands.indexOf('set-audio-route'));
    expect(useAppStore.getState()).toMatchObject({ audioRouteReady: true, previewMode: false });
  });

  it('réconcilie au démarrage un ancien mode Pro refusé avec le fallback effectif', async () => {
    const config = externalConfig();
    config.scene.audioMode = 'exclusive-pro';
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(config);
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    vi.mocked(desktopBridge.getEngineStatus).mockResolvedValue({
      ...structuredClone(emptyEngineStatus),
      audioMode: 'shared-low-latency',
      connection: 'degraded',
      captureActive: true,
      renderActive: true,
      lastError: 'MMCSS unavailable; AUDIO_MODE_FALLBACK requested=exclusive-pro effective=shared-low-latency reason=format refused',
    });

    render(<App />);

    await waitFor(() => expect(useAppStore.getState().scene.audioMode).toBe('shared-low-latency'));
    expect(useAppStore.getState().toasts.at(-1)).toMatchObject({
      tone: 'warning',
      title: 'Mode Pro exclusif indisponible',
    });
  });
});
