import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { defaultScene, emptyTrackingMetrics } from '../data/defaults';
import { useAppStore } from '../store/app-store';
import type { AudioDeviceSummary } from '../types/contracts';
import { ScenePage } from './ScenePage';

vi.mock('../components/scene/SpatialScene', () => ({
  SpatialScene: () => <div data-testid="spatial-scene" />,
}));

vi.mock('../tracking/TrackingProvider', () => ({
  useTrackingController: () => ({
    start: vi.fn().mockResolvedValue(undefined),
    calibrate: vi.fn().mockResolvedValue(true),
    running: false,
  }),
}));

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

const resetScene = () => {
  const scene = structuredClone(defaultScene);
  scene.captureProvider = 'external-render';
  scene.captureEndpointId = surroundCapture.id;
  useAppStore.setState({
    scene,
    tracking: structuredClone(emptyTrackingMetrics),
    audioDevices: [surroundCapture],
    toasts: [],
  });
};

describe('éditeur d’implantation 5.1', () => {
  beforeEach(resetScene);
  afterEach(cleanup);

  it('affiche toujours cinq enceintes et atténue C/LS/RS en stéréo', () => {
    const { container } = render(<ScenePage />);
    const speakers = [...container.querySelectorAll('.speaker-selector button')];
    expect(speakers).toHaveLength(5);
    expect(speakers.map((button) => button.querySelector('strong')?.textContent)).toEqual(['L', 'R', 'C', 'LS', 'RS']);
    expect(speakers.filter((button) => button.classList.contains('is-unrouted'))).toHaveLength(3);
    expect(screen.queryByText('LFE · EFFETS BASSE FRÉQUENCE')).not.toBeInTheDocument();
  });

  it('active le 5.1 uniquement avec la capture externe exacte et expose le LFE', () => {
    const { container } = render(<ScenePage />);
    fireEvent.click(screen.getByRole('radio', { name: '5.1' }));
    expect(useAppStore.getState().scene.inputLayout).toBe('5.1-surround');
    expect(container.querySelectorAll('.speaker-selector button.is-unrouted')).toHaveLength(0);
    expect(screen.getByText('LFE · EFFETS BASSE FRÉQUENCE')).toBeInTheDocument();
    expect(screen.getByRole('checkbox', { name: 'Canal LFE actif' })).toBeChecked();
  });

  it('désactive le 5.1 sans source compatible et explique le masque attendu', () => {
    useAppStore.getState().patchScene({ captureProvider: 'native-driver', captureEndpointId: null });
    render(<ScenePage />);
    expect(screen.getByRole('radio', { name: '5.1' })).toBeDisabled();
    expect(screen.getByRole('status')).toHaveTextContent('0x3F ou 0x60F');
    expect(screen.getByRole('status')).toHaveTextContent('aucun upmix');
  });

  it('lie symétriquement LS et RS mais désactive le lien pour C', () => {
    const { container } = render(<ScenePage />);
    const buttons = [...container.querySelectorAll<HTMLButtonElement>('.speaker-selector button')];
    fireEvent.click(buttons.find((button) => button.querySelector('strong')?.textContent === 'LS')!);
    fireEvent.change(screen.getByRole('slider', { name: 'Azimut' }), { target: { value: '-120' } });
    const [left, right] = ['LS', 'RS'].map((channel) =>
      useAppStore.getState().scene.speakers.find((speaker) => speaker.channel === channel)!,
    );
    expect(right.position.x).toBeCloseTo(-left.position.x, 6);
    expect(right.position.z).toBeCloseTo(left.position.z, 6);

    fireEvent.click(buttons.find((button) => button.querySelector('strong')?.textContent === 'C')!);
    expect(screen.getByTitle('Le canal central n’a pas de paire')).toBeDisabled();
  });
});
