import { lazy, Suspense, useEffect, useRef } from 'react';
import { AppShell } from './components/layout/AppShell';
import { defaultScene, defaultWindowSpatialization } from './data/defaults';
import { describeExclusiveFallback } from './lib/audio-mode-diagnostics';
import { deriveRuntimeCapabilities, isAudioRouteStructurallyComplete, routeSelectionFromScene, sameAudioEndpoint, shouldAutoStartAudio } from './lib/runtime-capabilities';
import { desktopBridge, isTauriRuntime } from './lib/tauri-bridge';
import { AssistantPage } from './pages/AssistantPage';
import { selectPersistedConfig, useAppStore } from './store/app-store';
import { useTrackingController } from './tracking/TrackingProvider';
import type { AudioDeviceSummary, SceneConfigV2 } from './types/contracts';

const ScenePage = lazy(() => import('./pages/ScenePage').then((module) => ({ default: module.ScenePage })));
const ProfilesPage = lazy(() => import('./pages/ProfilesPage').then((module) => ({ default: module.ProfilesPage })));
const DiagnosticsPage = lazy(() => import('./pages/DiagnosticsPage').then((module) => ({ default: module.DiagnosticsPage })));

const isCommandOutcomeUnknown = (detail: string) =>
  detail.startsWith('ENGINE_COMMAND_OUTCOME_UNKNOWN ');

const runtimeRouteSignature = (
  scene: SceneConfigV2,
  devices: AudioDeviceSummary[],
): string => {
  const selection = routeSelectionFromScene(scene);
  const capabilities = deriveRuntimeCapabilities(
    isTauriRuntime(),
    devices,
    selection,
  );
  return JSON.stringify([
    selection.captureProvider,
    selection.captureEndpointId?.trim().toLocaleLowerCase('en-US') ?? null,
    selection.physicalOutputDeviceId?.trim().toLocaleLowerCase('en-US') ?? null,
    selection.inputLayout,
    capabilities.routeIssue,
    capabilities.captureEndpoint?.channelCount ?? 0,
    capabilities.captureEndpoint?.channelMask ?? 0,
    capabilities.outputEndpoint?.sampleRate ?? 0,
  ]);
};

export default function App() {
  const activeView = useAppStore((state) => state.activeView);
  const initialized = useAppStore((state) => state.initialized);
  const scene = useAppStore((state) => state.scene);
  const windowSpatialization = useAppStore((state) => state.windowSpatialization);
  const preferences = useAppStore((state) => state.preferences);
  const audioDevices = useAppStore((state) => state.audioDevices);
  const hydrate = useAppStore((state) => state.hydrate);
  const setAudioDevices = useAppStore((state) => state.setAudioDevices);
  const setEngine = useAppStore((state) => state.setEngine);
  const setRuntimeCapabilities = useAppStore((state) => state.setRuntimeCapabilities);
  const notify = useAppStore((state) => state.notify);
  const { calibrate } = useTrackingController();
  const firstPersist = useRef(true);
  const lastSyncedScene = useRef(scene);
  const lastSyncedWindowSpatialization = useRef(windowSpatialization);
  const lastCommandError = useRef<string | null>(null);
  const lastDeviceRefreshError = useRef<string | null>(null);
  const lastAudioModeFallback = useRef<string | null>(null);
  const previousRouteReady = useRef<boolean | null>(null);
  const configSyncQueue = useRef<Promise<void>>(Promise.resolve());
  const lastEngineConnectionGeneration = useRef(0);
  const lastEngineReplayError = useRef<string | null>(null);
  const bootstrapCompleted = useRef(false);
  const bootstrapNeedsReplay = useRef(false);

  useEffect(() => desktopBridge.subscribeEngineCommandWarnings((detail) => {
    if (!bootstrapCompleted.current) bootstrapNeedsReplay.current = true;
    const outcomeUnknown = isCommandOutcomeUnknown(detail);
    notify({
      tone: 'warning',
      title: outcomeUnknown
        ? 'Confirmation moteur différée'
        : 'Configuration appliquée, persistance moteur en échec',
      detail,
    });
  }), [notify]);

  useEffect(() => {
    let active = true;
    const bootstrap = async () => {
      bootstrapCompleted.current = false;
      bootstrapNeedsReplay.current = false;
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
        schemaVersion: 3 as const,
        scene: structuredClone(defaultScene),
        preferences: useAppStore.getState().preferences,
        windowSpatialization: structuredClone(defaultWindowSpatialization),
      };
      const capabilities = deriveRuntimeCapabilities(
        isTauriRuntime(),
        devices,
        routeSelectionFromScene(effectiveConfig.scene),
      );
      setRuntimeCapabilities(capabilities);
      const tauriRuntime = isTauriRuntime();
      let bootstrapCommandGeneration: number | null = null;
      const sendBootstrapCommand = async (
        command: Parameters<typeof desktopBridge.sendCommand>[0],
      ) => {
        if (!tauriRuntime) {
          await desktopBridge.sendCommand(command);
          return;
        }
        const acknowledgedGeneration =
          await desktopBridge.sendCommandWithGeneration(command);
        if (acknowledgedGeneration <= 0) {
          bootstrapNeedsReplay.current = true;
          return;
        }
        if (bootstrapCommandGeneration === null) {
          bootstrapCommandGeneration = acknowledgedGeneration;
        } else if (bootstrapCommandGeneration !== acknowledgedGeneration) {
          // A restart between two individually acknowledged commands leaves
          // neither engine guaranteed to own the complete canonical state.
          bootstrapNeedsReplay.current = true;
        }
      };
      try {
        await desktopBridge.startEngine();
        // Appliquez d'abord le mode et la taille de tampon sauvegardés. La
        // commande de route qui suit rouvre WASAPI à partir de cette scène ;
        // l'ordre inverse pourrait laisser le backend en mode compatibilité
        // alors que l'interface affiche « Faible latence ».
        if (isAudioRouteStructurallyComplete(routeSelectionFromScene(effectiveConfig.scene))) {
          await sendBootstrapCommand({ version: 1, type: 'set-scene', scene: effectiveConfig.scene });
        }
        try {
          await sendBootstrapCommand({
            version: 1,
            type: 'set-window-spatialization',
            config: effectiveConfig.windowSpatialization,
          });
        } catch (error) {
          const detail = error instanceof Error ? error.message : String(error);
          const outcomeUnknown = isCommandOutcomeUnknown(detail);
          bootstrapNeedsReplay.current = true;
          if (!outcomeUnknown) {
            effectiveConfig.windowSpatialization = {
              ...effectiveConfig.windowSpatialization,
              enabled: false,
            };
          }
          await desktopBridge.saveConfig(effectiveConfig).catch(() => undefined);
          if (active) notify({
            tone: 'warning',
            title: outcomeUnknown
              ? 'Confirmation du mode fenêtres différée'
              : 'Mode fenêtres indisponible',
            detail,
          });
        }
        if (capabilities.audioRouteReady && effectiveConfig.scene.physicalOutputDeviceId) {
          await sendBootstrapCommand({
            version: 1,
            type: 'set-audio-route',
            captureProvider: effectiveConfig.scene.captureProvider,
            captureEndpointId: effectiveConfig.scene.captureEndpointId,
            outputDeviceId: effectiveConfig.scene.physicalOutputDeviceId,
          });
        } else {
          // Le moteur peut survivre à l’interface. Une configuration devenue
          // invalide doit donc arrêter explicitement une ancienne route.
          await sendBootstrapCommand({ version: 1, type: 'stop' });
        }
        if (shouldAutoStartAudio(capabilities, effectiveConfig.preferences.onboardingComplete)) {
          await sendBootstrapCommand({ version: 1, type: 'start' });
        }
      } catch (engineError) {
        bootstrapNeedsReplay.current = true;
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
      if (!bootstrapNeedsReplay.current && tauriRuntime) {
        try {
          const synchronizedStatus = await desktopBridge.getEngineStatus();
          if (bootstrapCommandGeneration !== null &&
              bootstrapCommandGeneration > 0 &&
              synchronizedStatus.connectionGeneration ===
                bootstrapCommandGeneration) {
            lastEngineConnectionGeneration.current =
              synchronizedStatus.connectionGeneration;
          } else {
            // This also covers a restart after the final ACK but before the
            // status snapshot. Leave generation zero as the polling baseline
            // so the canonical state is replayed on the live connection.
            bootstrapNeedsReplay.current = true;
          }
        } catch {
          bootstrapNeedsReplay.current = true;
        }
      }
      bootstrapCompleted.current = true;
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
    const wasRouteReady = previousRouteReady.current;
    setRuntimeCapabilities(capabilities);
    if (wasRouteReady === true && !capabilities.audioRouteReady) {
      const stopMissingRoute = async () =>
        desktopBridge.runCommandTransaction(async (sendCommand) => {
          let lastError: unknown = null;
          for (let attempt = 0; attempt < 2; attempt += 1) {
            const latest = useAppStore.getState();
            const latestCapabilities = deriveRuntimeCapabilities(
              isTauriRuntime(),
              latest.audioDevices,
              routeSelectionFromScene(latest.scene),
            );
            if (latestCapabilities.audioRouteReady) return;
            try {
              await sendCommand({ version: 1, type: 'stop' });
              return;
            } catch (error) {
              lastError = error;
            }
          }
          const latest = useAppStore.getState();
          const latestCapabilities = deriveRuntimeCapabilities(
            isTauriRuntime(),
            latest.audioDevices,
            routeSelectionFromScene(latest.scene),
          );
          if (!latestCapabilities.audioRouteReady) {
            latest.notify({
              tone: 'error',
              title: 'Arrêt moteur non confirmé',
              detail: lastError instanceof Error
                ? lastError.message
                : String(lastError),
            });
          }
        });
      void stopMissingRoute();
      notify({
        tone: 'warning',
        title: 'Routage audio interrompu',
        detail: 'La source ou la sortie sélectionnée n’est plus disponible. Un arrêt moteur sûr a été demandé pour éviter une boucle audio.',
      });
    } else if (wasRouteReady === false && capabilities.audioRouteReady && scene.physicalOutputDeviceId) {
      // Un endpoint peut revenir après un hotplug ou une erreur temporaire
      // d’énumération. Réappliquez alors toute la route avant de déclarer le
      // moteur opérationnel ; changer seulement le badge UI laisserait le
      // moteur arrêté sur son ancienne configuration.
      const recoverRoute = async () =>
        desktopBridge.runCommandTransaction(async (sendCommand) => {
          try {
          const latest = useAppStore.getState();
          const recoveredScene = structuredClone(latest.scene);
          const recoveredCapabilities = deriveRuntimeCapabilities(
            isTauriRuntime(),
            latest.audioDevices,
            routeSelectionFromScene(recoveredScene),
          );
          let appliedRouteSignature = runtimeRouteSignature(
            recoveredScene,
            latest.audioDevices,
          );
          if (!recoveredCapabilities.audioRouteReady ||
              !recoveredScene.physicalOutputDeviceId) {
            await sendCommand({ version: 1, type: 'stop' });
            return;
          }
          await sendCommand({ version: 1, type: 'set-scene', scene: recoveredScene });
          await sendCommand({
            version: 1,
            type: 'set-audio-route',
            captureProvider: recoveredScene.captureProvider,
            captureEndpointId: recoveredScene.captureEndpointId,
            outputDeviceId: recoveredScene.physicalOutputDeviceId as string,
          });
          const afterRoute = useAppStore.getState();
          const afterCapabilities = deriveRuntimeCapabilities(
            isTauriRuntime(),
            afterRoute.audioDevices,
            routeSelectionFromScene(afterRoute.scene),
          );
          if (!afterCapabilities.audioRouteReady) {
            await sendCommand({ version: 1, type: 'stop' });
            return;
          }
          if (runtimeRouteSignature(afterRoute.scene, afterRoute.audioDevices) !==
              appliedRouteSignature) {
            const latestScene = structuredClone(afterRoute.scene);
            if (!latestScene.physicalOutputDeviceId) {
              await sendCommand({ version: 1, type: 'stop' });
              return;
            }
            await sendCommand({ version: 1, type: 'set-scene', scene: latestScene });
            await sendCommand({
              version: 1,
              type: 'set-audio-route',
              captureProvider: latestScene.captureProvider,
              captureEndpointId: latestScene.captureEndpointId,
              outputDeviceId: latestScene.physicalOutputDeviceId,
            });
            appliedRouteSignature = runtimeRouteSignature(
              latestScene,
              afterRoute.audioDevices,
            );
          }
          const beforeStart = useAppStore.getState();
          const beforeStartCapabilities = deriveRuntimeCapabilities(
            isTauriRuntime(),
            beforeStart.audioDevices,
            routeSelectionFromScene(beforeStart.scene),
          );
          if (!beforeStartCapabilities.audioRouteReady ||
              runtimeRouteSignature(beforeStart.scene, beforeStart.audioDevices) !==
                appliedRouteSignature) {
            await sendCommand({ version: 1, type: 'stop' });
            return;
          }
          if (beforeStart.preferences.onboardingComplete) {
            await sendCommand({ version: 1, type: 'start' });
          }
        } catch (error) {
          await sendCommand({ version: 1, type: 'stop' }).catch(() => undefined);
          const latest = useAppStore.getState();
          const latestCapabilities = deriveRuntimeCapabilities(
            isTauriRuntime(),
            latest.audioDevices,
            routeSelectionFromScene(latest.scene),
          );
          if (latestCapabilities.audioRouteReady) notify({
            tone: 'warning',
            title: 'Routage audio non rétabli',
            detail: error instanceof Error ? error.message : String(error),
          });
        }
        });
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
      const state = useAppStore.getState();
      lastSyncedScene.current = state.scene;
      lastSyncedWindowSpatialization.current = state.windowSpatialization;
      return;
    }
    const timer = window.setTimeout(() => {
      const state = useAppStore.getState();
      const previousScene = lastSyncedScene.current;
      const previousWindowSpatialization = lastSyncedWindowSpatialization.current;
      const sceneChanged = lastSyncedScene.current !== state.scene;
      const windowSpatializationChanged =
        lastSyncedWindowSpatialization.current !== state.windowSpatialization;
      const desiredScene = state.scene;
      const desiredWindowSpatialization = state.windowSpatialization;
      const desiredConfig = selectPersistedConfig(state);
      lastSyncedScene.current = state.scene;
      lastSyncedWindowSpatialization.current = state.windowSpatialization;

      const synchronize = async () => {
        const expectedConnectionGeneration =
          lastEngineConnectionGeneration.current;
        const expectedRouteSignature = runtimeRouteSignature(
          desiredScene,
          useAppStore.getState().audioDevices,
        );
        let phase: 'scene' | 'windows' | 'save' = 'scene';
        let sceneApplied = false;
        let windowsApplied = false;
        const disableWindowsBeforeScene =
          sceneChanged &&
          windowSpatializationChanged &&
          previousWindowSpatialization.enabled &&
          !desiredWindowSpatialization.enabled;
        const applyDesiredEngineState = async (
          sendCommand: typeof desktopBridge.sendCommand,
        ) => {
          if (disableWindowsBeforeScene && windowSpatializationChanged) {
            phase = 'windows';
            await sendCommand({
              version: 1,
              type: 'set-window-spatialization',
              config: desiredWindowSpatialization,
            });
            windowsApplied = true;
          }
          if (sceneChanged &&
              isAudioRouteStructurallyComplete(routeSelectionFromScene(desiredScene))) {
            phase = 'scene';
            await sendCommand({
              version: 1,
              type: 'set-scene',
              scene: desiredScene,
            });
            sceneApplied = true;
          }
          if (!disableWindowsBeforeScene && windowSpatializationChanged) {
            phase = 'windows';
            await sendCommand({
              version: 1,
              type: 'set-window-spatialization',
              config: desiredWindowSpatialization,
            });
            windowsApplied = true;
          }
        };
        try {
          await desktopBridge.runCommandTransaction(applyDesiredEngineState);
          phase = 'save';
          await desktopBridge.saveConfig(desiredConfig);
          lastCommandError.current = null;
        } catch (error) {
          const detail = error instanceof Error ? error.message : String(error);
          // Assignments happen inside the async helper, which TypeScript does
          // not include in its local control-flow narrowing.
          const failedPhase = String(phase) as 'scene' | 'windows' | 'save';
          const outcomeUnknown = isCommandOutcomeUnknown(detail);
          if (outcomeUnknown) {
            // A timeout is not a rejection: the single-threaded engine may
            // still finish the command. Keep the desired UI state and persist
            // it on the desktop side so the next synchronization or engine
            // generation deterministically reapplies it.
            await desktopBridge.saveConfig(desiredConfig).catch((saveError) => {
              useAppStore.getState().notify({
                tone: 'error',
                title: 'Configuration de secours non enregistrée',
                detail: saveError instanceof Error ? saveError.message : String(saveError),
              });
            });
            if (lastCommandError.current !== detail) {
              lastCommandError.current = detail;
              useAppStore.getState().notify({
                tone: 'warning',
                title: 'Confirmation moteur différée',
                detail,
              });
            }

            // Release the global command transaction before the bounded retry
            // so a fail-safe route stop can pass immediately. Retry only if no
            // newer UI state, route availability or engine generation exists.
            await new Promise((resolve) => window.setTimeout(resolve, 250));
            const latest = useAppStore.getState();
            const stillCurrent =
              (!sceneChanged || latest.scene === desiredScene) &&
              (!windowSpatializationChanged ||
                latest.windowSpatialization === desiredWindowSpatialization) &&
              lastEngineConnectionGeneration.current ===
                expectedConnectionGeneration &&
              runtimeRouteSignature(latest.scene, latest.audioDevices) ===
                expectedRouteSignature;
            if (stillCurrent) {
              sceneApplied = false;
              windowsApplied = false;
              try {
                await desktopBridge.runCommandTransaction(
                  applyDesiredEngineState,
                );
                await desktopBridge.saveConfig(desiredConfig);
                lastCommandError.current = null;
              } catch (retryError) {
                const retryDetail = retryError instanceof Error
                  ? retryError.message
                  : String(retryError);
                if (lastCommandError.current !== retryDetail) {
                  lastCommandError.current = retryDetail;
                  useAppStore.getState().notify({
                    tone: 'warning',
                    title: 'Resynchronisation moteur incomplète',
                    detail: retryDetail,
                  });
                }
              }
            }
            return;
          }
          if (lastCommandError.current !== detail) {
            lastCommandError.current = detail;
            const title = failedPhase === 'save'
              ? 'Configuration non enregistrée'
              : failedPhase === 'windows'
                ? 'Spatialisation des fenêtres non appliquée'
                : 'Scène non appliquée au moteur';
            useAppStore.getState().notify({
              tone: failedPhase === 'save' ? 'error' : 'warning',
              title,
              detail,
            });
          }

          let previousWindowRestored = false;
          if (failedPhase !== 'save' && disableWindowsBeforeScene &&
              windowsApplied && !sceneApplied) {
            try {
              await desktopBridge.sendCommand({
                version: 1,
                type: 'set-window-spatialization',
                config: previousWindowSpatialization,
              });
              previousWindowRestored = true;
            } catch (restoreError) {
              useAppStore.getState().notify({
                tone: 'warning',
                title: 'Restauration du mode fenêtres incomplète',
                detail: restoreError instanceof Error
                  ? restoreError.message
                  : String(restoreError),
              });
            }
          }

          // Ne restaure que si l'utilisateur n'a pas déjà produit une révision
          // plus récente pendant l'aller-retour IPC.
          const latest = useAppStore.getState();
          if (failedPhase !== 'save') {
            if (sceneChanged && !sceneApplied && latest.scene === desiredScene)
              latest.replaceScene(previousScene);
            const windowWasNotApplied = !windowsApplied;
            const windowWasCompensated =
              disableWindowsBeforeScene && !sceneApplied &&
              previousWindowRestored;
            if (windowSpatializationChanged &&
                (windowWasNotApplied || windowWasCompensated) &&
                latest.windowSpatialization === desiredWindowSpatialization) {
              latest.replaceWindowSpatialization(previousWindowSpatialization);
            }
          }
        }
      };
      configSyncQueue.current = configSyncQueue.current
        .catch(() => undefined)
        .then(synchronize);
    }, 280);
    return () => window.clearTimeout(timer);
  }, [initialized, preferences, scene, windowSpatialization]);

  useEffect(() => {
    if (!initialized) return;
    let active = true;
    let refreshInFlight = false;
    const refresh = async () => {
      if (refreshInFlight) return;
      refreshInFlight = true;
      try {
        const status = await desktopBridge.getEngineStatus();
        if (active) {
          const state = useAppStore.getState();
          const previousGeneration = lastEngineConnectionGeneration.current;
          const currentGeneration = status.connectionGeneration;
          if (currentGeneration > 0 &&
              previousGeneration > 0 &&
              currentGeneration < previousGeneration) {
            return;
          }
          const selectedOutput = state.audioDevices.find((device) => sameAudioEndpoint(device.id, state.scene.physicalOutputDeviceId));
          setEngine({ ...status, physicalOutputName: selectedOutput?.name ?? null });
          const replayFirstUnconfirmedGeneration =
            currentGeneration > 0 &&
            previousGeneration === 0 &&
            bootstrapNeedsReplay.current;
          const replayNewGeneration =
            currentGeneration > previousGeneration &&
            previousGeneration > 0;
          if (currentGeneration > previousGeneration) {
            lastEngineConnectionGeneration.current = currentGeneration;
          }
          if (replayFirstUnconfirmedGeneration || replayNewGeneration) {
            bootstrapNeedsReplay.current = false;
            // The native supervisor can restart the engine while the WebView
            // remains alive. Replay the desktop-owned canonical state once per
            // pipe generation so a failed native persistence cannot leave the
            // restarted engine on stale settings.
            const replayGeneration = currentGeneration;
            const replay = async (
              sendCommand: typeof desktopBridge.sendCommand,
            ) => {
              if (lastEngineConnectionGeneration.current !== replayGeneration) return;
              try {
                for (let attempt = 0; attempt < 2; attempt += 1) {
                  if (lastEngineConnectionGeneration.current !== replayGeneration) return;
                  const snapshot = useAppStore.getState();
                  const sceneReference = snapshot.scene;
                  const windowsReference = snapshot.windowSpatialization;
                  const replayScene = structuredClone(sceneReference);
                  const replayWindows = structuredClone(windowsReference);
                  const route = routeSelectionFromScene(replayScene);
                  const routeSignature = runtimeRouteSignature(
                    sceneReference,
                    snapshot.audioDevices,
                  );
                  if (!replayWindows.enabled) {
                    await sendCommand({
                      version: 1,
                      type: 'set-window-spatialization',
                      config: replayWindows,
                    });
                  }
                  if (isAudioRouteStructurallyComplete(route)) {
                    await sendCommand({
                      version: 1,
                      type: 'set-scene',
                      scene: replayScene,
                    });
                  }
                  if (replayWindows.enabled) {
                    await sendCommand({
                      version: 1,
                      type: 'set-window-spatialization',
                      config: replayWindows,
                    });
                  }
                  if (lastEngineConnectionGeneration.current !== replayGeneration) return;
                  const beforeRoute = useAppStore.getState();
                  if (beforeRoute.scene !== sceneReference ||
                      beforeRoute.windowSpatialization !== windowsReference ||
                      runtimeRouteSignature(beforeRoute.scene, beforeRoute.audioDevices) !==
                        routeSignature) {
                    continue;
                  }
                  const capabilities = deriveRuntimeCapabilities(
                    isTauriRuntime(),
                    beforeRoute.audioDevices,
                    route,
                  );
                  if (!capabilities.audioRouteReady ||
                      !replayScene.physicalOutputDeviceId) {
                    await sendCommand({ version: 1, type: 'stop' });
                    lastEngineReplayError.current = null;
                    return;
                  }
                  await sendCommand({
                    version: 1,
                    type: 'set-audio-route',
                    captureProvider: replayScene.captureProvider,
                    captureEndpointId: replayScene.captureEndpointId,
                    outputDeviceId: replayScene.physicalOutputDeviceId,
                  });
                  if (lastEngineConnectionGeneration.current !== replayGeneration) return;
                  const beforeStart = useAppStore.getState();
                  if (runtimeRouteSignature(beforeStart.scene, beforeStart.audioDevices) !==
                      routeSignature) {
                    continue;
                  }
                  if (shouldAutoStartAudio(
                    deriveRuntimeCapabilities(
                      isTauriRuntime(),
                      beforeStart.audioDevices,
                      routeSelectionFromScene(beforeStart.scene),
                    ),
                    beforeStart.preferences.onboardingComplete,
                  )) {
                    await sendCommand({ version: 1, type: 'start' });
                  } else {
                    await sendCommand({ version: 1, type: 'stop' });
                  }
                  if (runtimeRouteSignature(
                    useAppStore.getState().scene,
                    useAppStore.getState().audioDevices,
                  ) === routeSignature) {
                    lastEngineReplayError.current = null;
                    return;
                  }
                }
                if (lastEngineConnectionGeneration.current === replayGeneration) {
                  await sendCommand({ version: 1, type: 'stop' });
                }
                throw new Error(
                  'Le routage audio a changé plusieurs fois pendant la restauration.',
                );
              } catch (error) {
                const detail = error instanceof Error ? error.message : String(error);
                if (lastEngineConnectionGeneration.current === replayGeneration) {
                  await sendCommand({ version: 1, type: 'stop' }).catch(() => undefined);
                }
                if (lastEngineReplayError.current !== detail) {
                  lastEngineReplayError.current = detail;
                  useAppStore.getState().notify({
                    tone: 'warning',
                    title: 'État moteur non restauré',
                    detail,
                  });
                }
              }
            };
            configSyncQueue.current = configSyncQueue.current
              .catch(() => undefined)
              .then(() => desktopBridge.runCommandTransaction(replay));
          }
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
      } finally {
        refreshInFlight = false;
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
