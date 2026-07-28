import { create } from 'zustand';
import {
  defaultPreferences,
  defaultScene,
  defaultWindowSpatialization,
  emptyEngineStatus,
  emptyTrackingMetrics,
} from '../data/defaults';
import type { AudioRouteIssue, RuntimeCapabilities } from '../lib/runtime-capabilities';
import type {
  AppPreferences,
  AudioDeviceSummary,
  EngineStatusV1,
  HeadPoseSampleV1,
  PersistedAppConfigV3,
  SceneConfigV2,
  TrackingMetrics,
  ViewId,
  WindowSpatializationConfigV1,
} from '../types/contracts';

interface ToastMessage {
  id: number;
  tone: 'info' | 'success' | 'warning' | 'error';
  title: string;
  detail?: string;
}

interface AppState {
  activeView: ViewId;
  initialized: boolean;
  previewMode: boolean;
  driverEndpointAvailable: boolean;
  audioRouteReady: boolean;
  audioRouteIssue: AudioRouteIssue | null;
  scene: SceneConfigV2;
  windowSpatialization: WindowSpatializationConfigV1;
  preferences: AppPreferences;
  engine: EngineStatusV1;
  tracking: TrackingMetrics;
  audioDevices: AudioDeviceSummary[];
  toasts: ToastMessage[];
  setActiveView: (view: ViewId) => void;
  hydrate: (config: PersistedAppConfigV3 | null) => void;
  patchScene: (patch: Partial<SceneConfigV2>) => void;
  replaceScene: (scene: SceneConfigV2) => void;
  patchWindowSpatialization: (patch: Partial<WindowSpatializationConfigV1>) => void;
  replaceWindowSpatialization: (config: WindowSpatializationConfigV1) => void;
  patchPreferences: (patch: Partial<AppPreferences>) => void;
  setEngine: (engine: EngineStatusV1) => void;
  patchTracking: (tracking: Partial<TrackingMetrics>) => void;
  setTrackingPose: (pose: HeadPoseSampleV1 | null, euler: TrackingMetrics['euler']) => void;
  setAudioDevices: (devices: AudioDeviceSummary[]) => void;
  setRuntimeCapabilities: (capabilities: RuntimeCapabilities) => void;
  notify: (toast: Omit<ToastMessage, 'id'>) => void;
  dismissToast: (id: number) => void;
}

let toastSequence = 0;

export const useAppStore = create<AppState>((set) => ({
  activeView: 'assistant',
  initialized: false,
  previewMode: false,
  driverEndpointAvailable: false,
  audioRouteReady: false,
  audioRouteIssue: null,
  scene: structuredClone(defaultScene),
  windowSpatialization: structuredClone(defaultWindowSpatialization),
  preferences: { ...defaultPreferences },
  engine: { ...emptyEngineStatus },
  tracking: { ...emptyTrackingMetrics },
  audioDevices: [],
  toasts: [],
  setActiveView: (activeView) => set({ activeView }),
  hydrate: (config) =>
    set({
      scene: config?.schemaVersion === 3 ? config.scene : structuredClone(defaultScene),
      windowSpatialization: config?.schemaVersion === 3
        ? config.windowSpatialization
        : structuredClone(defaultWindowSpatialization),
      preferences: config?.schemaVersion === 3 ? config.preferences : { ...defaultPreferences },
      activeView: config?.preferences.onboardingComplete ? 'scene' : 'assistant',
      initialized: true,
    }),
  patchScene: (patch) => set((state) => ({ scene: { ...state.scene, ...patch } })),
  replaceScene: (scene) => set({ scene }),
  patchWindowSpatialization: (patch) =>
    set((state) => ({ windowSpatialization: { ...state.windowSpatialization, ...patch } })),
  replaceWindowSpatialization: (windowSpatialization) => set({ windowSpatialization }),
  patchPreferences: (patch) =>
    set((state) => ({ preferences: { ...state.preferences, ...patch } })),
  setEngine: (engine) => set({ engine }),
  patchTracking: (patch) => set((state) => ({ tracking: { ...state.tracking, ...patch } })),
  setTrackingPose: (pose, euler) =>
    set((state) => ({ tracking: { ...state.tracking, pose, euler } })),
  setAudioDevices: (audioDevices) => set({ audioDevices }),
  setRuntimeCapabilities: (capabilities) => set({
    previewMode: capabilities.previewMode,
    driverEndpointAvailable: capabilities.driverEndpointAvailable,
    audioRouteReady: capabilities.audioRouteReady,
    audioRouteIssue: capabilities.routeIssue,
  }),
  notify: (toast) => {
    const id = ++toastSequence;
    set((state) => ({ toasts: [...state.toasts.slice(-2), { ...toast, id }] }));
    window.setTimeout(() => set((state) => ({ toasts: state.toasts.filter((item) => item.id !== id) })), 5200);
  },
  dismissToast: (id) => set((state) => ({ toasts: state.toasts.filter((item) => item.id !== id) })),
}));

export const selectPersistedConfig = (state: AppState): PersistedAppConfigV3 => ({
  schemaVersion: 3,
  scene: state.scene,
  preferences: state.preferences,
  windowSpatialization: state.windowSpatialization,
});
