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
});
