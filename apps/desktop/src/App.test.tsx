import { act, cleanup, render, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import App from './App';
import { defaultPreferences, defaultScene, defaultWindowSpatialization, emptyEngineStatus } from './data/defaults';
import { desktopBridge } from './lib/tauri-bridge';
import { useAppStore } from './store/app-store';
import type { AudioDeviceSummary, PersistedAppConfigV3 } from './types/contracts';

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
  channelCount: 2,
  channelMask: 0x3,
};

const external: AudioDeviceSummary = {
  id: 'external-render',
  name: 'Endpoint externe',
  isDefault: true,
  isSoundSpatializerEndpoint: false,
  transport: 'unknown',
  sampleRate: 48_000,
  channelCount: 2,
  channelMask: 0x3,
};

const externalSurround: AudioDeviceSummary = {
  ...external,
  id: 'external-render-5-1',
  name: 'Endpoint externe 5.1',
  channelCount: 6,
  channelMask: 0x3f,
};

const externalConfig = (): PersistedAppConfigV3 => {
  const scene = structuredClone(defaultScene);
  scene.captureProvider = 'external-render';
  scene.captureEndpointId = external.id;
  scene.physicalOutputDeviceId = physical.id;
  return {
    schemaVersion: 3,
    scene,
    preferences: { ...defaultPreferences, onboardingComplete: true },
    windowSpatialization: structuredClone(defaultWindowSpatialization),
  };
};

describe('bootstrap du routage audio', () => {
  beforeEach(() => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { configurable: true, value: {} });
    useAppStore.setState({ initialized: false, audioDevices: [], toasts: [] });
    vi.spyOn(desktopBridge, 'startEngine').mockResolvedValue();
    vi.spyOn(desktopBridge, 'getEngineStatus').mockResolvedValue({
      ...structuredClone(emptyEngineStatus),
      connectionGeneration: 1,
    });
    vi.spyOn(desktopBridge, 'saveConfig').mockResolvedValue();
    vi.spyOn(desktopBridge, 'sendCommandWithGeneration')
      .mockImplementation(async (command) => {
        await desktopBridge.sendCommand(command);
        return 1;
      });
    vi.spyOn(desktopBridge, 'runCommandTransaction')
      .mockImplementation((operation) => operation((command) =>
        desktopBridge.sendCommand(command)));
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
    expect(commands[1]).toMatchObject({ type: 'set-window-spatialization' });
    expect(commands[2]).toEqual({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: external.id,
      outputDeviceId: physical.id,
    });
    expect(commands[3]).toEqual({ version: 1, type: 'start' });
    expect(useAppStore.getState()).toMatchObject({
      driverEndpointAvailable: false,
      audioRouteReady: true,
      previewMode: false,
    });
  });

  it('rejoue la configuration canonique après une nouvelle génération moteur', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    let generation = 1;
    vi.spyOn(desktopBridge, 'getEngineStatus').mockImplementation(async () => ({
      ...structuredClone(emptyEngineStatus),
      connectionGeneration: generation,
    }));
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    await waitFor(() => expect(useAppStore.getState().engine.connectionGeneration).toBe(1));
    sendCommand.mockClear();

    generation = 2;

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: external.id,
      outputDeviceId: physical.id,
    }));
    expect(sendCommand.mock.calls.map(([command]) => command.type)).toEqual([
      'set-window-spatialization',
      'set-scene',
      'set-audio-route',
      'start',
    ]);
  });

  it('rejoue si la génération change entre deux ACK du bootstrap', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    vi.mocked(desktopBridge.getEngineStatus).mockResolvedValue({
      ...structuredClone(emptyEngineStatus),
      connectionGeneration: 2,
    });
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    let acknowledgedCommands = 0;
    vi.mocked(desktopBridge.sendCommandWithGeneration)
      .mockImplementation(async (command) => {
        await desktopBridge.sendCommand(command);
        acknowledgedCommands += 1;
        return acknowledgedCommands === 1 ? 1 : 2;
      });

    render(<App />);

    await waitFor(() => expect(
      sendCommand.mock.calls.filter(([command]) => command.type === 'set-scene'),
    ).toHaveLength(2));
    expect(desktopBridge.sendCommandWithGeneration).toHaveBeenCalledTimes(4);
    expect(sendCommand.mock.calls.map(([command]) => command.type)).toEqual([
      'set-scene',
      'set-window-spatialization',
      'set-audio-route',
      'start',
      'set-window-spatialization',
      'set-scene',
      'set-audio-route',
      'start',
    ]);
  });

  it('rejoue si le moteur redémarre entre le dernier ACK et le statut final', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    vi.mocked(desktopBridge.getEngineStatus).mockResolvedValue({
      ...structuredClone(emptyEngineStatus),
      connectionGeneration: 8,
    });
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    vi.mocked(desktopBridge.sendCommandWithGeneration)
      .mockImplementation(async (command) => {
        await desktopBridge.sendCommand(command);
        return 7;
      });

    render(<App />);

    await waitFor(() => expect(
      sendCommand.mock.calls.filter(([command]) => command.type === 'set-scene'),
    ).toHaveLength(2));
    expect(desktopBridge.sendCommandWithGeneration).toHaveBeenCalledTimes(4);
    expect(sendCommand.mock.calls.map(([command]) => command.type)).toEqual([
      'set-scene',
      'set-window-spatialization',
      'set-audio-route',
      'start',
      'set-window-spatialization',
      'set-scene',
      'set-audio-route',
      'start',
    ]);
  });

  it('rejoue dès la première génération quand le bootstrap reste indéterminé', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    vi.spyOn(desktopBridge, 'getEngineStatus').mockResolvedValue({
      ...structuredClone(emptyEngineStatus),
      connectionGeneration: 1,
    });
    let sceneAttempts = 0;
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand')
      .mockImplementation(async (command) => {
        if (command.type === 'set-scene' && sceneAttempts++ === 0) {
          throw new Error(
            'ENGINE_COMMAND_OUTCOME_UNKNOWN commandId=7: connexion interrompue',
          );
        }
      });

    render(<App />);

    await waitFor(() => expect(
      sendCommand.mock.calls.filter(([command]) => command.type === 'set-scene'),
    ).toHaveLength(2));
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({
      version: 1,
      type: 'set-audio-route',
      captureProvider: 'external-render',
      captureEndpointId: external.id,
      outputDeviceId: physical.id,
    }));
    expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' });
  });

  it('n’envoie qu’une commande lors d’un changement du mode fenêtres', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    sendCommand.mockClear();

    act(() => useAppStore.getState().replaceWindowSpatialization({
      ...structuredClone(defaultWindowSpatialization),
      enabled: true,
    }));

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith(
      expect.objectContaining({ type: 'set-window-spatialization' }),
    ));
    expect(sendCommand.mock.calls.filter(([command]) =>
      command.type === 'set-window-spatialization')).toHaveLength(1);
    expect(sendCommand).not.toHaveBeenCalledWith(
      expect.objectContaining({ type: 'set-scene' }),
    );
  });

  it('applique la scène stéréo avant le mode fenêtres lors d’une transition 5.1 combinée', async () => {
    const config = externalConfig();
    config.scene.captureEndpointId = externalSurround.id;
    config.scene.inputLayout = '5.1-surround';
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(config);
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([externalSurround, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    sendCommand.mockClear();

    act(() => {
      const state = useAppStore.getState();
      state.replaceScene({ ...state.scene, inputLayout: 'stereo' });
      state.replaceWindowSpatialization({
        ...state.windowSpatialization,
        enabled: true,
      });
    });

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith(
      expect.objectContaining({ type: 'set-window-spatialization' }),
    ));
    const commands = sendCommand.mock.calls.map(([command]) => command.type);
    expect(commands).toEqual(['set-scene', 'set-window-spatialization']);
  });

  it('désactive le mode fenêtres avant d’appliquer une scène 5.1 combinée', async () => {
    const config = externalConfig();
    config.scene.captureEndpointId = externalSurround.id;
    config.windowSpatialization.enabled = true;
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(config);
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([externalSurround, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    sendCommand.mockClear();

    act(() => {
      const state = useAppStore.getState();
      state.replaceWindowSpatialization({
        ...state.windowSpatialization,
        enabled: false,
      });
      state.replaceScene({ ...state.scene, inputLayout: '5.1-surround' });
    });

    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith(
      expect.objectContaining({ type: 'set-scene' }),
    ));
    expect(sendCommand.mock.calls.map(([command]) => command.type)).toEqual([
      'set-window-spatialization',
      'set-scene',
    ]);
  });

  it('restaure le mode précédent quand le moteur refuse la spatialisation des fenêtres', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();

    render(<App />);
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    sendCommand.mockImplementation(async (command) => {
      if (command.type === 'set-window-spatialization' && command.config.enabled)
        throw new Error('window spatialization requires the stereo input layout');
    });

    act(() => useAppStore.getState().replaceWindowSpatialization({
      ...structuredClone(defaultWindowSpatialization),
      enabled: true,
    }));

    await waitFor(() => expect(useAppStore.getState().windowSpatialization.enabled).toBe(false));
    expect(useAppStore.getState().toasts.at(-1)).toMatchObject({
      tone: 'warning',
      title: 'Spatialisation des fenêtres non appliquée',
    });
  });

  it('ne restaure pas une commande dont le résultat reste indéterminé après timeout', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    const saveConfig = vi.mocked(desktopBridge.saveConfig);

    render(<App />);
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    sendCommand.mockImplementation(async (command) => {
      if (command.type === 'set-window-spatialization' && command.config.enabled) {
        throw new Error(
          'ENGINE_COMMAND_OUTCOME_UNKNOWN commandId=44: confirmation tardive',
        );
      }
    });

    act(() => useAppStore.getState().replaceWindowSpatialization({
      ...structuredClone(defaultWindowSpatialization),
      enabled: true,
    }));

    await waitFor(() => expect(useAppStore.getState().toasts.at(-1)).toMatchObject({
      title: 'Confirmation moteur différée',
    }));
    expect(useAppStore.getState().windowSpatialization.enabled).toBe(true);
    expect(saveConfig).toHaveBeenCalledWith(expect.objectContaining({
      windowSpatialization: expect.objectContaining({ enabled: true }),
    }));
  });

  it('affiche l’avertissement de persistance remonté par le bridge réel', async () => {
    vi.spyOn(desktopBridge, 'loadConfig').mockResolvedValue(externalConfig());
    vi.spyOn(desktopBridge, 'listAudioDevices').mockResolvedValue([external, physical]);
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    let emitWarning: ((detail: string) => void) | undefined;
    vi.spyOn(desktopBridge, 'subscribeEngineCommandWarnings')
      .mockImplementation((listener) => {
        emitWarning = listener;
        return () => undefined;
      });

    render(<App />);
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({ version: 1, type: 'start' }));
    act(() => emitWarning?.(
      'ENGINE_COMMAND_APPLIED_NOT_PERSISTED commandId=45: disque plein',
    ));

    await waitFor(() => expect(useAppStore.getState().toasts.at(-1)).toMatchObject({
      title: 'Configuration appliquée, persistance moteur en échec',
    }));
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
      connectionGeneration: 1,
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
