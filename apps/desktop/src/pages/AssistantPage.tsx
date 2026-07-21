import { useEffect, useMemo, useRef, useState } from 'react';
import {
  ArrowLeft,
  ArrowRight,
  AudioLines,
  Camera,
  Check,
  CheckCircle2,
  Crosshair,
  Gauge,
  Headphones,
  Laptop,
  MonitorCog,
  Radio,
  RefreshCw,
  Rotate3D,
  Settings2,
  ShieldCheck,
  Sparkles,
  Usb,
  Webcam,
  WifiOff,
} from 'lucide-react';
import { StepMarker } from '../components/ui/Controls';
import { deriveRuntimeCapabilities, routeSelectionFromScene, sameAudioEndpoint, supportsSurround5_1, type AudioRouteIssue, type RuntimeCapabilities } from '../lib/runtime-capabilities';
import { desktopBridge, isTauriRuntime } from '../lib/tauri-bridge';
import { useAppStore } from '../store/app-store';
import { useTrackingController } from '../tracking/TrackingProvider';
import type { AudioDeviceSummary, CaptureProvider, EngineCommandV1, SceneConfigV2 } from '../types/contracts';

const STEPS = [
  { label: 'Bienvenue', short: 'Préparation' },
  { label: 'Sortie audio', short: 'Votre casque' },
  { label: 'Suivi facial', short: 'Caméra locale' },
  { label: 'Calibration', short: 'Point de référence' },
] as const;

const routeIssueTitle = (issue: AudioRouteIssue | null): string => {
  if (issue === 'native-endpoint-unavailable') return 'Pilote natif indisponible';
  if (issue === 'capture-endpoint-required' || issue === 'capture-endpoint-unavailable') return 'Source audio à sélectionner';
  if (issue === 'capture-layout-unsupported') return 'Source 5.1 incompatible';
  if (issue === 'output-endpoint-required' || issue === 'output-endpoint-unavailable') return 'Casque à sélectionner';
  if (issue === 'capture-equals-output') return 'Boucle audio bloquée';
  if (issue === 'desktop-runtime-required') return 'Aperçu sans capture système';
  return issue ? 'Routage audio non valide' : 'Routage audio prêt';
};

const routeIssueDetail = (issue: AudioRouteIssue | null): string => {
  switch (issue) {
    case 'desktop-runtime-required': return 'La capture WASAPI est disponible uniquement dans l’application de bureau.';
    case 'native-endpoint-unavailable': return 'Le pilote Sound Spatializer n’est pas actif. Vous pouvez choisir manuellement un endpoint de rendu externe.';
    case 'capture-endpoint-required': return 'Choisissez l’endpoint de rendu fourni par votre câble audio virtuel.';
    case 'capture-endpoint-unavailable': return 'La source externe enregistrée a disparu. Rebranchez-la ou sélectionnez-en une autre.';
    case 'capture-endpoint-is-native': return 'Le mode externe ne peut pas cibler l’endpoint du pilote Sound Spatializer.';
    case 'capture-layout-unsupported': return 'Le 5.1 requiert une source externe de six canaux exactement, avec un masque 0x3F ou 0x60F.';
    case 'output-endpoint-required': return 'Choisissez le casque physique qui recevra le son spatialisé.';
    case 'output-endpoint-unavailable': return 'Le casque enregistré n’est plus actif. Rebranchez-le ou sélectionnez-en un autre.';
    case 'output-endpoint-is-native': return 'Sound Spatializer est une source de capture et ne peut jamais être la sortie du moteur.';
    case 'capture-equals-output': return 'La source et la sortie doivent être deux endpoints différents.';
    default: return 'La source système et le casque sont correctement séparés.';
  }
};

const audioRouteCommand = (scene: SceneConfigV2): EngineCommandV1 => {
  if (!scene.physicalOutputDeviceId) throw new Error('Aucune sortie physique n’est sélectionnée.');
  return {
    version: 1,
    type: 'set-audio-route',
    captureProvider: scene.captureProvider,
    captureEndpointId: scene.captureProvider === 'external-render' ? scene.captureEndpointId : null,
    outputDeviceId: scene.physicalOutputDeviceId,
  };
};

const canContinueAudioStep = (
  scene: SceneConfigV2,
  capabilities: RuntimeCapabilities,
): boolean => capabilities.audioRouteReady || (
  scene.captureProvider === 'native-driver'
  && Boolean(capabilities.outputEndpoint)
  && (capabilities.routeIssue === 'native-endpoint-unavailable'
    || capabilities.routeIssue === 'desktop-runtime-required')
);

export function AssistantPage() {
  const preferences = useAppStore((state) => state.preferences);
  const patchPreferences = useAppStore((state) => state.patchPreferences);
  const patchScene = useAppStore((state) => state.patchScene);
  const setActiveView = useAppStore((state) => state.setActiveView);
  const scene = useAppStore((state) => state.scene);
  const devices = useAppStore((state) => state.audioDevices);
  const setAudioDevices = useAppStore((state) => state.setAudioDevices);
  const tracking = useAppStore((state) => state.tracking);
  const notify = useAppStore((state) => state.notify);
  const { start, stop, calibrate, running, videoElement } = useTrackingController();
  const [refreshingDevices, setRefreshingDevices] = useState(false);
  const [routeApplying, setRouteApplying] = useState(false);
  const routeApplyInFlight = useRef(false);
  const step = Math.max(0, Math.min(STEPS.length - 1, preferences.onboardingStep));
  const capabilities = useMemo(
    () => deriveRuntimeCapabilities(isTauriRuntime(), devices, routeSelectionFromScene(scene)),
    [devices, scene.captureEndpointId, scene.captureProvider, scene.inputLayout, scene.physicalOutputDeviceId],
  );
  const selectedOutput = capabilities.outputEndpoint;
  const selectedCapture = scene.captureProvider === 'external-render' ? capabilities.captureEndpoint : null;

  const goTo = (next: number) => patchPreferences({ onboardingStep: Math.max(0, Math.min(3, next)) });
  const finish = async () => {
    if (routeApplyInFlight.current) return;
    try {
      if (running && !(await calibrate())) return;
      const latestScene = useAppStore.getState().scene;
      const latestCapabilities = deriveRuntimeCapabilities(
        isTauriRuntime(),
        useAppStore.getState().audioDevices,
        routeSelectionFromScene(latestScene),
      );
      if (!latestCapabilities.audioRouteReady) {
        const previewAllowed = latestScene.captureProvider === 'native-driver'
          && (latestCapabilities.routeIssue === 'native-endpoint-unavailable'
            || latestCapabilities.routeIssue === 'desktop-runtime-required');
        if (!previewAllowed) {
          notify({
            tone: 'warning',
            title: 'Routage audio incomplet',
            detail: routeIssueDetail(latestCapabilities.routeIssue),
          });
          return;
        }
        patchPreferences({ onboardingComplete: true, onboardingStep: 3 });
        setActiveView('scene');
        notify({
          tone: 'info',
          title: 'Aperçu sans audio système',
          detail: 'Vous pourrez revenir ici pour installer le pilote natif ou choisir explicitement un endpoint de rendu externe.',
        });
        return;
      }
      // La route ouvre WASAPI avec le mode de la scène moteur courante.
      // Synchronisez donc le mode/tampon avant toute (ré)ouverture.
      await desktopBridge.sendCommand({ version: 1, type: 'set-scene', scene: latestScene });
      await desktopBridge.sendCommand(audioRouteCommand(latestScene));
      await desktopBridge.sendCommand({ version: 1, type: 'start' });
      patchPreferences({ onboardingComplete: true, onboardingStep: 3 });
      setActiveView('scene');
      notify({
        tone: 'success',
        title: 'Votre espace est prêt',
        detail: latestScene.captureProvider === 'native-driver'
          ? 'Lancez un contenu stéréo et gardez Sound Spatializer sélectionné dans Windows.'
          : `Dans Windows, envoyez le son vers « ${latestCapabilities.captureEndpoint?.name ?? 'la source externe choisie'} » puis écoutez au casque.`,
      });
    } catch (error) {
      notify({ tone: 'error', title: 'Démarrage audio impossible', detail: error instanceof Error ? error.message : String(error) });
    }
  };

  const applyAudioRoute = async (patch: Partial<SceneConfigV2>) => {
    // Une route atomique doit partir d’un seul instantané. Sans ce verrou,
    // deux clics rapides peuvent chacun fusionner leur patch avec l’ancien
    // store et produire une source identique à la sortie.
    if (routeApplyInFlight.current) return;
    const nextScene = { ...useAppStore.getState().scene, ...patch };
    const nextCapabilities = deriveRuntimeCapabilities(
      isTauriRuntime(),
      useAppStore.getState().audioDevices,
      routeSelectionFromScene(nextScene),
    );
    if (!nextCapabilities.audioRouteReady) {
      patchScene(patch);
      return;
    }
    routeApplyInFlight.current = true;
    setRouteApplying(true);
    try {
      await desktopBridge.sendCommand({ version: 1, type: 'set-scene', scene: nextScene });
      await desktopBridge.sendCommand(audioRouteCommand(nextScene));
      patchScene(patch);
    } catch (error) {
      notify({ tone: 'error', title: 'Routage audio non appliqué', detail: error instanceof Error ? error.message : String(error) });
    } finally {
      routeApplyInFlight.current = false;
      setRouteApplying(false);
    }
  };

  const selectCaptureProvider = (captureProvider: CaptureProvider) => {
    void applyAudioRoute({
      captureProvider,
      captureEndpointId: null,
      ...(captureProvider === 'native-driver' ? { inputLayout: 'stereo' as const } : {}),
    });
  };

  const selectCaptureEndpoint = (device: AudioDeviceSummary) => {
    if (device.isSoundSpatializerEndpoint || sameAudioEndpoint(device.id, scene.physicalOutputDeviceId)) return;
    void applyAudioRoute({
      captureProvider: 'external-render',
      captureEndpointId: device.id,
      ...(scene.inputLayout === '5.1-surround' && !supportsSurround5_1(device) ? { inputLayout: 'stereo' as const } : {}),
    });
  };

  const selectOutputEndpoint = (device: AudioDeviceSummary) => {
    if (device.isSoundSpatializerEndpoint || (scene.captureProvider === 'external-render' && sameAudioEndpoint(device.id, scene.captureEndpointId))) return;
    void applyAudioRoute({ physicalOutputDeviceId: device.id });
  };

  const refreshAudioDevices = async () => {
    setRefreshingDevices(true);
    try {
      setAudioDevices(await desktopBridge.listAudioDevices());
    } catch (error) {
      notify({ tone: 'warning', title: 'Liste audio non actualisée', detail: error instanceof Error ? error.message : String(error) });
    } finally {
      setRefreshingDevices(false);
    }
  };

  const enableTracking = async () => {
    try {
      await start();
    } catch {
      // L'état contient déjà le diagnostic utilisateur.
    }
  };

  return (
    <div className="assistant-layout">
      <aside className="onboarding-progress panel">
        <div className="progress-intro">
          <span className="eyebrow">INSTALLATION</span>
          <h2>Quatre étapes, puis oubliez l’application.</h2>
          <p>Tout le traitement reste sur cette machine. Aucun flux caméra ou audio ne quitte votre PC.</p>
        </div>
        <ol>
          {STEPS.map((item, index) => {
            const status = index < step ? 'done' : index === step ? 'current' : 'future';
            return (
              <li key={item.label} className={`is-${status}`}>
                <StepMarker status={status} index={index} />
                <span><strong>{item.label}</strong><small>{item.short}</small></span>
              </li>
            );
          })}
        </ol>
        <div className="privacy-seal">
          <ShieldCheck size={20} />
          <span><strong>Confidentialité locale</strong><small>Pas de cloud · pas de télémétrie</small></span>
        </div>
      </aside>

      <section className="onboarding-card panel">
        <div className="onboarding-card-content">
          {capabilities.previewMode && (
            <div className="preview-driver-notice" role="status">
              <MonitorCog size={18} />
              <span><strong>{routeIssueTitle(capabilities.routeIssue)}</strong><small>{routeIssueDetail(capabilities.routeIssue)}</small></span>
            </div>
          )}
          {step === 0 && <WelcomeStep />}
          {step === 1 && (
            <AudioStep
              devices={devices}
              selectedCapture={selectedCapture}
              selectedOutput={selectedOutput}
              captureProvider={scene.captureProvider}
              driverEndpointAvailable={capabilities.driverEndpointAvailable}
              refreshing={refreshingDevices}
              routeApplying={routeApplying}
              onSelectProvider={selectCaptureProvider}
              onSelectCapture={selectCaptureEndpoint}
              onSelectOutput={selectOutputEndpoint}
              onRefresh={() => void refreshAudioDevices()}
              onOpenSettings={() => void desktopBridge.openSoundSettings()}
            />
          )}
          {step === 2 && (
            <TrackingStep
              running={running}
              tracking={tracking}
              videoElement={videoElement}
              onStart={enableTracking}
              onStop={stop}
            />
          )}
          {step === 3 && <CalibrationStep running={running} onStart={enableTracking} onCalibrate={calibrate} yaw={tracking.euler.yaw} pitch={tracking.euler.pitch} />}
        </div>
        <footer className="onboarding-actions">
          <button type="button" className="ghost-button" onClick={() => goTo(step - 1)} disabled={step === 0}>
            <ArrowLeft size={16} /> Retour
          </button>
          <span>Étape {step + 1} sur {STEPS.length}</span>
          {step < 3 ? (
            <button
              type="button"
              className="primary-button"
              onClick={() => goTo(step + 1)}
              disabled={routeApplying || (step === 1 && !canContinueAudioStep(scene, capabilities))}
            >
              Continuer <ArrowRight size={16} />
            </button>
          ) : (
            <button type="button" className="primary-button" onClick={() => void finish()} disabled={routeApplying}>
              Terminer <Check size={16} />
            </button>
          )}
        </footer>
      </section>
    </div>
  );
}

function WelcomeStep() {
  return (
    <div className="welcome-step">
      <div className="hero-orbit" aria-hidden="true">
        <span className="orbit orbit-one" />
        <span className="orbit orbit-two" />
        <span className="hero-head"><Headphones size={52} strokeWidth={1.25} /></span>
        <span className="hero-speaker hero-left">L</span>
        <span className="hero-speaker hero-right">R</span>
      </div>
      <span className="eyebrow accent">BIENVENUE DANS SOUND SPATIALIZER</span>
      <h2>Des enceintes devant vous.<br />Même quand votre tête bouge.</h2>
      <p className="lead">Nous allons relier votre casque, vérifier la caméra et définir votre orientation neutre. Comptez moins de deux minutes.</p>
      <div className="requirement-grid">
        <Requirement icon={<Headphones size={20} />} title="Casque filaire ou USB" detail="Recommandé pour une faible latence" status="recommended" />
        <Requirement icon={<Webcam size={20} />} title="Webcam 60 i/s" detail="30 i/s accepté en mode dégradé" status="recommended" />
        <Requirement icon={<Laptop size={20} />} title="Windows 11 x64" detail="Version 24H2 ou ultérieure" status="ready" />
      </div>
    </div>
  );
}

function Requirement({ icon, title, detail, status }: { icon: React.ReactNode; title: string; detail: string; status: 'ready' | 'recommended' }) {
  return (
    <article className="requirement-card">
      <span className="requirement-icon">{icon}</span>
      <span><strong>{title}</strong><small>{detail}</small></span>
      {status === 'ready' ? <CheckCircle2 size={17} className="success-icon" /> : <span className="recommended-label">CONSEILLÉ</span>}
    </article>
  );
}

function AudioStep({
  devices,
  selectedCapture,
  selectedOutput,
  captureProvider,
  driverEndpointAvailable,
  refreshing,
  routeApplying,
  onSelectProvider,
  onSelectCapture,
  onSelectOutput,
  onRefresh,
  onOpenSettings,
}: {
  devices: AudioDeviceSummary[];
  selectedCapture: AudioDeviceSummary | null;
  selectedOutput: AudioDeviceSummary | null;
  captureProvider: CaptureProvider;
  driverEndpointAvailable: boolean;
  refreshing: boolean;
  routeApplying: boolean;
  onSelectProvider: (provider: CaptureProvider) => void;
  onSelectCapture: (device: AudioDeviceSummary) => void;
  onSelectOutput: (device: AudioDeviceSummary) => void;
  onRefresh: () => void;
  onOpenSettings: () => void;
}) {
  const selectableEndpoints = devices.filter((device) => !device.isSoundSpatializerEndpoint);
  return (
    <div className="setup-step audio-route-step" aria-busy={routeApplying}>
      <div className="step-icon"><Headphones size={27} /></div>
      <span className="eyebrow accent">ROUTAGE AUDIO</span>
      <h2>D’où vient le son, et où doit-il arriver&nbsp;?</h2>
      <p className="lead">Choisissez explicitement une source système puis un casque différent. Aucun périphérique tiers n’est détecté ou sélectionné automatiquement.</p>

      <div className="audio-route-toolbar">
        <span><strong>1. Source du son système</strong><small>Endpoint de rendu capturé en loopback</small></span>
        <button type="button" className="secondary-button compact" onClick={onRefresh} disabled={refreshing || routeApplying}>
          <RefreshCw size={14} className={refreshing ? 'is-spinning' : ''} /> {refreshing ? 'Actualisation…' : 'Actualiser'}
        </button>
      </div>
      <fieldset className="capture-provider-grid" aria-label="Méthode de capture audio" disabled={routeApplying}>
        <label
          className={`capture-provider-card ${captureProvider === 'native-driver' ? 'is-selected' : ''}`}
        >
          <input
            type="radio"
            name="capture-provider"
            value="native-driver"
            checked={captureProvider === 'native-driver'}
            onChange={() => onSelectProvider('native-driver')}
          />
          <span className="device-icon"><Radio size={20} /></span>
          <span><strong>Pilote natif</strong><small>{driverEndpointAvailable ? 'Sound Spatializer détecté · recommandé' : 'Indisponible sur ce PC'}</small></span>
          <span className="selection-ring">{captureProvider === 'native-driver' && <Check size={14} />}</span>
        </label>
        <label
          className={`capture-provider-card ${captureProvider === 'external-render' ? 'is-selected' : ''}`}
        >
          <input
            type="radio"
            name="capture-provider"
            value="external-render"
            checked={captureProvider === 'external-render'}
            onChange={() => onSelectProvider('external-render')}
          />
          <span className="device-icon"><AudioLines size={20} /></span>
          <span><strong>Endpoint de rendu externe</strong><small>Mode compatibilité · sélection manuelle</small></span>
          <span className="selection-ring">{captureProvider === 'external-render' && <Check size={14} />}</span>
        </label>
      </fieldset>

      {captureProvider === 'external-render' && (
        <div className="audio-route-section">
          <span className="route-section-label">Endpoint externe à capturer</span>
          <div className="device-list compact-device-list">
            {selectableEndpoints.length === 0 ? (
              <div className="device-scan"><strong>Aucun endpoint de rendu externe actif.</strong></div>
            ) : selectableEndpoints.map((device) => (
              <AudioDeviceButton
                key={`capture-${device.id}`}
                device={device}
                selected={sameAudioEndpoint(selectedCapture?.id, device.id)}
                disabled={routeApplying || sameAudioEndpoint(selectedOutput?.id, device.id)}
                disabledLabel={routeApplying ? 'Application du routage en cours' : 'Déjà utilisé comme sortie casque'}
                roleLabel="Source"
                onSelect={onSelectCapture}
              />
            ))}
          </div>
        </div>
      )}

      <div className="audio-route-flow" aria-label="Résumé du routage audio">
        <span>{captureProvider === 'native-driver' ? 'Sound Spatializer' : selectedCapture?.name ?? 'Source externe à choisir'}</span>
        <ArrowRight size={15} />
        <strong>Moteur binaural</strong>
        <ArrowRight size={15} />
        <span>{selectedOutput?.name ?? 'Casque à choisir'}</span>
      </div>

      <div className="audio-route-toolbar output-heading">
        <span><strong>2. Sortie physique</strong><small>Casque filaire ou USB recommandé</small></span>
      </div>
      <div className="device-list">
        {selectableEndpoints.length === 0 ? (
          <div className="device-scan"><span className="spinner" /><strong>Recherche des sorties audio…</strong></div>
        ) : selectableEndpoints.map((device) => (
          <AudioDeviceButton
            key={`output-${device.id}`}
            device={device}
            selected={sameAudioEndpoint(selectedOutput?.id, device.id)}
            disabled={routeApplying || (captureProvider === 'external-render' && sameAudioEndpoint(selectedCapture?.id, device.id))}
            disabledLabel={routeApplying ? 'Application du routage en cours' : 'Déjà utilisé comme source'}
            roleLabel="Sortie"
            onSelect={onSelectOutput}
          />
        ))}
      </div>
      {captureProvider === 'native-driver' && driverEndpointAvailable ? (
        <div className="setup-callout">
          <MonitorCog size={19} />
          <span><strong>Ensuite, dans Windows</strong><small>Choisissez « Sound Spatializer » comme sortie système pour que les applications passent par le moteur.</small></span>
          <button type="button" className="secondary-button compact" onClick={onOpenSettings}><Settings2 size={15} /> Ouvrir Son</button>
        </div>
      ) : captureProvider === 'external-render' ? (
        <div className="setup-callout compatibility-callout">
          <MonitorCog size={19} />
          <span><strong>Mode compatibilité externe</strong><small>{selectedCapture ? `Dans Windows, choisissez « ${selectedCapture.name} » comme sortie. Désactivez tout monitoring direct pour éviter un doublage.` : 'Sélectionnez d’abord l’endpoint de rendu fourni par votre câble audio virtuel.'}</small></span>
          <button type="button" className="secondary-button compact" onClick={onOpenSettings} disabled={!selectedCapture}><Settings2 size={15} /> Ouvrir Son</button>
        </div>
      ) : (
        <div className="setup-callout preview-callout">
          <MonitorCog size={19} />
          <span><strong>Pilote natif indisponible</strong><small>Choisissez « Endpoint de rendu externe » ci-dessus pour obtenir du son, ou continuez en aperçu sans capture système.</small></span>
        </div>
      )}
    </div>
  );
}

function AudioDeviceButton({
  device,
  selected,
  disabled,
  disabledLabel,
  roleLabel,
  onSelect,
}: {
  device: AudioDeviceSummary;
  selected: boolean;
  disabled: boolean;
  disabledLabel: string;
  roleLabel: string;
  onSelect: (device: AudioDeviceSummary) => void;
}) {
  return (
    <button
      type="button"
      className={selected ? 'is-selected' : ''}
      onClick={() => onSelect(device)}
      disabled={disabled}
      aria-pressed={selected}
      aria-label={`${roleLabel} : ${device.name}${disabled ? ` — ${disabledLabel}` : ''}`}
    >
      <span className="device-icon">{device.transport === 'usb' ? <Usb size={21} /> : device.transport === 'bluetooth' ? <WifiOff size={21} /> : <Headphones size={21} />}</span>
      <span>
        <strong>{device.name}</strong>
        <small>{device.sampleRate / 1000} kHz · {device.transport.toUpperCase()}{device.isDefault ? ' · Sortie Windows actuelle' : ''}{disabled ? ` · ${disabledLabel}` : ''}</small>
      </span>
      <span className="selection-ring">{selected && <Check size={14} />}</span>
    </button>
  );
}

function CameraPreview({ source }: { source: HTMLVideoElement | null }) {
  const ref = useRef<HTMLVideoElement>(null);
  useEffect(() => {
    const preview = ref.current;
    if (!preview) return;
    preview.srcObject = source?.srcObject ?? null;
    if (preview.srcObject) void preview.play();
    return () => { preview.srcObject = null; };
  }, [source]);
  return <video ref={ref} muted playsInline aria-label="Aperçu local de la caméra" />;
}

function TrackingStep({
  running,
  tracking,
  videoElement,
  onStart,
  onStop,
}: {
  running: boolean;
  tracking: ReturnType<typeof useAppStore.getState>['tracking'];
  videoElement: HTMLVideoElement | null;
  onStart: () => void;
  onStop: () => void;
}) {
  const quality = useMemo(() => {
    if (!running) return { label: 'En attente', tone: 'offline' };
    if ((tracking.cameraFrameRate ?? tracking.fps) >= 55) return { label: 'Cadence cible atteinte', tone: 'good' };
    return { label: 'Cadence dégradée', tone: 'warning' };
  }, [running, tracking.cameraFrameRate, tracking.fps]);
  return (
    <div className="setup-step tracking-step">
      <div className="step-icon"><Camera size={27} /></div>
      <span className="eyebrow accent">SUIVI LOCAL</span>
      <h2>Votre visage pilote la scène, jamais le cloud.</h2>
      <p className="lead">L’image est analysée dans un Worker MediaPipe puis immédiatement supprimée. Seule une orientation 3DoF est transmise au moteur.</p>
      <div className={`camera-panel ${running ? 'is-running' : ''}`}>
        {running ? <CameraPreview source={videoElement} /> : <div className="camera-placeholder"><ScanFaceGraphic /><span>Caméra inactive</span></div>}
        <div className="camera-reticle"><i /><i /><i /><i /></div>
        {running && <span className="camera-local-badge"><ShieldCheck size={13} /> TRAITEMENT LOCAL</span>}
        <div className="camera-data">
          <span><small>CADENCE</small><strong>{tracking.fps || '—'} i/s</strong></span>
          <span><small>INFÉRENCE</small><strong>{tracking.processingMs ? tracking.processingMs.toFixed(1) : '—'} ms</strong></span>
          <span><small>ÉTAT</small><strong className={`tone-${quality.tone}`}>{quality.label}</strong></span>
        </div>
      </div>
      {tracking.error && <div className="inline-error">{tracking.error}</div>}
      <button type="button" className={running ? 'secondary-button' : 'primary-button'} onClick={running ? onStop : onStart}>
        {running ? <WifiOff size={16} /> : <Camera size={16} />}{running ? 'Désactiver la caméra' : 'Autoriser et démarrer'}
      </button>
    </div>
  );
}

function ScanFaceGraphic() {
  return <span className="scan-face-graphic"><Webcam size={38} strokeWidth={1.25} /></span>;
}

function CalibrationStep({ running, onStart, onCalibrate, yaw, pitch }: { running: boolean; onStart: () => void; onCalibrate: () => Promise<boolean>; yaw: number; pitch: number }) {
  const [calibrated, setCalibrated] = useState(false);
  const runCalibration = async () => {
    setCalibrated(await onCalibrate());
  };
  return (
    <div className="setup-step calibration-step">
      <div className={`calibration-visual ${calibrated ? 'is-complete' : ''}`}>
        <span className="calibration-axis axis-x" />
        <span className="calibration-axis axis-y" />
        <span className="calibration-head"><Headphones size={48} strokeWidth={1.2} /></span>
        <span className="calibration-speaker left">L</span>
        <span className="calibration-speaker right">R</span>
        {calibrated && <span className="calibration-check"><Check size={23} /></span>}
      </div>
      <span className="eyebrow accent">ORIENTATION NEUTRE</span>
      <h2>{calibrated ? 'Point de référence enregistré.' : 'Regardez simplement droit devant vous.'}</h2>
      <p className="lead">Asseyez-vous confortablement, face au centre virtuel. Cette pose devient le zéro de la scène acoustique.</p>
      <div className="calibration-readout">
        <span><Rotate3D size={16} /><small>YAW</small><strong>{yaw.toFixed(1)}°</strong></span>
        <span><Gauge size={16} /><small>PITCH</small><strong>{pitch.toFixed(1)}°</strong></span>
        <span><Radio size={16} /><small>CAMÉRA</small><strong>{running ? 'Prête' : 'Inactive'}</strong></span>
      </div>
      <button type="button" className="primary-button wide" onClick={() => (running ? void runCalibration() : onStart())}>
        {running ? <Crosshair size={17} /> : <Camera size={17} />}{running ? 'Définir comme position neutre' : 'Activer la caméra'}
      </button>
      <span className="fine-print"><AudioLines size={14} /> Vous pourrez recentrer la scène à tout moment.</span>
    </div>
  );
}
