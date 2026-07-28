import { cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { defaultScene, emptyEngineStatus, emptyTrackingMetrics } from '../data/defaults';
import { useAppStore } from '../store/app-store';
import type { AudioDeviceSummary } from '../types/contracts';
import { DiagnosticsPage } from './DiagnosticsPage';

const surroundCapture: AudioDeviceSummary = {
  id: 'capture-5.1',
  name: 'Capture HDMI 5.1',
  isDefault: false,
  isSoundSpatializerEndpoint: false,
  transport: 'hdmi',
  sampleRate: 48_000,
  channelCount: 6,
  channelMask: 0x60f,
};

describe('diagnostic du format multicanal', () => {
  afterEach(cleanup);

  it('affiche le layout, le nombre de canaux, le masque et la matrice HRTF 5×2', () => {
    const scene = structuredClone(defaultScene);
    scene.captureProvider = 'external-render';
    scene.captureEndpointId = surroundCapture.id;
    scene.inputLayout = '5.1-surround';
    useAppStore.setState({
      scene,
      engine: {
        ...structuredClone(emptyEngineStatus),
        inputLayout: '5.1-surround',
        captureChannels: 6,
        captureChannelMask: 0x60f,
        connection: 'ready',
        captureActive: true,
      },
      tracking: structuredClone(emptyTrackingMetrics),
      audioDevices: [surroundCapture],
      previewMode: false,
      audioRouteIssue: null,
    });

    render(<DiagnosticsPage />);

    expect(screen.getByText(/Capture HDMI 5\.1 · 6 canaux · masque 0x60F/)).toBeInTheDocument();
    expect(screen.getByText('Matrice dynamique 5 × 2')).toBeInTheDocument();
    expect(screen.getByText('Float32 · 5.1 surround · 48 kHz')).toBeInTheDocument();
    expect(screen.getByText('6 canaux · masque 0x60F')).toBeInTheDocument();
  });

  it('affiche les paires L/R du mode fenêtres', () => {
    useAppStore.setState({
      scene: structuredClone(defaultScene),
      engine: {
        ...structuredClone(emptyEngineStatus),
        spatialInputMode: 'process-windows',
        requestedSpatialInputMode: 'process-windows',
        connection: 'ready',
        captureActive: true,
        windowAudio: {
          supported: true,
          running: true,
          sourceCount: 2,
          fifoOverruns: 0,
          fifoUnderruns: 0,
          lastError: null,
          displays: [{
            displayId: 'main',
            name: 'Principal',
            isPrimary: true,
            boundsPx: { x: 0, y: 0, width: 1920, height: 1080 },
            rotationDegrees: 0,
            center: { x: 0, y: 1.2, z: 0.9 },
            widthM: 0.6,
            heightM: 0.34,
            orientation: { x: 0, y: 0, z: 0, w: 1 },
            calibrated: false,
          }],
          windowSources: [],
        },
      },
      tracking: structuredClone(emptyTrackingMetrics),
      audioDevices: [],
      previewMode: false,
      audioRouteIssue: null,
    });

    render(<DiagnosticsPage />);

    expect(screen.getByText('2 session(s)/arbre(s) de processus · 1 écran(s)')).toBeInTheDocument();
    expect(screen.getByText('Matrice dynamique 4 × 2')).toBeInTheDocument();
    expect(screen.getByText('Fenêtres · paires L/R')).toBeInTheDocument();
    expect(screen.getAllByText('0 overrun(s) · 0 underrun(s)')).toHaveLength(2);
  });

  it('signale le repli endpoint tant que les captures applicatives ne sont pas prêtes', () => {
    useAppStore.setState({
      scene: structuredClone(defaultScene),
      engine: {
        ...structuredClone(emptyEngineStatus),
        spatialInputMode: 'endpoint-mix',
        requestedSpatialInputMode: 'process-windows',
        connection: 'ready',
        captureActive: true,
        renderActive: true,
        windowAudio: {
          supported: true,
          running: true,
          sourceCount: 0,
          fifoOverruns: 0,
          fifoUnderruns: 0,
          lastError: null,
          displays: [],
          windowSources: [],
        },
      },
      tracking: structuredClone(emptyTrackingMetrics),
      audioDevices: [],
      previewMode: false,
      audioRouteIssue: null,
    });

    render(<DiagnosticsPage />);

    expect(screen.getByText('Repli de sécurité sur le mix endpoint')).toBeInTheDocument();
    expect(screen.getByText('Fenêtres demandées · repli endpoint')).toBeInTheDocument();
    expect(screen.getByText(/Couverture par session incomplète ou en cours/)).toBeInTheDocument();
    expect(screen.getByText('Matrice dynamique 2 × 2')).toBeInTheDocument();
  });

  it('affiche et signale les xruns FIFO des captures process-loopback', () => {
    useAppStore.setState({
      scene: structuredClone(defaultScene),
      engine: {
        ...structuredClone(emptyEngineStatus),
        spatialInputMode: 'process-windows',
        requestedSpatialInputMode: 'process-windows',
        connection: 'ready',
        captureActive: true,
        renderActive: true,
        trackingActive: true,
        motionToSoundLatencyMs: { p50: 10, p95: 15 },
        windowAudio: {
          supported: true,
          running: true,
          sourceCount: 1,
          fifoOverruns: 3,
          fifoUnderruns: 5,
          lastError: null,
          displays: [],
          windowSources: [],
        },
      },
      tracking: {
        ...structuredClone(emptyTrackingMetrics),
        state: 'running',
        fps: 60,
      },
      audioDevices: [],
      previewMode: false,
      audioRouteIssue: null,
    });

    render(<DiagnosticsPage />);

    expect(screen.getAllByText('3 overrun(s) · 5 underrun(s)')).toHaveLength(2);
    expect(screen.getByText(/8 interruption\(s\) FIFO des captures par processus/)).toBeInTheDocument();
  });
});
