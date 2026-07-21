import { invoke } from '@tauri-apps/api/core';
import { open } from '@tauri-apps/plugin-dialog';
import type {
  AudioDeviceSummary,
  EngineCommandV1,
  EngineStatusV1,
  HeadPoseSampleV1,
  ImportedHeadphoneEq,
  ImportedSofa,
  PersistedAppConfigV1,
  QpcSnapshot,
} from '../types/contracts';
import { demoAudioDevices, emptyEngineStatus } from '../data/defaults';
import { fromWireEngineStatus, isWireEngineStatusV1, migratePersistedConfig, packHeadPoseV1, toPersistedDesktopConfig, toWireEngineCommand } from './contracts';
import { LatestValueQueue } from './latest-value-queue';

export const isTauriRuntime = () => '__TAURI_INTERNALS__' in window;

const CONFIG_KEY = 'sound-spatializer.config.v1';
type PoseDeliveryListener = (error: unknown | null) => void;
const poseDeliveryListeners = new Set<PoseDeliveryListener>();
const poseQueue = new LatestValueQueue<Uint8Array>(
  (payload) => invoke('push_head_pose', payload),
  {
    // Douze appels couvrent un aller-retour IPC d'environ 200 ms même à 60 i/s,
    // sans jamais accumuler plus d'une pose supplémentaire en attente.
    maxInFlight: 12,
    onSettled: (error) => {
      for (const listener of poseDeliveryListeners) listener(error);
    },
  },
);

export const desktopBridge = {
  async loadConfig(): Promise<PersistedAppConfigV1 | null> {
    if (!isTauriRuntime()) {
      const value = window.localStorage.getItem(CONFIG_KEY);
      return value ? migratePersistedConfig(JSON.parse(value)) : null;
    }
    const value = await invoke<string | null>('load_app_config');
    return value ? migratePersistedConfig(JSON.parse(value)) : null;
  },

  async saveConfig(config: PersistedAppConfigV1): Promise<void> {
    const payload = JSON.stringify(toPersistedDesktopConfig(config));
    if (!isTauriRuntime()) {
      window.localStorage.setItem(CONFIG_KEY, payload);
      return;
    }
    await invoke('save_app_config', { payload });
  },

  async listAudioDevices(): Promise<AudioDeviceSummary[]> {
    if (!isTauriRuntime()) return demoAudioDevices;
    return invoke<AudioDeviceSummary[]>('list_audio_devices');
  },

  async getBuiltinHrtfAvailability(): Promise<Record<string, boolean>> {
    if (!isTauriRuntime()) {
      return {
        'sadie-d2-kemar': true,
        'sadie-h6': true,
        'sadie-h9': true,
        'sadie-h10': true,
        'sadie-h19': true,
        'sadie-h20': true,
      };
    }
    return invoke<Record<string, boolean>>('get_builtin_hrtf_availability');
  },

  async openSoundSettings(): Promise<void> {
    if (!isTauriRuntime()) return;
    await invoke('open_windows_sound_settings');
  },

  async sendCommand(command: EngineCommandV1): Promise<void> {
    if (!isTauriRuntime()) return;
    await invoke('send_engine_command', { payload: JSON.stringify(toWireEngineCommand(command)) });
  },

  pushHeadPose(sample: HeadPoseSampleV1): void {
    if (!isTauriRuntime()) return;
    poseQueue.push(packHeadPoseV1(sample));
  },

  subscribeHeadPoseDelivery(listener: PoseDeliveryListener): () => void {
    poseDeliveryListeners.add(listener);
    return () => { poseDeliveryListeners.delete(listener); };
  },

  async getEngineStatus(): Promise<EngineStatusV1> {
    if (!isTauriRuntime()) return emptyEngineStatus;
    const value = await invoke<string | null>('get_engine_status');
    if (!value) return emptyEngineStatus;
    const parsed: unknown = JSON.parse(value);
    if (!isWireEngineStatusV1(parsed)) throw new Error('EngineStatusV1 reçu du moteur est invalide ou incompatible.');
    return fromWireEngineStatus(parsed);
  },

  async startEngine(): Promise<void> {
    if (!isTauriRuntime()) return;
    await invoke('start_engine');
  },

  async importSofa(): Promise<ImportedSofa | null> {
    if (!isTauriRuntime()) return null;
    const selected = await open({
      title: 'Importer une HRTF SOFA',
      multiple: false,
      directory: false,
      filters: [{ name: 'Spatially Oriented Format for Acoustics', extensions: ['sofa'] }],
    });
    if (!selected) return null;
    return invoke<ImportedSofa>('import_sofa', { source: selected });
  },

  async importHeadphoneEq(): Promise<ImportedHeadphoneEq | null> {
    if (!isTauriRuntime()) return null;
    const selected = await open({
      title: 'Importer une égalisation casque',
      multiple: false,
      directory: false,
      filters: [
        { name: 'Profils EQ pris en charge', extensions: ['json', 'txt'] },
        { name: 'Sound Spatializer EQ V1', extensions: ['json'] },
        { name: 'Equalizer APO / AutoEQ', extensions: ['txt'] },
      ],
    });
    if (!selected) return null;
    return invoke<ImportedHeadphoneEq>('import_headphone_eq', { source: selected });
  },

  async qpcSnapshot(): Promise<QpcSnapshot> {
    if (!isTauriRuntime()) {
      return { ticks: String(Math.round(performance.now() * 1_000_000)), frequency: '1000000000' };
    }
    return invoke<QpcSnapshot>('qpc_snapshot');
  },

  async exportDiagnostics(): Promise<string | null> {
    if (!isTauriRuntime()) return null;
    return invoke<string>('export_diagnostics');
  },
};

export class QpcClock {
  private frequency = 1_000_000_000;
  private ticksAtSync = 0n;
  private performanceAtSync = 0;

  async synchronize(samples = 6): Promise<void> {
    let best: { roundTrip: number; midpoint: number; snapshot: QpcSnapshot } | null = null;
    for (let i = 0; i < samples; i += 1) {
      const before = performance.now();
      const snapshot = await desktopBridge.qpcSnapshot();
      const after = performance.now();
      const candidate = { roundTrip: after - before, midpoint: (before + after) / 2, snapshot };
      if (!best || candidate.roundTrip < best.roundTrip) best = candidate;
    }
    if (!best) return;
    this.frequency = Number(best.snapshot.frequency);
    this.ticksAtSync = BigInt(best.snapshot.ticks);
    this.performanceAtSync = best.midpoint;
  }

  fromPerformanceTime(timestampMs: number): string {
    const elapsedTicks = BigInt(Math.round(((timestampMs - this.performanceAtSync) / 1000) * this.frequency));
    return String(this.ticksAtSync + elapsedTicks);
  }
}
