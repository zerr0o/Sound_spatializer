import { useMemo, useRef, useState } from 'react';
import { AudioWaveform, Check, ChevronRight, CircleHelp, FileAudio, Gauge, Headphones, Import, SlidersHorizontal, Sparkles, Volume2, WandSparkles, X } from 'lucide-react';
import { HRTF_PROFILES } from '../data/hrtf-profiles';
import { desktopBridge, isTauriRuntime } from '../lib/tauri-bridge';
import { useAppStore } from '../store/app-store';
import { RangeControl, SegmentedControl, Toggle } from '../components/ui/Controls';
import type { AudioMode, EngineStatusV1, HeadphoneEqBand, HeadphoneEqConfig, ImportedHeadphoneEq } from '../types/contracts';
import { useHrtfAvailability } from '../hooks/useHrtfAvailability';
import { describeExclusiveFallback } from '../lib/audio-mode-diagnostics';

const DEFAULT_EQ_BANDS: HeadphoneEqBand[] = [
  { id: 'low', enabled: true, type: 'low-shelf', frequencyHz: 105, gainDb: 0, q: 0.7 },
  { id: 'presence', enabled: true, type: 'peak', frequencyHz: 2_800, gainDb: 0, q: 1.1 },
  { id: 'air', enabled: true, type: 'high-shelf', frequencyHz: 9_000, gainDb: 0, q: 0.7 },
];

const AUDIO_MODE_OPTIONS: ReadonlyArray<{ value: AudioMode; label: string }> = [
  { value: 'shared-low-latency', label: 'Faible latence' },
  { value: 'exclusive-pro', label: 'Pro exclusif' },
  { value: 'compatibility', label: 'Compatibilité 256' },
];

const AUDIO_MODE_DESCRIPTION: Record<AudioMode, string> = {
  'shared-low-latency': 'Mode partagé à la plus petite période prise en charge par le casque. Recommandé par défaut.',
  'exclusive-pro': 'Accès exclusif au casque pour réduire la période. Les autres applications ne peuvent pas l’utiliser simultanément.',
  compatibility: 'Tampon partagé de 256 frames, plus tolérant aux périphériques et pilotes audio difficiles.',
};

const AUDIO_MODE_FALLBACK_PREFIX = 'AUDIO_MODE_FALLBACK ';
const AUDIO_MODE_STATUS_POLL_MS = 150;
const AUDIO_MODE_STATUS_ATTEMPTS = 100;

type AudioModeOutcome =
  | { kind: 'applied'; status: EngineStatusV1 }
  | { kind: 'fallback'; status: EngineStatusV1 };

const wait = (milliseconds: number) => new Promise<void>((resolve) => window.setTimeout(resolve, milliseconds));

/**
 * Le pipe confirme uniquement l'écriture de la commande. Le résultat matériel
 * arrive ensuite dans EngineStatus : attendre ce statut empêche une scène Pro
 * non ouverte d'être persistée par l'effet global de synchronisation.
 */
export async function waitForAudioModeOutcome(
  requestedMode: AudioMode,
  readStatus: () => Promise<EngineStatusV1>,
  pollMs = AUDIO_MODE_STATUS_POLL_MS,
  attempts = AUDIO_MODE_STATUS_ATTEMPTS,
): Promise<AudioModeOutcome> {
  let lastReadError: unknown = null;
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    if (pollMs > 0) await wait(pollMs);
    try {
      const status = await readStatus();
      lastReadError = null;
      const streamsActive = status.captureActive && status.renderActive &&
        (status.connection === 'ready' || status.connection === 'degraded');
      if (streamsActive && status.audioMode === requestedMode) return { kind: 'applied', status };
      if (
        streamsActive &&
        requestedMode === 'exclusive-pro' &&
        status.audioMode === 'shared-low-latency' &&
        status.lastError?.includes(AUDIO_MODE_FALLBACK_PREFIX)
      ) {
        return { kind: 'fallback', status };
      }
      // L'échec exclusif est publié avant que la tentative de secours partagée
      // ne soit terminée. Conserver le diagnostic, mais ne pas annuler ce
      // fallback sur un statut d'erreur transitoire.
      if (status.connection === 'error' && status.lastError) lastReadError = new Error(status.lastError);
    } catch (error) {
      lastReadError = error;
    }
  }
  const suffix = lastReadError instanceof Error ? ` (${lastReadError.message})` : '';
  throw new Error(`Le moteur n’a pas confirmé le mode audio dans le délai imparti${suffix}.`);
}

const EQ_TYPE_LABELS: Record<HeadphoneEqBand['type'], string> = {
  peak: 'PK',
  'low-shelf': 'LS',
  'high-shelf': 'HS',
};

export function ProfilesPage() {
  const scene = useAppStore((state) => state.scene);
  const patchScene = useAppStore((state) => state.patchScene);
  const notify = useAppStore((state) => state.notify);
  const [abOpen, setAbOpen] = useState(false);
  const [abPair, setAbPair] = useState<[string, string]>(['sadie-d2-kemar', 'sadie-h6']);
  const [audition, setAudition] = useState<'A' | 'B'>('A');
  const [eqPreview, setEqPreview] = useState<ImportedHeadphoneEq | null>(null);
  const [eqImporting, setEqImporting] = useState(false);
  const [eqImportError, setEqImportError] = useState<string | null>(null);
  const [pendingAudioMode, setPendingAudioMode] = useState<AudioMode | null>(null);
  const audioModeRequest = useRef(0);
  const availability = useHrtfAvailability();
  const selected = useMemo(() => HRTF_PROFILES.find((profile) => profile.id === scene.hrtfProfileId), [scene.hrtfProfileId]);

  const selectProfile = async (id: string) => {
    try {
      const nextScene = { ...scene, hrtfProfileId: id, importedSofaHash: null, importedSofaPath: null };
      await desktopBridge.sendCommand({ version: 1, type: 'set-hrtf', profileId: id, sofaPath: null });
      patchScene(nextScene);
      return true;
    } catch (error) {
      notify({ tone: 'error', title: 'Profil HRTF non appliqué', detail: error instanceof Error ? error.message : String(error) });
      return false;
    }
  };

  const importSofa = async () => {
    try {
      const imported = await desktopBridge.importSofa();
      if (!imported) {
        notify({ tone: 'info', title: 'Import disponible dans l’application Windows', detail: 'Le mode navigateur ne peut pas copier un profil SOFA vers le moteur.' });
        return;
      }
      const profileId = `personal-${imported.hash.slice(0, 8)}`;
      await desktopBridge.sendCommand({ version: 1, type: 'set-hrtf', profileId, sofaPath: imported.localPath });
      patchScene({ hrtfProfileId: profileId, importedSofaHash: imported.hash, importedSofaPath: imported.localPath });
      notify({ tone: 'success', title: 'HRTF personnelle importée', detail: `${imported.fileName} a été copiée localement et transmise au moteur pour validation.` });
    } catch (error) {
      notify({ tone: 'error', title: 'Import SOFA impossible', detail: error instanceof Error ? error.message : String(error) });
    }
  };

  const toggleEq = (enabled: boolean) => {
    patchScene({ headphoneEq: { ...scene.headphoneEq, enabled, bands: scene.headphoneEq.bands.length ? scene.headphoneEq.bands : DEFAULT_EQ_BANDS } });
  };

  const updateBand = (id: string, patch: Partial<HeadphoneEqBand>) => {
    patchScene({ headphoneEq: { ...scene.headphoneEq, bands: scene.headphoneEq.bands.map((band) => (band.id === id ? { ...band, ...patch } : band)) } });
  };

  const importHeadphoneEq = async () => {
    setEqImportError(null);
    setEqImporting(true);
    try {
      const imported = await desktopBridge.importHeadphoneEq();
      if (imported) setEqPreview(imported);
    } catch (error) {
      const detail = error instanceof Error ? error.message : String(error);
      setEqImportError(detail);
      notify({ tone: 'error', title: 'Import EQ impossible', detail });
    } finally {
      setEqImporting(false);
    }
  };

  const applyImportedEq = async () => {
    if (!eqPreview) return;
    const headphoneEq: HeadphoneEqConfig = {
      enabled: true,
      preampDb: eqPreview.preampDb,
      profileName: eqPreview.profileName,
      bands: eqPreview.bands,
    };
    setEqImportError(null);
    try {
      await desktopBridge.sendCommand({ version: 1, type: 'set-headphone-eq', eq: headphoneEq });
      patchScene({ headphoneEq });
      setEqPreview(null);
      notify({ tone: 'success', title: 'Égalisation appliquée', detail: `${headphoneEq.profileName} est maintenant active.` });
    } catch (error) {
      const detail = error instanceof Error ? error.message : String(error);
      setEqImportError(detail);
      notify({ tone: 'error', title: 'EQ non appliquée', detail });
    }
  };

  const setAudioMode = async (audioMode: AudioMode) => {
    if (audioMode === scene.audioMode || pendingAudioMode) return;
    const previousMode = scene.audioMode;
    const request = ++audioModeRequest.current;
    setPendingAudioMode(audioMode);
    try {
      await desktopBridge.sendCommand({ version: 1, type: 'set-audio-mode', mode: audioMode });
      if (!isTauriRuntime()) {
        patchScene({ audioMode });
        return;
      }
      const outcome = await waitForAudioModeOutcome(audioMode, () => desktopBridge.getEngineStatus());
      if (audioModeRequest.current !== request) return;
      patchScene({ audioMode: outcome.status.audioMode });
      if (outcome.kind === 'fallback') {
        notify({
          tone: 'warning',
          title: 'Mode Pro exclusif indisponible',
          detail: describeExclusiveFallback(outcome.status.lastError),
        });
      }
    } catch (error) {
      // La scène UI n'a pas encore changé, donc sa persistance reste saine.
      // Réappliquez tout de même l'ancien mode au moteur si l'ouverture s'est
      // interrompue avant qu'un fallback natif ait pu être confirmé.
      await desktopBridge.sendCommand({ version: 1, type: 'set-audio-mode', mode: previousMode }).catch(() => undefined);
      notify({
        tone: 'warning',
        title: 'Mode audio non appliqué',
        detail: error instanceof Error ? error.message : String(error),
      });
    } finally {
      if (audioModeRequest.current === request) setPendingAudioMode(null);
    }
  };

  const auditionProfile = async (letter: 'A' | 'B') => {
    const profileId = abPair[letter === 'A' ? 0 : 1];
    try {
      await desktopBridge.sendCommand({ version: 1, type: 'set-hrtf', profileId, sofaPath: null });
      setAudition(letter);
    } catch (error) {
      notify({ tone: 'error', title: 'Écoute A/B indisponible', detail: error instanceof Error ? error.message : String(error) });
    }
  };

  const closeAb = () => {
    setAbOpen(false);
    void desktopBridge.sendCommand({
      version: 1,
      type: 'set-hrtf',
      profileId: scene.hrtfProfileId,
      sofaPath: scene.importedSofaPath,
    }).catch((error) => notify({
      tone: 'warning',
      title: 'Profil HRTF non restauré',
      detail: error instanceof Error ? error.message : String(error),
    }));
  };

  const openAb = () => {
    setAbOpen(true);
    void auditionProfile('A');
  };

  const chooseAbProfile = async (profileId: string) => {
    if (await selectProfile(profileId)) setAbOpen(false);
  };

  return (
    <div className="profiles-page">
      <section className="profiles-hero panel">
        <div>
          <span className="eyebrow accent">PROFIL ACTIF</span>
          <h2>{selected?.name ?? 'HRTF personnelle'}</h2>
          <p>La forme de chaque oreille est différente. Le meilleur profil est celui qui maintient les enceintes devant vous sans coloration gênante.</p>
        </div>
        <div className="profile-hero-actions">
          <button type="button" className="secondary-button" onClick={openAb}><WandSparkles size={16} /> Assistant d’écoute A/B</button>
          <button type="button" className="ghost-button" onClick={() => void importSofa()}><Import size={16} /> Importer un .SOFA</button>
        </div>
        <div className="earprint" aria-hidden="true"><span /><span /><span /></div>
      </section>

      <section className="profile-section">
        <div className="section-heading-row page-section-heading">
          <div><span className="eyebrow">BIBLIOTHÈQUE SADIE II</span><h2>Choisir une signature binaurale</h2></div>
          <span className="section-help"><CircleHelp size={15} /> La préférence est individuelle : comparez au casque.</span>
        </div>
        <div className="profile-grid">
          {HRTF_PROFILES.map((profile) => {
            const active = profile.id === scene.hrtfProfileId;
            const installed = availability?.[profile.id] ?? false;
            const checking = availability === null;
            return (
              <button key={profile.id} type="button" disabled={!installed && !checking} className={`hrtf-card panel ${active ? 'is-active' : ''} ${!installed && !checking ? 'is-missing' : ''}`} onClick={() => void selectProfile(profile.id)} style={{ '--profile-accent': profile.accent } as React.CSSProperties}>
                <span className="hrtf-card-top"><i className="profile-wave"><AudioWaveform size={23} /></i>{active ? <span className="active-badge"><Check size={12} /> ACTIF</span> : <span className={`resource-badge ${installed ? 'is-installed' : ''}`}>{checking ? 'VÉRIFICATION' : installed ? 'INSTALLÉ' : 'RESSOURCE ABSENTE'}</span>}</span>
                <span><small>{profile.subject}</small><strong>{profile.name}</strong><p>{profile.description}</p></span>
                <span className="profile-traits">{profile.traits.map((trait) => <i key={trait}>{trait}</i>)}</span>
              </button>
            );
          })}
          <button type="button" className="hrtf-card import-card panel" onClick={() => void importSofa()}>
            <span className="import-circle"><FileAudio size={24} /></span>
            <strong>Votre mesure SOFA</strong>
            <p>Format AES69 · convention SimpleFreeFieldHRIR</p>
            <span className="text-link">Importer <ChevronRight size={14} /></span>
          </button>
        </div>
      </section>

      <section className="audio-engine-settings panel">
        <div className="eq-heading">
          <span className="eq-icon"><AudioWaveform size={22} /></span>
          <div>
            <span className="eyebrow">RENDU BINAURAL</span>
            <h2>Correction du centre fantôme</h2>
            <p>
              Les deux enceintes virtuelles atteignent chacune vos deux oreilles avec un léger décalage :
              tout ce qui est commun aux canaux gauche et droit — voix, dialogues, basses — subit un filtre
              en peigne d’une dizaine de décibels, dont la fréquence suit l’écartement des émetteurs. La
              correction l’aplanit et laisse intacts les sons latéralisés. Désactivez-la pour comparer.
            </p>
          </div>
          <div className="eq-heading-actions">
            <Toggle
              checked={scene.phantomCentreCompensation}
              onChange={(phantomCentreCompensation) => patchScene({ phantomCentreCompensation })}
              label={scene.phantomCentreCompensation ? 'Activée' : 'Désactivée'}
            />
          </div>
        </div>
      </section>

      <section className="audio-engine-settings panel">
        <div className="eq-heading">
          <span className="eq-icon"><Gauge size={22} /></span>
          <div><span className="eyebrow">MOTEUR AUDIO</span><h2>Latence et niveau de sortie</h2><p>Adaptez le chemin WASAPI au casque sélectionné. Un changement de mode peut rouvrir le périphérique audio.</p></div>
        </div>
        <div className="audio-engine-editor">
          <div className="audio-mode-control">
            <span className="eyebrow">MODE DE BUFFER</span>
            <SegmentedControl<AudioMode>
              ariaLabel="Mode audio"
              value={pendingAudioMode ?? scene.audioMode}
              onChange={(audioMode) => void setAudioMode(audioMode)}
              options={AUDIO_MODE_OPTIONS}
              disabled={pendingAudioMode !== null}
            />
            <p role={pendingAudioMode ? 'status' : undefined}>
              {pendingAudioMode
                ? `Reconfiguration de WASAPI en cours… ${AUDIO_MODE_DESCRIPTION[pendingAudioMode]}`
                : AUDIO_MODE_DESCRIPTION[scene.audioMode]}
            </p>
          </div>
          <RangeControl
            label="Gain maître"
            value={scene.masterGainDb}
            min={-60}
            max={6}
            step={0.5}
            unit="dB"
            onChange={(masterGainDb) => patchScene({ masterGainDb })}
            hint="Conservez −6 dB ou moins pour la marge true-peak recommandée."
          />
        </div>
      </section>

      <section className="headphone-eq panel">
        <div className="eq-heading">
          <span className="eq-icon"><SlidersHorizontal size={22} /></span>
          <div><span className="eyebrow">COMPENSATION CASQUE</span><h2>Égalisation paramétrique</h2><p>Désactivée par défaut. Importez ou ajustez une mesure fiable de votre casque.</p></div>
          <div className="eq-heading-actions">
            <button type="button" className="secondary-button compact" disabled={eqImporting} onClick={() => void importHeadphoneEq()}><Import size={15} /> {eqImporting ? 'Lecture…' : 'Importer une EQ'}</button>
            <Toggle checked={scene.headphoneEq.enabled} onChange={toggleEq} label={scene.headphoneEq.enabled ? 'Activée' : 'Désactivée'} />
          </div>
        </div>
        {eqImportError && <div className="eq-import-error" role="alert">{eqImportError}</div>}
        {eqPreview && (
          <div className="eq-import-preview" aria-label="Prévisualisation de l’égalisation importée">
            <div className="eq-preview-heading">
              <span><small>PROFIL À CONFIRMER</small><strong>{eqPreview.profileName}</strong><i>{eqPreview.fileName} · {eqPreview.format === 'equalizer-apo' ? 'Equalizer APO / AutoEQ' : 'JSON Sound Spatializer V1'}</i></span>
              <dl>
                <div><dt>Préampli</dt><dd>{eqPreview.preampDb.toFixed(1)} dB</dd></div>
                <div><dt>Filtres actifs</dt><dd>{eqPreview.bands.filter((band) => band.enabled).length} / {eqPreview.bands.length}</dd></div>
              </dl>
            </div>
            <div className="eq-preview-filter-list">
              {eqPreview.bands.map((band, index) => (
                <span key={band.id} className={band.enabled ? '' : 'is-disabled'}>
                  <i>{index + 1}</i><strong>{EQ_TYPE_LABELS[band.type]}</strong><b>{band.frequencyHz.toFixed(0)} Hz</b><b>{band.gainDb >= 0 ? '+' : ''}{band.gainDb.toFixed(1)} dB</b><b>Q {band.q.toFixed(2)}</b><small>{band.enabled ? 'ON' : 'OFF'}</small>
                </span>
              ))}
            </div>
            <div className="eq-preview-actions">
              <p>La lecture seule n’a pas modifié l’EQ active. L’application ci-dessous active explicitement ce profil.</p>
              <button type="button" className="ghost-button compact" onClick={() => setEqPreview(null)}>Annuler</button>
              <button type="button" className="primary-button compact" onClick={() => void applyImportedEq()}><Check size={15} /> Appliquer et activer</button>
            </div>
          </div>
        )}
        {scene.headphoneEq.enabled && (
          <div className="eq-editor">
            <div className="eq-graph" aria-label="Réponse d’égalisation schématique">
              <span className="eq-zero" />
              <svg viewBox="0 0 800 160" preserveAspectRatio="none" role="img">
                <defs><linearGradient id="eqFill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stopColor="#65e5cf" stopOpacity=".25"/><stop offset="1" stopColor="#65e5cf" stopOpacity="0"/></linearGradient></defs>
                <path d="M0,80 C110,80 120,76 190,78 S310,83 400,80 S560,74 640,79 S730,82 800,80 L800,160 L0,160Z" fill="url(#eqFill)" />
                <path d="M0,80 C110,80 120,76 190,78 S310,83 400,80 S560,74 640,79 S730,82 800,80" fill="none" stroke="#65e5cf" strokeWidth="2" />
              </svg>
              <span className="eq-label low">20 Hz</span><span className="eq-label mid">1 kHz</span><span className="eq-label high">20 kHz</span>
            </div>
            <div className="eq-band-list">
              {scene.headphoneEq.bands.map((band, index) => (
                <div className="eq-band" key={band.id}>
                  <span className="band-index">{index + 1}</span>
                  <span><small>FRÉQUENCE</small><input type="number" value={band.frequencyHz} min={10} max={24000} onChange={(event) => updateBand(band.id, { frequencyHz: Number(event.target.value) })} /><i>Hz</i></span>
                  <span><small>GAIN</small><input type="number" value={band.gainDb} min={-24} max={24} step={0.5} onChange={(event) => updateBand(band.id, { gainDb: Number(event.target.value) })} /><i>dB</i></span>
                  <span><small>Q</small><input type="number" value={band.q} min={0.1} max={30} step={0.1} onChange={(event) => updateBand(band.id, { q: Number(event.target.value) })} /></span>
                </div>
              ))}
            </div>
            <div className="eq-footer">
              <RangeControl label="Préampli de sécurité" value={scene.headphoneEq.preampDb} min={-12} max={0} step={0.5} unit="dB" onChange={(preampDb) => patchScene({ headphoneEq: { ...scene.headphoneEq, preampDb } })} />
              <span className="headroom-badge"><Volume2 size={15} /> Headroom recommandé&nbsp;: −6 dB</span>
            </div>
          </div>
        )}
      </section>

      {abOpen && (
        <div className="modal-backdrop" role="presentation" onMouseDown={closeAb}>
          <div className="ab-modal panel" role="dialog" aria-modal="true" aria-labelledby="ab-title" onMouseDown={(event) => event.stopPropagation()}>
            <button type="button" className="modal-close icon-button" onClick={closeAb} aria-label="Fermer"><X size={18} /></button>
            <span className="step-icon"><Sparkles size={24} /></span>
            <span className="eyebrow accent">COMPARAISON AVEUGLE</span>
            <h2 id="ab-title">Quel profil garde la voix devant vous&nbsp;?</h2>
            <p>Alternez entre A et B en tournant doucement la tête. Privilégiez la stabilité frontale, puis le timbre.</p>
            <div className="ab-profile-selectors">
              {(['A', 'B'] as const).map((letter, index) => (
                <label key={letter} className={audition === letter ? 'is-auditioning' : ''}>
                  <span>{letter}</span>
                  <select value={abPair[index]} onChange={(event) => setAbPair((pair) => index === 0 ? [event.target.value, pair[1]] : [pair[0], event.target.value])}>
                    {HRTF_PROFILES.map((profile) => <option key={profile.id} value={profile.id} disabled={availability?.[profile.id] === false}>{profile.name}</option>)}
                  </select>
                  <button type="button" onClick={() => void auditionProfile(letter)}>{audition === letter ? 'En écoute' : 'Écouter'}</button>
                </label>
              ))}
            </div>
            <div className="ab-waveform"><AudioWaveform size={24} /><i /><i /><i /><i /><i /><i /><i /></div>
            <div className="ab-actions">
              <button type="button" className="secondary-button" onClick={() => void chooseAbProfile(abPair[0])}>Je préfère A</button>
              <button type="button" className="primary-button" onClick={() => void chooseAbProfile(abPair[1])}>Je préfère B</button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
