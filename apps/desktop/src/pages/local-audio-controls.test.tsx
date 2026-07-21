import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { defaultScene, emptyEngineStatus } from '../data/defaults';
import { desktopBridge } from '../lib/tauri-bridge';
import { useAppStore } from '../store/app-store';
import { DiagnosticsPage } from './DiagnosticsPage';
import { ProfilesPage, waitForAudioModeOutcome } from './ProfilesPage';

vi.mock('../hooks/useHrtfAvailability', () => ({
  useHrtfAvailability: () => ({
    'sadie-d2-kemar': true,
    'sadie-h6': true,
    'sadie-h9': true,
    'sadie-h10': true,
    'sadie-h19': true,
    'sadie-h20': true,
  }),
}));

describe('réglages audio locaux', () => {
  beforeEach(() => {
    useAppStore.getState().replaceScene(structuredClone(defaultScene));
    useAppStore.getState().setEngine(structuredClone(emptyEngineStatus));
    useAppStore.setState({ previewMode: false, audioRouteReady: true, audioRouteIssue: null });
  });

  afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
    Reflect.deleteProperty(window, '__TAURI_INTERNALS__');
  });

  it('expose les trois modes et ne persiste le choix qu’après confirmation effective', async () => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { configurable: true, value: {} });
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    vi.spyOn(desktopBridge, 'getEngineStatus').mockResolvedValue({
      ...structuredClone(emptyEngineStatus),
      audioMode: 'exclusive-pro',
      connection: 'ready',
      captureActive: true,
      renderActive: true,
    });
    render(<ProfilesPage />);

    expect(screen.getByRole('radio', { name: 'Faible latence' })).toHaveAttribute('aria-checked', 'true');
    expect(screen.getByRole('radio', { name: 'Compatibilité 256' })).toBeInTheDocument();
    fireEvent.click(screen.getByRole('radio', { name: 'Pro exclusif' }));

    expect(useAppStore.getState().scene.audioMode).toBe('shared-low-latency');
    expect(screen.getByRole('status')).toHaveTextContent('Reconfiguration de WASAPI en cours');
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({
      version: 1,
      type: 'set-audio-mode',
      mode: 'exclusive-pro',
    }));
    await waitFor(() => expect(useAppStore.getState().scene.audioMode).toBe('exclusive-pro'));
  });

  it('réconcilie explicitement le fallback Pro vers le mode partagé', async () => {
    const transientExclusiveFailure = {
      ...structuredClone(emptyEngineStatus),
      audioMode: 'exclusive-pro' as const,
      connection: 'error' as const,
      lastError: 'the physical endpoint does not support exclusive stereo float32/48 kHz',
    };
    const fallback = {
      ...structuredClone(emptyEngineStatus),
      audioMode: 'shared-low-latency' as const,
      connection: 'degraded' as const,
      captureActive: true,
      renderActive: true,
      lastError: 'MMCSS unavailable; AUDIO_MODE_FALLBACK requested=exclusive-pro effective=shared-low-latency reason=format refused',
    };
    const readStatus = vi.fn()
      .mockResolvedValueOnce(transientExclusiveFailure)
      .mockResolvedValueOnce(fallback);
    await expect(waitForAudioModeOutcome('exclusive-pro', readStatus, 0, 2)).resolves.toMatchObject({
      kind: 'fallback',
      status: { audioMode: 'shared-low-latency' },
    });
    expect(readStatus).toHaveBeenCalledTimes(2);
  });

  it('expose et persiste le gain maître dans la scène', () => {
    render(<ProfilesPage />);
    fireEvent.change(screen.getByRole('slider', { name: 'Gain maître' }), { target: { value: '-3.5' } });
    expect(useAppStore.getState().scene.masterGainDb).toBe(-3.5);
  });

  it('prévisualise une EQ sans l’activer puis attend une confirmation explicite', async () => {
    vi.spyOn(desktopBridge, 'importHeadphoneEq').mockResolvedValue({
      fileName: 'reference.txt',
      format: 'equalizer-apo',
      profileName: 'Casque de référence',
      preampDb: -6.5,
      bands: [
        { id: 'import-apo-1', enabled: true, type: 'peak', frequencyHz: 2_800, gainDb: -2.5, q: 1.1 },
      ],
    });
    const sendCommand = vi.spyOn(desktopBridge, 'sendCommand').mockResolvedValue();
    render(<ProfilesPage />);

    fireEvent.click(screen.getByRole('button', { name: 'Importer une EQ' }));
    expect(await screen.findByLabelText('Prévisualisation de l’égalisation importée')).toBeInTheDocument();
    expect(screen.getByText('Casque de référence')).toBeInTheDocument();
    expect(useAppStore.getState().scene.headphoneEq.enabled).toBe(false);
    expect(sendCommand).not.toHaveBeenCalled();

    fireEvent.click(screen.getByRole('button', { name: 'Appliquer et activer' }));
    await waitFor(() => expect(sendCommand).toHaveBeenCalledWith({
      version: 1,
      type: 'set-headphone-eq',
      eq: {
        enabled: true,
        profileName: 'Casque de référence',
        preampDb: -6.5,
        bands: [{ id: 'import-apo-1', enabled: true, type: 'peak', frequencyHz: 2_800, gainDb: -2.5, q: 1.1 }],
      },
    }));
    expect(useAppStore.getState().scene.headphoneEq.enabled).toBe(true);
  });

  it('affiche les erreurs de lecture ou de parsing EQ', async () => {
    vi.spyOn(desktopBridge, 'importHeadphoneEq').mockRejectedValue(new Error('Ligne 2 : directive non prise en charge'));
    render(<ProfilesPage />);
    fireEvent.click(screen.getByRole('button', { name: 'Importer une EQ' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Ligne 2 : directive non prise en charge');
    expect(useAppStore.getState().scene.headphoneEq.enabled).toBe(false);
  });
});

describe('libellés de diagnostic', () => {
  beforeEach(() => {
    useAppStore.getState().setEngine({
      ...structuredClone(emptyEngineStatus),
      connection: 'ready',
      callbackCpuPercent: 24,
      motionToSoundLatencyMs: { p50: 12, p95: 18 },
      audioPipelineLatencyMs: 5.34,
    });
    useAppStore.getState().patchTracking({ state: 'running', fps: 60 });
    useAppStore.setState({ previewMode: false, audioRouteReady: true, audioRouteIssue: null });
  });

  afterEach(cleanup);

  it('distingue les estimations logicielles des mesures physiques et de la charge p99', () => {
    render(<DiagnosticsPage />);
    expect(screen.getByText('ÉTAT LOGICIEL — NON QUALIFICATIF')).toBeInTheDocument();
    expect(screen.getByText('Mouvement → PCM (p95 estimé)')).toBeInTheDocument();
    expect(screen.getByText('Charge callback instantanée')).toBeInTheDocument();
    expect(screen.queryByText('Charge callback p99')).not.toBeInTheDocument();
  });
});
