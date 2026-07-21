import type { PropsWithChildren } from 'react';
import {
  Activity,
  AudioLines,
  Box,
  CircleGauge,
  Headphones,
  LifeBuoy,
  Power,
  SlidersHorizontal,
  Sparkles,
} from 'lucide-react';
import { useAppStore } from '../../store/app-store';
import type { ViewId } from '../../types/contracts';
import { StatusDot } from '../ui/MetricCard';

const NAV_ITEMS: Array<{ id: ViewId; label: string; detail: string; icon: typeof Box }> = [
  { id: 'assistant', label: 'Assistant', detail: 'Installation', icon: Sparkles },
  { id: 'scene', label: 'Scène', detail: 'Enceintes & pièce', icon: Box },
  { id: 'profiles', label: 'HRTF & casque', detail: 'Personnalisation', icon: Headphones },
  { id: 'diagnostics', label: 'Diagnostic', detail: 'Temps réel', icon: CircleGauge },
];

const TITLES: Record<ViewId, { eyebrow: string; title: string }> = {
  assistant: { eyebrow: 'Mise en route', title: 'Configuration guidée' },
  scene: { eyebrow: 'Scène virtuelle', title: 'Votre espace d’écoute' },
  profiles: { eyebrow: 'Signature binaurale', title: 'HRTF & compensation casque' },
  diagnostics: { eyebrow: 'Moteur temps réel', title: 'Diagnostic de performance' },
};

export function AppShell({ children }: PropsWithChildren) {
  const activeView = useAppStore((state) => state.activeView);
  const setActiveView = useAppStore((state) => state.setActiveView);
  const engine = useAppStore((state) => state.engine);
  const tracking = useAppStore((state) => state.tracking);
  const scene = useAppStore((state) => state.scene);
  const patchScene = useAppStore((state) => state.patchScene);
  const previewMode = useAppStore((state) => state.previewMode);
  const audioRouteIssue = useAppStore((state) => state.audioRouteIssue);
  const title = TITLES[activeView];
  const trackingLive = tracking.state === 'running' && tracking.pose?.trackingState === 'tracked';
  const engineLive = engine.connection === 'ready';

  return (
    <div className="app-frame">
      <aside className="sidebar">
        <div className="brand" data-tauri-drag-region>
          <span className="brand-mark" aria-hidden="true">
            <i />
            <i />
            <i />
          </span>
          <span>
            <strong>Sound</strong>
            <small>Spatializer</small>
          </span>
        </div>

        <nav className="main-nav" aria-label="Navigation principale">
          <span className="nav-section-label">Espace de travail</span>
          {NAV_ITEMS.map((item) => {
            const Icon = item.icon;
            return (
              <button
                key={item.id}
                type="button"
                className={activeView === item.id ? 'is-active' : ''}
                onClick={() => setActiveView(item.id)}
                aria-current={activeView === item.id ? 'page' : undefined}
              >
                <Icon size={19} strokeWidth={1.7} />
                <span>
                  <strong>{item.label}</strong>
                  <small>{item.detail}</small>
                </span>
              </button>
            );
          })}
        </nav>

        <div className="sidebar-system">
          <span className="nav-section-label">Système</span>
          <div className="system-line">
            <AudioLines size={16} />
            <span>
              <small>Moteur audio</small>
              <StatusDot state={engineLive ? 'active' : 'offline'} label={engineLive ? 'Actif' : previewMode ? 'Audio non configuré' : 'Hors ligne'} />
            </span>
          </div>
          <div className="system-line">
            <Activity size={16} />
            <span>
              <small>Suivi facial</small>
              <StatusDot state={trackingLive ? 'active' : tracking.error ? 'error' : 'offline'} label={trackingLive ? `${tracking.fps} i/s` : 'Inactif'} />
            </span>
          </div>
        </div>

        <button type="button" className="support-link" onClick={() => setActiveView('assistant')}>
          <LifeBuoy size={16} />
          Configuration guidée
        </button>
        <div className="version-label">EARLY ACCESS · 0.1.0</div>
      </aside>

      <main className="workspace">
        <header className="topbar" data-tauri-drag-region>
          <div className="page-title">
            <span>{title.eyebrow}</span>
            <h1>{title.title}</h1>
          </div>
          <div className="topbar-actions">
            {previewMode && <span className="preview-chip">{audioRouteChip(audioRouteIssue, scene.captureProvider)}</span>}
            <div className="pose-readout" title="Orientation de la tête">
              <span>YAW <strong>{tracking.euler.yaw.toFixed(0)}°</strong></span>
              <i />
              <span>PITCH <strong>{tracking.euler.pitch.toFixed(0)}°</strong></span>
            </div>
            <span className="binaural-hint">Déjà binaural&nbsp;? utilisez le bypass</span>
            <button
              type="button"
              className={`bypass-button ${scene.bypass ? 'is-bypassed' : ''}`}
              onClick={() => patchScene({ bypass: !scene.bypass })}
              aria-pressed={scene.bypass}
            >
              <Power size={16} />
              {scene.bypass ? 'Bypass' : 'Spatialisation'}
            </button>
            <button type="button" className="icon-button" aria-label="Ouvrir les réglages HRTF et casque" onClick={() => setActiveView('profiles')}>
              <SlidersHorizontal size={18} />
            </button>
          </div>
        </header>
        <div className="page-content">{children}</div>
      </main>
      <ToastStack />
    </div>
  );
}

function audioRouteChip(issue: ReturnType<typeof useAppStore.getState>['audioRouteIssue'], provider: ReturnType<typeof useAppStore.getState>['scene']['captureProvider']) {
  if (issue === 'desktop-runtime-required') return 'Aperçu navigateur';
  if (issue === 'native-endpoint-unavailable') return 'Pilote natif absent';
  if (provider === 'external-render') return 'Routage externe incomplet';
  return 'Audio non configuré';
}

function ToastStack() {
  const toasts = useAppStore((state) => state.toasts);
  const dismiss = useAppStore((state) => state.dismissToast);
  return (
    <div className="toast-stack" aria-live="polite">
      {toasts.map((toast) => (
        <button key={toast.id} type="button" className={`toast tone-${toast.tone}`} onClick={() => dismiss(toast.id)}>
          <span className="toast-icon">
            {toast.tone === 'success' ? <Sparkles size={16} /> : <Activity size={16} />}
          </span>
          <span>
            <strong>{toast.title}</strong>
            {toast.detail && <small>{toast.detail}</small>}
          </span>
        </button>
      ))}
    </div>
  );
}
