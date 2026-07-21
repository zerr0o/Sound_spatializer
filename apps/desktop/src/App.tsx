import { lazy, Suspense, useEffect, useRef } from 'react';
import { AppShell } from './components/layout/AppShell';
import { defaultScene } from './data/defaults';
import { describeExclusiveFallback } from './lib/audio-mode-diagnostics';
import { deriveRuntimeCapabilities, isAudioRouteStructurallyComplete, routeSelectionFromScene, sameAudioEndpoint, shouldAutoStartAudio } from './lib/runtime-capabilities';
import { desktopBridge, isTauriRuntime } from './lib/tauri-bridge';
import { AssistantPage } from './pages/AssistantPage';
import { selectPersistedConfig, useAppStore } from './store/app-store';
import { useTrackingController } from './tracking/TrackingProvider';

const ScenePage = lazy(() => import('./pages/ScenePage').then((module) => ({ default: module.ScenePage })));
const ProfilesPage = lazy(() => import('./pages/ProfilesPage').then((module) => ({ default: module.ProfilesPage })));
const DiagnosticsPage = lazy(() => import('./pages/DiagnosticsPage').then((module) => ({ default: module.DiagnosticsPage })));

export default function App() {
  const activeView = useAppStore((state) => state.activeView);
  const initialized = useAppStore((state) => state.initialized);
  const scene = useAppStore((state) => state.scene);
  const preferences = useAppStore((state) => state.preferences);
  const audioDevices = useAppStore((state) => state.audioDevices);
  const hydrate = useAppStore((state) => state.hydrate);
  const setAudioDevices = useAppStore((state) => state.setAudioDevices);
  const setEngine = useAppStore((state) => state.setEngine);
  const setRuntimeCapabilities = useAppStore((state) => state.setRuntimeCapabilities);
  const notify = useAppStore((state) => state.notify);
  const { calibrate } = useTrackingController();
  const firstPersist = useRef(true);
  const lastCommandError = useRef<string | null>(null);
  const lastDeviceRefreshError = useRef<string | null>(null);
  const lastAudioModeFallback = useRef<string | null>(null);
  const previousRouteReady = useRef<boolean | null>(null);
  const routeSyncRevision = useRef(0);

  useEffect(() => {
    let active = true;
    const bootstrap = async () => {
      let config: Awaited<ReturnType<typeof desktopBridge.loadConfig>> = null;
      try {
        config = await desktopBridge.loadConfig();
      } catch (error) {
        if (active) notify({ tone: 'warning', title: 'Configuration réinitialisée', detail: error instanceof Error ? error.message : String(error) });
      }
      let devices: Awaited<ReturnType<typeof desktopBridge.listAudioDevices>> = [];
      try {
        devices = await desktopBridge.listAudioDevices();
      } catch (error) {
        if (active) notify({
          tone: 'warning',
          title: 'Sorties audio indisponibles',
          detail: error instanceof Error ? error.message : String(error),
        });
      }
      if (!active) return;
      const effectiveConfig = config ?? {
        schemaVersion: 2 as const,
        scene: structuredClone(defaultScene),
        preferences: useAppStore.getState().preferences,
      };
      const capabilities = deriveRuntimeCapabilities(
        isTauriRuntime(),
        devices,
        routeSelectionFromScene(effectiveConfig.scene),
      );
      setRuntimeCapabilities(capabilities);
      try {
        await desktopBridge.startEngine();
        // Appliquez d'abord le mode et la taille de tampon sauvegardés. La
        // commande de route qui suit rouvre WASAPI à partir de cette scène ;
        // l'ordre inverse pourrait laisser le backend en mode compatibilité
        // alors que l'interface affiche « Faible latence ».
        if (isAudioRouteStructurallyComplete(routeSelectionFromScene(effectiveConfig.scene))) {
          await desktopBridge.sendCommand({ version: 1, type: 'set-scene', scene: effectiveConfig.scene });
        }
        if (capabilities.audioRouteReady && effectiveConfig.scene.physicalOutputDeviceId) {
          await desktopBridge.sendCommand({
            version: 1,
            type: 'set-audio-route',
            captureProvider: effectiveConfig.scene.captureProvider,
            captureEndpointId: effectiveConfig.scene.captureEndpointId,
            outputDeviceId: effectiveConfig.scene.physicalOutputDeviceId,
          });
        } else {
          // Le moteur peut survivre à l’interface. Une configuration devenue
          // invalide doit donc arrêter explicitement une ancienne route.
          await desktopBridge.sendCommand({ version: 1, type: 'stop' });
        }
        if (shouldAutoStartAudio(capabilities, effectiveConfig.preferences.onboardingComplete)) {
          await desktopBridge.sendCommand({ version: 1, type: 'start' });
        }
      } catch (engineError) {
        // Si l’application d’une route échoue, ne laissez jamais un moteur
        // autonome continuer sur une destination qui n’est plus celle de l’UI.
        void desktopBridge.sendCommand({ version: 1, type: 'stop' }).catch(() => undefined);
        if (active) notify({
          tone: 'warning',
          title: 'Moteur audio indisponible',
          detail: engineError instanceof Error ? engineError.message : String(engineError),
        });
      }
      if (!active) return;
      // N’exposez l’interface qu’après la synchronisation initiale : une
      // sélection utilisateur ne peut ainsi pas être écrasée par effectiveConfig.
      hydrate(effectiveConfig);
      setAudioDevices(devices);
    };
    void bootstrap();
    return () => { active = false; };
  }, [hydrate, notify, setAudioDevices, setRuntimeCapabilities]);

  useEffect(() => {
    if (!initialized) return;
    const capabilities = deriveRuntimeCapabilities(
      isTauriRuntime(),
      audioDevices,
      routeSelectionFromScene(scene),
    );
    const revision = ++routeSyncRevision.current;
    const wasRouteReady = previousRouteReady.current;
    setRuntimeCapabilities(capabilities);
    if (wasRouteReady === true && !capabilities.audioRouteReady) {
      void desktopBridge.sendCommand({ version: 1, type: 'stop' }).catch(() => undefined);
      notify({
        tone: 'warning',
        title: 'Routage audio interrompu',
        detail: 'La source ou la sortie sélectionnée n’est plus disponible. Le moteur a été arrêté pour éviter une boucle audio.',
      });
    } else if (wasRouteReady === false && capabilities.audioRouteReady && scene.physicalOutputDeviceId) {
      // Un endpoint peut revenir après un hotplug ou une erreur temporaire
      // d’énumération. Réappliquez alors toute la route avant de déclarer le
      // moteur opérationnel ; changer seulement le badge UI laisserait le
      // moteur arrêté sur son ancienne configuration.
      const recoveredScene = structuredClone(scene);
      const recoverRoute = async () => {
        try {
          await desktopBridge.sendCommand({ version: 1, type: 'set-scene', scene: recoveredScene });
          if (routeSyncRevision.current !== revision) {
            await desktopBridge.sendCommand({ version: 1, type: 'stop' });
            return;
          }
          await desktopBridge.sendCommand({
            version: 1,
            type: 'set-audio-route',
            captureProvider: recoveredScene.captureProvider,
            captureEndpointId: recoveredScene.captureEndpointId,
            outputDeviceId: recoveredScene.physicalOutputDeviceId as string,
          });
          if (routeSyncRevision.current !== revision) {
            await desktopBridge.sendCommand({ version: 1, type: 'stop' });
            return;
          }
          if (useAppStore.getState().preferences.onboardingComplete) {
            await desktopBridge.sendCommand({ version: 1, type: 'start' });
          }
        } catch (error) {
          await desktopBridge.sendCommand({ version: 1, type: 'stop' }).catch(() => undefined);
          if (routeSyncRevision.current === revision) notify({
            tone: 'warning',
            title: 'Routage audio non rétabli',
            detail: error instanceof Error ? error.message : String(error),
          });
        }
      };
      void recoverRoute();
    }
    previousRouteReady.current = capabilities.audioRouteReady;
  }, [audioDevices, initialized, notify, scene.captureEndpointId, scene.captureProvider, scene.inputLayout, scene.physicalOutputDeviceId, setRuntimeCapabilities]);

  useEffect(() => {
    if (!initialized) return;
    let active = true;
    const refreshDevices = async () => {
      try {
        const devices = await desktopBridge.listAudioDevices();
        if (!active) return;
        setAudioDevices(devices);
        lastDeviceRefreshError.current = null;
      } catch (error) {
        if (!active) return;
        const detail = error instanceof Error ? error.message : String(error);
        if (lastDeviceRefreshError.current !== detail) {
          lastDeviceRefreshError.current = detail;
          notify({ tone: 'warning', title: 'Liste audio non actualisée', detail });
        }
      }
    };
    const onFocus = () => void refreshDevices();
    window.addEventListener('focus', onFocus);
    const timer = window.setInterval(() => void refreshDevices(), 4_000);
    return () => {
      active = false;
      window.removeEventListener('focus', onFocus);
      window.clearInterval(timer);
    };
  }, [initialized, notify, setAudioDevices]);

  useEffect(() => {
    if (!initialized) return;
    if (firstPersist.current) {
      firstPersist.current = false;
      return;
    }
    const timer = window.setTimeout(() => {
      const state = useAppStore.getState();
      void desktopBridge.saveConfig(selectPersistedConfig(state)).catch((error) => {
        state.notify({ tone: 'error', title: 'Configuration non enregistrée', detail: error instanceof Error ? error.message : String(error) });
      });
      if (!isAudioRouteStructurallyComplete(routeSelectionFromScene(state.scene))) {
        lastCommandError.current = null;
        return;
      }
      void desktopBridge.sendCommand({ version: 1, type: 'set-scene', scene: state.scene })
        .then(() => { lastCommandError.current = null; })
        .catch((error) => {
          const detail = error instanceof Error ? error.message : String(error);
          if (lastCommandError.current !== detail) {
            lastCommandError.current = detail;
            state.notify({ tone: 'warning', title: 'Scène non appliquée au moteur', detail });
          }
        });
    }, 280);
    return () => window.clearTimeout(timer);
  }, [initialized, preferences, scene]);

  useEffect(() => {
    if (!initialized) return;
    let active = true;
    const refresh = async () => {
      try {
        const status = await desktopBridge.getEngineStatus();
        if (active) {
          const state = useAppStore.getState();
          const selectedOutput = state.audioDevices.find((device) => sameAudioEndpoint(device.id, state.scene.physicalOutputDeviceId));
          setEngine({ ...status, physicalOutputName: selectedOutput?.name ?? null });
          const fallbackFromPersistedExclusive =
            status.captureActive && status.renderActive &&
            status.lastError?.includes('AUDIO_MODE_FALLBACK ') &&
            state.scene.audioMode === 'exclusive-pro' &&
            status.audioMode === 'shared-low-latency';
          if (fallbackFromPersistedExclusive) {
            // Une configuration Pro peut provenir d'une version antérieure qui
            // persistait le choix avant l'ouverture WASAPI. Réconcilier avec le
            // mode réellement ouvert empêche de retenter l'échec à chaque boot.
            state.patchScene({ audioMode: status.audioMode });
            if (lastAudioModeFallback.current !== status.lastError) {
              lastAudioModeFallback.current = status.lastError;
              state.notify({
                tone: 'warning',
                title: 'Mode Pro exclusif indisponible',
                detail: `${describeExclusiveFallback(status.lastError)} Le mode effectif a été enregistré.`,
              });
            }
          } else if (!status.lastError?.includes('AUDIO_MODE_FALLBACK ')) {
            lastAudioModeFallback.current = null;
          }
        }
      } catch (error) {
        if (active) setEngine({
          ...useAppStore.getState().engine,
          connection: 'error',
          captureActive: false,
          renderActive: false,
          lastError: error instanceof Error ? error.message : String(error),
        });
      }
    };
    void refresh();
    const timer = window.setInterval(() => void refresh(), 250);
    return () => { active = false; window.clearInterval(timer); };
  }, [initialized, setEngine]);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.code !== 'Space' || activeView !== 'scene') return;
      const target = event.target as HTMLElement | null;
      if (target?.matches('input, select, textarea, button, [contenteditable="true"]')) return;
      event.preventDefault();
      void calibrate();
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [activeView, calibrate]);

  if (!initialized) return <SplashScreen />;

  return (
    <AppShell>
      {activeView === 'assistant' && <AssistantPage />}
      <Suspense fallback={<PageLoader />}>
        {activeView === 'scene' && <ScenePage />}
        {activeView === 'profiles' && <ProfilesPage />}
        {activeView === 'diagnostics' && <DiagnosticsPage />}
      </Suspense>
    </AppShell>
  );
}

function PageLoader() {
  return <div className="page-loader"><span className="spinner" /><small>Chargement du module…</small></div>;
}

function SplashScreen() {
  return (
    <div className="splash-screen">
      <span className="brand-mark large" aria-hidden="true"><i /><i /><i /></span>
      <strong>Sound Spatializer</strong>
      <small>Initialisation du moteur binaural…</small>
      <span className="splash-progress"><i /></span>
    </div>
  );
}
