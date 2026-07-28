import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  defaultScene,
  defaultWindowSpatialization,
  emptyEngineStatus,
  emptyTrackingMetrics,
} from '../data/defaults';
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
    windowSpatialization: structuredClone(defaultWindowSpatialization),
    engine: structuredClone(emptyEngineStatus),
    tracking: structuredClone(emptyTrackingMetrics),
    audioDevices: [surroundCapture],
    toasts: [],
  });
};

const enableWindowRuntime = () => {
  useAppStore.getState().replaceWindowSpatialization({
    ...structuredClone(defaultWindowSpatialization),
    enabled: true,
  });
  useAppStore.getState().setEngine({
    ...structuredClone(emptyEngineStatus),
    spatialInputMode: 'process-windows',
    requestedSpatialInputMode: 'process-windows',
    windowAudio: {
      supported: true,
      running: true,
      sourceCount: 1,
      fifoOverruns: 0,
      fifoUnderruns: 0,
      lastError: null,
      displays: [{
        displayId: 'display-right',
        name: 'Écran droit',
        isPrimary: false,
        boundsPx: { x: 1920, y: 0, width: 2560, height: 1440 },
        rotationDegrees: 0,
        center: { x: 0.75, y: 1.2, z: 0.9 },
        widthM: 0.62,
        heightM: 0.35,
        orientation: { x: 0, y: 0, z: 0, w: 1 },
        calibrated: false,
      }],
      windowSources: [{
        sourceId: 'source-player',
        applicationId: 'player.exe',
        applicationName: 'Lecteur',
        windowTitle: 'Musique',
        processId: 4242,
        displayId: 'display-right',
        active: true,
        leftPosition: { x: 0.56, y: 1.2, z: 0.9 },
        rightPosition: { x: 0.94, y: 1.2, z: 0.9 },
        gainDb: 0,
        sampleRate: 48_000,
        channelCount: 2,
        captureState: 'capturing',
      }],
    },
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

  it('bascule vers la spatialisation stéréo par fenêtres', () => {
    render(<ScenePage />);
    fireEvent.click(screen.getByRole('radio', { name: 'Fenêtres' }));

    expect(useAppStore.getState().windowSpatialization.enabled).toBe(true);
    expect(screen.getByRole('radiogroup', { name: 'Panneau de spatialisation des fenêtres' })).toBeInTheDocument();
    expect(screen.getByText('Aucun écran reçu')).toBeInTheDocument();
    expect(screen.queryByRole('radiogroup', { name: 'Format des canaux d’entrée' })).not.toBeInTheDocument();
    expect(screen.queryByRole('slider', { name: 'Balance son direct et pièce' })).not.toBeInTheDocument();
  });

  it('refuse le mode fenêtres tant que l’entrée 5.1 n’est pas repassée en stéréo', () => {
    useAppStore.getState().patchScene({ inputLayout: '5.1-surround' });
    render(<ScenePage />);

    fireEvent.click(screen.getByRole('radio', { name: 'Fenêtres' }));

    expect(useAppStore.getState().windowSpatialization.enabled).toBe(false);
    expect(useAppStore.getState().toasts.at(-1)).toMatchObject({
      tone: 'warning',
      title: 'Mode fenêtres stéréo',
    });
  });

  it('calibre un écran Windows et conserve une paire stéréo par source', () => {
    enableWindowRuntime();
    render(<ScenePage />);

    expect(screen.getByText('Écran droit')).toBeInTheDocument();
    fireEvent.change(screen.getByRole('slider', { name: 'Décalage horizontal' }), {
      target: { value: '1.2' },
    });
    expect(useAppStore.getState().windowSpatialization.displayCalibrations[0]).toMatchObject({
      displayId: 'display-right',
      center: { x: 1.2, y: 1.2, z: 0.9 },
    });
    fireEvent.change(screen.getByRole('slider', { name: 'Angle horizontal' }), {
      target: { value: '-30' },
    });
    const orientation = useAppStore.getState().windowSpatialization.displayCalibrations[0].orientation;
    expect(orientation.y).toBeCloseTo(-0.258819, 5);
    expect(orientation.w).toBeCloseTo(0.965926, 5);

    fireEvent.click(screen.getByRole('radio', { name: 'Sources' }));
    expect(screen.getByText('Lecteur')).toBeInTheDocument();
    expect(screen.getByText('Musique · L/R')).toBeInTheDocument();
    fireEvent.change(screen.getByRole('slider', { name: 'Écartement L/R' }), {
      target: { value: '0.35' },
    });
    expect(useAppStore.getState().windowSpatialization.sourceRules[0]).toMatchObject({
      applicationId: 'player.exe',
      enabled: true,
      stereoSpread: 0.35,
    });
  });

  it('place L/R sur les bords de fenêtre avec un mode explicite', () => {
    enableWindowRuntime();
    render(<ScenePage />);

    fireEvent.click(screen.getByRole('radio', { name: 'Bords de fenêtre' }));

    expect(useAppStore.getState().windowSpatialization.emitterPlacementMode).toBe('window-edges');
    expect(screen.queryByRole('slider', { name: 'Largeur stéréo par défaut' })).not.toBeInTheDocument();
    expect(screen.getByText(/Chaque bord est projeté indépendamment/)).toBeInTheDocument();
  });

  it('signale le repli global qui protège une source non séparée', () => {
    enableWindowRuntime();
    useAppStore.getState().setEngine({
      ...structuredClone(useAppStore.getState().engine),
      spatialInputMode: 'endpoint-mix',
      requestedSpatialInputMode: 'process-windows',
    });

    render(<ScenePage />);

    expect(screen.getByText('Mix global de sécurité')).toBeInTheDocument();
    expect(screen.getByRole('status')).toHaveTextContent(
      'le mix global reste audible afin de ne couper aucune source',
    );
  });

  it('conserve une application désactivée afin de pouvoir la réactiver', () => {
    useAppStore.getState().replaceWindowSpatialization({
      ...structuredClone(defaultWindowSpatialization),
      enabled: true,
      sourceRules: [{
        applicationId: 'player.exe',
        enabled: false,
        gainDb: -2,
        stereoSpread: 0.5,
        fallbackDisplayId: null,
      }],
    });
    useAppStore.getState().setEngine({
      ...structuredClone(emptyEngineStatus),
      spatialInputMode: 'process-windows',
      requestedSpatialInputMode: 'process-windows',
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
    });
    render(<ScenePage />);

    expect(screen.getByText('En attente d’une application audio')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('radio', { name: 'Sources' }));

    expect(screen.getAllByText('player.exe')).not.toHaveLength(0);
    expect(screen.getByText('Désactivée · règle mémorisée')).toBeInTheDocument();
    const applicationToggle = screen.getByRole('checkbox', {
      name: /Spatialiser cette session audio/,
    });
    expect(applicationToggle).not.toBeChecked();

    fireEvent.click(applicationToggle);

    expect(useAppStore.getState().windowSpatialization.sourceRules).toContainEqual({
      applicationId: 'player.exe',
      enabled: true,
      gainDb: -2,
      stereoSpread: 0.5,
      fallbackDisplayId: null,
    });
  });

  it('supprime explicitement une règle de source mémorisée', () => {
    useAppStore.getState().replaceWindowSpatialization({
      ...structuredClone(defaultWindowSpatialization),
      enabled: true,
      sourceRules: [{
        applicationId: 'player.exe',
        enabled: false,
        gainDb: -2,
        stereoSpread: 0.5,
        fallbackDisplayId: null,
      }],
    });
    render(<ScenePage />);
    fireEvent.click(screen.getByRole('radio', { name: 'Sources' }));
    fireEvent.click(screen.getByRole('button', { name: 'Supprimer la règle' }));

    expect(useAppStore.getState().windowSpatialization.sourceRules).toEqual([]);
    expect(screen.getByText('Aucune source audio')).toBeInTheDocument();
  });

  it('refuse explicitement une 65e règle au lieu de la tronquer silencieusement', () => {
    enableWindowRuntime();
    useAppStore.getState().replaceWindowSpatialization({
      ...useAppStore.getState().windowSpatialization,
      sourceRules: Array.from({ length: 64 }, (_, index) => ({
        applicationId: `application-${index}.exe`,
        enabled: true,
        gainDb: 0,
        stereoSpread: 1,
        fallbackDisplayId: null,
      })),
    });
    render(<ScenePage />);
    fireEvent.click(screen.getByRole('radio', { name: 'Sources' }));
    fireEvent.change(screen.getByRole('slider', { name: 'Écartement L/R' }), {
      target: { value: '0.4' },
    });

    expect(useAppStore.getState().windowSpatialization.sourceRules).toHaveLength(64);
    expect(useAppStore.getState().toasts.at(-1)).toMatchObject({
      tone: 'warning',
      title: 'Limite de règles atteinte',
    });
  });
});
