import { useEffect, useMemo, useState } from 'react';
import { Activity, AlertTriangle, Camera, CheckCircle2, Clock3, Cpu, Download, Gauge, Headphones, Radio, RefreshCw, TimerReset, Waves } from 'lucide-react';
import { MetricCard, StatusDot } from '../components/ui/MetricCard';
import { PoseGauge } from '../components/ui/PoseGauge';
import { sameAudioEndpoint } from '../lib/runtime-capabilities';
import { desktopBridge } from '../lib/tauri-bridge';
import { useAppStore } from '../store/app-store';

const LIMITS = { motionTarget: 20, motionCeiling: 30, audioTarget: 20, trackingTarget: 55 };

export function DiagnosticsPage() {
  const engine = useAppStore((state) => state.engine);
  const tracking = useAppStore((state) => state.tracking);
  const preview = useAppStore((state) => state.previewMode);
  const notify = useAppStore((state) => state.notify);
  const scene = useAppStore((state) => state.scene);
  const audioDevices = useAppStore((state) => state.audioDevices);
  const audioRouteIssue = useAppStore((state) => state.audioRouteIssue);
  const patchScene = useAppStore((state) => state.patchScene);
  const setEngine = useAppStore((state) => state.setEngine);
  const [history, setHistory] = useState<number[]>(() => Array(48).fill(0));
  const captureEndpoint = scene.captureProvider === 'native-driver'
    ? audioDevices.find((device) => device.isSoundSpatializerEndpoint) ?? null
    : audioDevices.find((device) => sameAudioEndpoint(device.id, scene.captureEndpointId)) ?? null;
  const captureTitle = scene.captureProvider === 'native-driver' ? 'Capture pilote natif' : 'Capture endpoint externe';
  const inputLayoutLabel = engine.inputLayout === '5.1-surround' ? '5.1 surround' : 'stéréo';
  const captureMask = `0x${engine.captureChannelMask.toString(16).toUpperCase()}`;
  const captureDetail = captureEndpoint
    ? `${captureEndpoint.name} · ${engine.captureChannels} canaux · masque ${captureMask} · ${captureEndpoint.sampleRate / 1000} kHz · ${engine.capturePeriodMs.toFixed(2)} ms`
    : scene.captureProvider === 'native-driver' ? 'Endpoint Sound Spatializer indisponible' : 'Source externe indisponible';

  useEffect(() => {
    const timer = window.setInterval(() => {
      const value = engine.motionToSoundLatencyMs.p95;
      setHistory((items) => [...items.slice(-47), value]);
    }, 500);
    return () => window.clearInterval(timer);
  }, [engine.motionToSoundLatencyMs.p95, tracking.processingMs, tracking.state]);

  const latency = engine.motionToSoundLatencyMs.p95;
  const latencyTone = latency === 0 ? 'neutral' : latency <= LIMITS.motionTarget ? 'good' : latency <= LIMITS.motionCeiling ? 'warning' : 'danger';
  const quality = useMemo(() => {
    const issues: string[] = [];
    if (tracking.state !== 'running') issues.push('Suivi facial inactif');
    if (engine.connection !== 'ready' && engine.connection !== 'degraded') issues.push('Moteur audio inactif');
    if (tracking.pose?.trackingState === 'tracked' && engine.renderActive && !engine.trackingActive) issues.push('Pose caméra non exploitable par le moteur');
    if (tracking.fps > 0 && tracking.fps < LIMITS.trackingTarget) issues.push('Cadence caméra sous 55 i/s');
    if (latency === 0) issues.push('Estimation moteur mouvement→PCM indisponible');
    if (latency > LIMITS.motionCeiling) issues.push('Estimation moteur mouvement→PCM au-dessus du plafond indicatif');
    if (engine.xruns > 0) issues.push(`${engine.xruns} interruption(s) audio`);
    return issues;
  }, [engine.connection, engine.renderActive, engine.trackingActive, engine.xruns, latency, tracking.fps, tracking.pose?.trackingState, tracking.state]);

  const exportReport = async () => {
    try {
      const path = await desktopBridge.exportDiagnostics();
      notify({ tone: 'success', title: 'Rapport de diagnostic créé', detail: path ?? 'Disponible dans la version Windows installée.' });
    } catch (error) {
      notify({ tone: 'error', title: 'Export du diagnostic impossible', detail: error instanceof Error ? error.message : String(error) });
    }
  };

  const refreshStatus = async () => {
    try {
      const status = await desktopBridge.getEngineStatus();
      const state = useAppStore.getState();
      const selectedOutput = state.audioDevices.find((device) => sameAudioEndpoint(device.id, state.scene.physicalOutputDeviceId));
      setEngine({ ...status, physicalOutputName: selectedOutput?.name ?? null });
    } catch (error) {
      notify({ tone: 'warning', title: 'Statut moteur indisponible', detail: error instanceof Error ? error.message : String(error) });
    }
  };

  return (
    <div className="diagnostics-page">
      <section className={`qualification-banner panel ${quality.length ? 'has-issues' : 'is-qualified'}`}>
        <span className="qualification-icon">{quality.length ? <AlertTriangle size={23} /> : <CheckCircle2 size={23} />}</span>
        <div>
          <span className="eyebrow">ÉTAT LOGICIEL — NON QUALIFICATIF</span>
          <h2>{preview ? routeDiagnosticTitle(scene.captureProvider, audioRouteIssue) : quality.length ? 'Diagnostic logiciel incomplet' : 'Indicateurs logiciels dans les cibles'}</h2>
          <p>{preview
            ? routeDiagnosticDetail(scene.captureProvider, audioRouteIssue)
            : quality.length
              ? quality.join(' · ')
              : 'Ces indicateurs sont des estimations internes. Seul un banc physique mouvement→sortie casque peut qualifier la latence réelle.'}</p>
        </div>
        <button type="button" className="secondary-button" onClick={() => void exportReport()}><Download size={16} /> Exporter le rapport</button>
      </section>

      {engine.potentiallyBinaural && !scene.bypass && (
        <section className="binaural-warning panel" role="status">
          <AlertTriangle size={19} />
          <span><strong>Contenu potentiellement déjà binaural</strong><small>L’heuristique est indicative. Comparez manuellement avant de désactiver la spatialisation.</small></span>
          <button type="button" className="secondary-button compact" onClick={() => patchScene({ bypass: true })}>Activer le bypass</button>
        </section>
      )}

      <div className="metric-grid">
        <MetricCard icon={<Clock3 size={17} />} label="Mouvement → PCM (p95 estimé)" value={latency ? latency.toFixed(1) : '—'} unit="ms" tone={latencyTone} hint="Indicateur moteur · cible indicative ≤20 ms" />
        <MetricCard icon={<TimerReset size={17} />} label="Périodes audio estimées" value={engine.audioPipelineLatencyMs ? engine.audioPipelineLatencyMs.toFixed(1) : '—'} unit="ms" tone={engine.audioPipelineLatencyMs > 20 ? 'warning' : engine.audioPipelineLatencyMs ? 'good' : 'neutral'} hint={`Somme logicielle · ${engine.blockFrames} frames à ${engine.sampleRate / 1000} kHz`} />
        <MetricCard icon={<Camera size={17} />} label="Suivi facial" value={tracking.fps || '—'} unit="i/s" tone={tracking.fps >= 55 ? 'good' : tracking.fps ? 'warning' : 'neutral'} hint={`${tracking.processingMs ? tracking.processingMs.toFixed(1) : '—'} ms d’inférence`} />
        <MetricCard icon={<Cpu size={17} />} label="Charge callback instantanée" value={engine.callbackCpuPercent ? engine.callbackCpuPercent.toFixed(1) : '—'} unit="%" tone={engine.callbackCpuPercent > 50 ? 'danger' : engine.callbackCpuPercent ? 'good' : 'neutral'} hint="Dernier échantillon · objectif <50 %" />
      </div>

      <div className="diagnostic-grid">
        <section className="latency-chart panel">
          <div className="section-heading-row">
            <div><span className="eyebrow">HISTORIQUE LOGICIEL COURT</span><h2>Indicateur moteur mouvement → PCM</h2></div>
            <div className="chart-legend"><span><i className="legend-good" /> Cible 20 ms</span><span><i className="legend-ceiling" /> Plafond 30 ms</span></div>
          </div>
          <LatencyChart values={history} />
          <div className="chart-footer"><span>Il y a 24 s</span><span>Maintenant</span></div>
        </section>

        <section className="signal-chain panel">
          <div className="section-heading-row"><div><span className="eyebrow">CHEMIN DU SIGNAL</span><h2>État des modules</h2></div><button type="button" className="icon-button small" aria-label="Actualiser" onClick={() => void refreshStatus()}><RefreshCw size={15} /></button></div>
          <div className="signal-list">
            <SignalRow icon={<Waves size={17} />} title={captureTitle} detail={captureDetail} active={engine.captureActive} preview={preview} />
            <SignalRow icon={<Activity size={17} />} title="Pose exploitable par le moteur" detail={engine.trackingActive ? `${engine.trackingHz.toFixed(1)} échantillons/s` : 'Aucune pose exploitable'} active={engine.trackingActive} preview={preview} />
            <SignalRow icon={<Radio size={17} />} title="Convolution HRTF" detail={`Matrice dynamique ${engine.inputLayout === '5.1-surround' ? '5 × 2' : '2 × 2'}`} active={engine.connection === 'ready'} preview={preview} />
            <SignalRow icon={<Gauge size={17} />} title="FIFO & ASRC" detail={`${engine.fifoFillPercent.toFixed(0)} % · ${engine.clockDriftPpm.toFixed(1)} ppm`} active={engine.renderActive} preview={preview} />
            <SignalRow icon={<Headphones size={17} />} title="Rendu casque" detail={engine.physicalOutputName ?? 'Aucune sortie mesurée'} active={engine.renderActive} preview={preview} last />
          </div>
        </section>
      </div>

      <div className="diagnostic-grid lower">
        <section className="telemetry-table panel">
          <div className="section-heading-row"><div><span className="eyebrow">TÉLÉMÉTRIE LOCALE</span><h2>Horloges et tampons</h2></div><StatusDot state={engine.xruns ? 'warning' : engine.connection === 'ready' ? 'active' : 'offline'} label={engine.xruns ? `${engine.xruns} xruns` : 'Aucun xrun'} /></div>
          <dl>
            <div><dt>Format de travail</dt><dd>Float32 · {inputLayoutLabel} · {engine.sampleRate / 1000} kHz</dd></div>
            <div><dt>Capture effective</dt><dd>{engine.captureChannels} canaux · masque {captureMask}</dd></div>
            <div><dt>Sortie WASAPI</dt><dd>{engine.renderSampleFormat === 'pcm-s32' ? 'PCM32 signé' : engine.renderSampleFormat === 'float32' ? 'Float32' : '—'} · stéréo · {engine.sampleRate / 1000} kHz</dd></div>
            <div><dt>Période capture</dt><dd>{engine.capturePeriodMs.toFixed(2)} ms</dd></div>
            <div><dt>Période rendu</dt><dd>{engine.renderPeriodMs.toFixed(2)} ms</dd></div>
            <div><dt>Remplissage FIFO</dt><dd><span className="mini-meter"><i style={{ width: `${engine.fifoFillPercent}%` }} /></span>{engine.fifoFillFrames} frames</dd></div>
            <div><dt>Dérive d’horloge</dt><dd>{engine.clockDriftPpm.toFixed(1)} ppm</dd></div>
            <div><dt>Uptime moteur</dt><dd>{formatDuration(engine.uptimeSeconds)}</dd></div>
          </dl>
        </section>
        <section className="tracking-telemetry panel">
          <div className="section-heading-row"><div><span className="eyebrow">POSE 3DOF</span><h2>Suivi de la tête</h2></div><Activity size={19} className={tracking.state === 'running' ? 'pulse-icon' : ''} /></div>
          <div className="pose-gauges">
            {(['yaw', 'pitch', 'roll'] as const).map((axis) => (
              <PoseGauge key={axis} axis={axis} value={tracking.euler[axis]} />
            ))}
          </div>
          <div className="tracking-meta"><span>Confiance <strong>{tracking.pose ? `${(tracking.pose.confidence * 100).toFixed(0)} %` : '—'}</strong></span><span>État <strong>{tracking.pose?.trackingState ?? 'inactif'}</strong></span></div>
        </section>
      </div>
    </div>
  );
}

function routeDiagnosticTitle(provider: ReturnType<typeof useAppStore.getState>['scene']['captureProvider'], issue: ReturnType<typeof useAppStore.getState>['audioRouteIssue']) {
  if (issue === 'desktop-runtime-required') return 'Aperçu navigateur — capture système inactive';
  if (provider === 'external-render') return 'Routage externe incomplet — capture système inactive';
  return 'Pilote natif indisponible — capture système inactive';
}

function routeDiagnosticDetail(provider: ReturnType<typeof useAppStore.getState>['scene']['captureProvider'], issue: ReturnType<typeof useAppStore.getState>['audioRouteIssue']) {
  if (issue === 'desktop-runtime-required') return 'La capture WASAPI est disponible uniquement dans l’application de bureau Windows.';
  if (issue === 'capture-equals-output') return 'La source et la sortie sont identiques. Choisissez deux endpoints différents pour éviter une boucle audio.';
  if (issue === 'capture-endpoint-unavailable') return 'L’endpoint de rendu externe enregistré n’est plus actif. Rebranchez-le ou sélectionnez une autre source dans l’assistant.';
  if (issue === 'capture-layout-unsupported') return 'Le format 5.1 exige une source externe de six canaux avec masque 0x3F ou 0x60F. Aucun upmix n’est effectué.';
  if (issue === 'output-endpoint-unavailable' || issue === 'output-endpoint-required') return 'Le casque physique doit être sélectionné et actif avant le démarrage du moteur.';
  if (provider === 'external-render') return 'Choisissez explicitement un endpoint de rendu externe actif et un casque différent dans l’assistant.';
  return 'Installez le pilote natif ou choisissez un endpoint de rendu externe dans l’assistant. Le suivi facial reste disponible.';
}

function LatencyChart({ values }: { values: number[] }) {
  const width = 800;
  const height = 190;
  const max = 40;
  const points = values.map((value, index) => `${(index / (values.length - 1)) * width},${height - (Math.min(max, value) / max) * height}`).join(' ');
  const area = `0,${height} ${points} ${width},${height}`;
  return (
    <div className="chart-shell">
      <span className="target-line target-20" /><span className="target-line target-30" />
      <span className="chart-axis axis-0">0</span><span className="chart-axis axis-20">20</span><span className="chart-axis axis-40">40 ms</span>
      <svg viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" aria-label="Historique de l’estimation logicielle de latence">
        <defs><linearGradient id="latencyFill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stopColor="#65e5cf" stopOpacity=".3"/><stop offset="1" stopColor="#65e5cf" stopOpacity=".02"/></linearGradient></defs>
        <polygon points={area} fill="url(#latencyFill)" />
        <polyline points={points} fill="none" stroke="#68ead3" strokeWidth="2.5" vectorEffect="non-scaling-stroke" />
      </svg>
    </div>
  );
}

function SignalRow({ icon, title, detail, active, preview, last = false }: { icon: React.ReactNode; title: string; detail: string; active: boolean; preview: boolean; last?: boolean }) {
  return (
    <div className="signal-row">
      <span className={`signal-icon ${active ? 'is-active' : ''}`}>{icon}</span>
      <span><strong>{title}</strong><small>{detail}</small></span>
      <StatusDot state={active ? 'active' : 'offline'} label={active ? 'Actif' : preview ? 'Aperçu' : 'Inactif'} />
      {!last && <i className={`signal-connector ${active ? 'is-active' : ''}`} />}
    </div>
  );
}

const formatDuration = (seconds: number) => {
  if (!seconds) return '—';
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return `${hours} h ${String(minutes).padStart(2, '0')} min`;
};
