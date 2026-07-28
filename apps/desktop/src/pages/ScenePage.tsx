import { useMemo, useState } from 'react';
import {
  AppWindow,
  Camera,
  Crosshair,
  Link2,
  Maximize2,
  Monitor,
  Radio,
  Rotate3D,
  Ruler,
  ScanFace,
  Sparkles,
  Trash2,
  Unlink2,
  Volume2,
} from 'lucide-react';
import { SpatialScene } from '../components/scene/SpatialScene';
import { RangeControl, SegmentedControl, Toggle } from '../components/ui/Controls';
import { applyMaterialPreset, MATERIAL_PRESETS, ROOM_SURFACES, setSurfaceBand, type AcousticBand, type RoomSurfaceKey } from '../lib/room-acoustics';
import { speakerPolarFromListener, speakerPositionFromPolar } from '../lib/scene-geometry';
import { isTauriRuntime } from '../lib/tauri-bridge';
import { supportsSurround5_1 } from '../lib/runtime-capabilities';
import { isSpeakerRouted, pairedSpeakerChannel } from '../lib/speaker-layout';
import { useAppStore } from '../store/app-store';
import { useTrackingController } from '../tracking/TrackingProvider';
import type {
  Channel,
  DisplayRuntimeInfo,
  InputLayout,
  Quaternion,
  SpatialInputMode,
  SpeakerConfig,
  SpeakerSet,
  WindowAudioSourceInfo,
  WindowSourceRule,
  WindowSpatializationConfigV1,
} from '../types/contracts';

type InspectorMode = 'speakers' | 'room';
type WindowInspectorMode = 'displays' | 'sources';
type ReflectionOrderOption = '0' | '1' | '2';

interface WindowSourceEntry {
  key: string;
  applicationId: string;
  source: WindowAudioSourceInfo | null;
  rule: WindowSourceRule | null;
}

const displayYawDegrees = (orientation: Quaternion) =>
  Math.atan2(
    2 * (orientation.w * orientation.y + orientation.x * orientation.z),
    1 - 2 * (orientation.y * orientation.y + orientation.z * orientation.z),
  ) * 180 / Math.PI;

const displayOrientationFromYaw = (degrees: number): Quaternion => {
  const halfAngle = degrees * Math.PI / 360;
  return { x: 0, y: Math.sin(halfAngle), z: 0, w: Math.cos(halfAngle) };
};

export function ScenePage() {
  const scene = useAppStore((state) => state.scene);
  const windowSpatialization = useAppStore((state) => state.windowSpatialization);
  const engine = useAppStore((state) => state.engine);
  const tracking = useAppStore((state) => state.tracking);
  const patchScene = useAppStore((state) => state.patchScene);
  const replaceWindowSpatialization = useAppStore((state) => state.replaceWindowSpatialization);
  const notify = useAppStore((state) => state.notify);
  const audioDevices = useAppStore((state) => state.audioDevices);
  const { start, calibrate, running } = useTrackingController();
  const [selected, setSelected] = useState<Channel>('L');
  const [linked, setLinked] = useState(true);
  const [mode, setMode] = useState<InspectorMode>('speakers');
  const [windowMode, setWindowMode] = useState<WindowInspectorMode>('displays');
  const [selectedDisplayId, setSelectedDisplayId] = useState<string | null>(null);
  const [selectedSourceId, setSelectedSourceId] = useState<string | null>(null);
  const [selectedSurface, setSelectedSurface] = useState<RoomSurfaceKey>('front');
  const speaker = scene.speakers.find((item) => item.id === selected) ?? scene.speakers[0];
  const geometry = useMemo(
    () => speakerPolarFromListener(speaker.position, scene.listener.position),
    [scene.listener.position, speaker.position],
  );
  const surface = scene.room.surfaces[selectedSurface];
  const externalCapture = scene.captureProvider === 'external-render'
    ? audioDevices.find((device) => device.id.toLocaleLowerCase('en-US') === scene.captureEndpointId?.toLocaleLowerCase('en-US')) ?? null
    : null;
  const surroundUnavailable = scene.captureProvider !== 'external-render' || !supportsSurround5_1(externalCapture);
  const pairedChannel = pairedSpeakerChannel(selected);
  const spatialInputMode: SpatialInputMode = windowSpatialization.enabled ? 'process-windows' : 'endpoint-mix';
  const windowRenderingEffective = engine.spatialInputMode === 'process-windows';
  const windowEndpointFallback = windowSpatialization.enabled
    && engine.windowAudio.supported
    && engine.windowAudio.running
    && engine.windowAudio.sourceCount > 0
    && !windowRenderingEffective;
  const displays = engine.windowAudio.displays;
  const windowSources = engine.windowAudio.windowSources;
  const windowSourceEntries = useMemo<WindowSourceEntry[]>(() => {
    const normalizeApplicationId = (value: string) => value.toLocaleLowerCase('en-US');
    const runtimeApplicationIds = new Set(
      windowSources.map((source) => normalizeApplicationId(source.applicationId)),
    );
    return [
      ...windowSources.map((source) => ({
        key: `source:${source.sourceId}`,
        applicationId: source.applicationId,
        source,
        rule: windowSpatialization.sourceRules.find(
          (item) => normalizeApplicationId(item.applicationId) ===
            normalizeApplicationId(source.applicationId),
        ) ?? null,
      })),
      ...windowSpatialization.sourceRules
        .filter((rule) => !runtimeApplicationIds.has(
          normalizeApplicationId(rule.applicationId),
        ))
        .map((rule) => ({
          key: `rule:${rule.applicationId}`,
          applicationId: rule.applicationId,
          source: null,
          rule,
        })),
    ];
  }, [windowSources, windowSpatialization.sourceRules]);
  const visualDisplays = displays.map((display) => {
    const calibration = windowSpatialization.displayCalibrations.find((item) => item.displayId === display.displayId);
    return calibration ? {
      ...display,
      center: calibration.center,
      widthM: calibration.widthM,
      heightM: calibration.heightM,
      orientation: calibration.orientation,
      calibrated: true,
    } : display;
  });
  const selectedDisplay = displays.find((display) => display.displayId === selectedDisplayId) ?? displays[0] ?? null;
  const selectedSourceEntry = windowSourceEntries.find((entry) => entry.key === selectedSourceId)
    ?? windowSourceEntries[0]
    ?? null;
  const selectedSource = selectedSourceEntry?.source ?? null;

  const applyWindowSpatialization = (patch: Partial<WindowSpatializationConfigV1>) => {
    const next: WindowSpatializationConfigV1 = {
      ...windowSpatialization,
      ...patch,
      maxSources: Math.max(1, Math.min(8, Math.round(patch.maxSources ?? windowSpatialization.maxSources))),
      stereoSpread: Math.max(0, Math.min(1, patch.stereoSpread ?? windowSpatialization.stereoSpread)),
    };
    replaceWindowSpatialization(next);
  };

  const setSpatialInputMode = (nextMode: SpatialInputMode) => {
    if (nextMode === 'process-windows' && scene.inputLayout !== 'stereo') {
      notify({
        tone: 'warning',
        title: 'Mode fenêtres stéréo',
        detail: 'Repassez le format d’entrée sur Stéréo avant d’activer la spatialisation par fenêtres.',
      });
      return;
    }
    applyWindowSpatialization({ enabled: nextMode === 'process-windows' });
    if (nextMode === 'process-windows') setWindowMode('displays');
  };

  const displayCalibration = selectedDisplay
    ? windowSpatialization.displayCalibrations.find((item) => item.displayId === selectedDisplay.displayId) ?? null
    : null;

  const ensureDisplayCalibration = (display: DisplayRuntimeInfo) => displayCalibration ?? {
    displayId: display.displayId,
    center: { ...display.center },
    widthM: display.widthM,
    heightM: display.heightM,
    orientation: { ...display.orientation },
  };

  const updateDisplayCalibration = (
    display: DisplayRuntimeInfo,
    patch: Partial<ReturnType<typeof ensureDisplayCalibration>>,
  ) => {
    const calibration = { ...ensureDisplayCalibration(display), ...patch };
    const displayCalibrations = windowSpatialization.displayCalibrations
      .filter((item) => item.displayId !== display.displayId)
      .concat(calibration);
    applyWindowSpatialization({ displayCalibrations });
  };

  const sourceRule = selectedSourceEntry?.rule ?? null;

  const updateSourceRule = (
    applicationId: string,
    source: WindowAudioSourceInfo | null,
    patch: Partial<WindowSourceRule>,
  ) => {
    const existingRule = windowSpatialization.sourceRules.find(
      (item) => item.applicationId.toLocaleLowerCase('en-US') ===
        applicationId.toLocaleLowerCase('en-US'),
    ) ?? null;
    if (!existingRule && windowSpatialization.sourceRules.length >= 64) {
      notify({
        tone: 'warning',
        title: 'Limite de règles atteinte',
        detail: 'Supprimez une règle existante avant d’en ajouter une nouvelle (maximum 64 applications).',
      });
      return;
    }
    const current: WindowSourceRule = existingRule ?? {
      applicationId,
      enabled: true,
      gainDb: source?.gainDb ?? 0,
      stereoSpread: windowSpatialization.stereoSpread,
      fallbackDisplayId: source?.displayId ?? null,
    };
    const rule: WindowSourceRule = {
      ...current,
      ...patch,
      gainDb: Math.max(-60, Math.min(12, patch.gainDb ?? current.gainDb)),
      stereoSpread: Math.max(0, Math.min(1, patch.stereoSpread ?? current.stereoSpread)),
    };
    applyWindowSpatialization({
      sourceRules: windowSpatialization.sourceRules
        .filter((item) => item.applicationId.toLocaleLowerCase('en-US') !==
          applicationId.toLocaleLowerCase('en-US'))
        .concat(rule),
    });
  };

  const removeSourceRule = (applicationId: string) => {
    const normalizedId = applicationId.toLocaleLowerCase('en-US');
    applyWindowSpatialization({
      sourceRules: windowSpatialization.sourceRules.filter(
        (item) => item.applicationId.toLocaleLowerCase('en-US') !== normalizedId,
      ),
    });
  };

  const updateSpeaker = (id: Channel, update: Partial<SpeakerConfig>) => {
    const speakers = scene.speakers.map((item) => (item.id === id ? { ...item, ...update } : item)) as SpeakerSet;
    patchScene({ speakers });
  };

  const updatePolar = (property: 'azimuth' | 'distance', value: number) => {
    const current = speakerPolarFromListener(speaker.position, scene.listener.position);
    const next = { ...current, [property]: value };
    const position = speakerPositionFromPolar(next, scene.listener.position, speaker.position.y);
    let speakers = scene.speakers.map((item) => (item.id === selected ? { ...item, position } : item)) as SpeakerSet;
    if (linked && pairedChannel) {
      const otherId = pairedChannel;
      speakers = speakers.map((item) =>
        item.id === otherId
          ? { ...item, position: { ...item.position, x: 2 * scene.listener.position.x - position.x, y: position.y, z: position.z } }
          : item,
      ) as SpeakerSet;
    }
    patchScene({ speakers });
  };

  const setInputLayout = (inputLayout: InputLayout) => {
    if (inputLayout === '5.1-surround' && surroundUnavailable) {
      notify({
        tone: 'warning',
        title: 'Source 5.1 indisponible',
        detail: scene.captureProvider !== 'external-render'
          ? 'Le mode 5.1 requiert une source externe explicitement sélectionnée.'
          : `${externalCapture?.name ?? 'La source externe'} expose ${externalCapture?.channelCount ?? 0} canaux (masque 0x${(externalCapture?.channelMask ?? 0).toString(16).toUpperCase()}). Il faut exactement 6 canaux avec un masque 0x3F ou 0x60F, sans upmix automatique.`,
      });
      return;
    }
    patchScene({ inputLayout });
  };

  const toggleFullscreen = async () => {
    try {
      if (isTauriRuntime()) {
        const { getCurrentWindow } = await import('@tauri-apps/api/window');
        const currentWindow = getCurrentWindow();
        await currentWindow.setFullscreen(!(await currentWindow.isFullscreen()));
      } else if (document.fullscreenElement) {
        await document.exitFullscreen();
      } else {
        await document.documentElement.requestFullscreen();
      }
    } catch (error) {
      notify({ tone: 'warning', title: 'Plein écran indisponible', detail: error instanceof Error ? error.message : String(error) });
    }
  };

  const setRoomDimension = (key: 'width' | 'length' | 'height', value: number) => {
    const position = { ...scene.listener.position };
    if (key === 'width') position.x = Math.max(-value / 2 + 0.1, Math.min(value / 2 - 0.1, position.x));
    if (key === 'length') position.z = Math.max(-value / 2 + 0.1, Math.min(value / 2 - 0.1, position.z));
    if (key === 'height') position.y = Math.max(0.4, Math.min(value - 0.1, position.y));
    patchScene({
      room: { ...scene.room, dimensions: { ...scene.room.dimensions, [key]: value } },
      listener: { ...scene.listener, position },
    });
  };

  const updateSurfaceBand = (property: 'absorption' | 'diffusion', band: AcousticBand, value: number) => {
    patchScene({ room: setSurfaceBand(scene.room, selectedSurface, property, band, value) });
  };

  const startTracking = async () => {
    try {
      await start();
      notify({ tone: 'success', title: 'Suivi facial actif', detail: 'Regardez droit devant vous puis calibrez la position neutre.' });
    } catch {
      // Le fournisseur a déjà publié une erreur détaillée.
    }
  };

  return (
    <div className="scene-page page-grid">
      <section className="scene-stage panel">
        <div className="scene-toolbar">
          <div className="scene-legend">
            {spatialInputMode === 'endpoint-mix' ? (
              <>
                <span><i className="legend-left" /> Enceintes routées</span>
                <span><i className="legend-unrouted" /> Non routées</span>
              </>
            ) : (
              <>
                <span><i className="legend-left" /> Canal gauche de la fenêtre</span>
                <span><i className="legend-right" /> Canal droit de la fenêtre</span>
              </>
            )}
            <span><i className="legend-listener" /> Point d’écoute</span>
          </div>
          <button type="button" className="ghost-button compact" onClick={() => void toggleFullscreen()}><Maximize2 size={15} /> Plein écran</button>
        </div>
        <div className="scene-canvas">
          <SpatialScene
            speakers={scene.speakers}
            listener={scene.listener}
            room={scene.room}
            pose={tracking.pose}
            inputLayout={scene.inputLayout}
            spatialInputMode={spatialInputMode}
            displays={visualDisplays}
            windowSources={windowSources}
            selectedSpeaker={selected}
            onSelectSpeaker={setSelected}
          />
          <div className="scene-hud scene-hud-top">
            <span className={`tracking-orb ${running ? 'is-live' : ''}`}><ScanFace size={17} /></span>
            <span>
              <small>ANCRAGE DE TÊTE</small>
              <strong>{running ? (tracking.pose?.trackingState === 'tracked' ? 'Verrouillé' : 'Recherche…') : 'Inactif'}</strong>
            </span>
          </div>
          <div className="scene-hud scene-hud-bottom">
            <span><Rotate3D size={14} /> Yaw <strong>{tracking.euler.yaw.toFixed(1)}°</strong></span>
            <span>Pitch <strong>{tracking.euler.pitch.toFixed(1)}°</strong></span>
            <span>Roll <strong>{tracking.euler.roll.toFixed(1)}°</strong></span>
          </div>
        </div>
        <div className="scene-footer">
          <div>
            <button type="button" className="primary-button" onClick={() => (running ? calibrate() : startTracking())}>
              {running ? <Crosshair size={17} /> : <Camera size={17} />}
              {running ? 'Recentrer face à moi' : 'Activer la caméra'}
            </button>
            <span className="keyboard-hint">ESPACE · RECENTRER</span>
          </div>
          {spatialInputMode === 'endpoint-mix' && (
            <div className="room-mix-compact">
              <Volume2 size={16} />
              <span>Direct</span>
              <input
                aria-label="Balance son direct et pièce"
                type="range"
                min={0}
                max={1}
                step={0.01}
                value={scene.directRoomMix}
                style={{ '--range-fill': `${scene.directRoomMix * 100}%` } as React.CSSProperties}
                onChange={(event) => patchScene({ directRoomMix: Number(event.target.value) })}
              />
              <span>Pièce</span>
            </div>
          )}
        </div>
      </section>

      <aside className="inspector panel">
        <div className="spatial-input-mode">
          <span className="eyebrow">ORIGINE DES SOURCES</span>
          <SegmentedControl<SpatialInputMode>
            ariaLabel="Mode de spatialisation"
            value={spatialInputMode}
            onChange={setSpatialInputMode}
            options={[
              { value: 'endpoint-mix', label: 'Enceintes', icon: <Radio size={15} /> },
              { value: 'process-windows', label: 'Fenêtres', icon: <AppWindow size={15} /> },
            ]}
          />
        </div>

        {!windowSpatialization.enabled && (
        <SegmentedControl
          ariaLabel="Panneau de configuration de la scène"
          value={mode}
          onChange={setMode}
          options={[
            { value: 'speakers', label: 'Enceintes', icon: <Radio size={15} /> },
            { value: 'room', label: 'Pièce', icon: <Ruler size={15} /> },
          ]}
        />
        )}
        {windowSpatialization.enabled && (
          <SegmentedControl<WindowInspectorMode>
            ariaLabel="Panneau de spatialisation des fenêtres"
            value={windowMode}
            onChange={setWindowMode}
            options={[
              { value: 'displays', label: 'Écrans', icon: <Monitor size={15} /> },
              { value: 'sources', label: 'Sources', icon: <AppWindow size={15} /> },
            ]}
          />
        )}

        {!windowSpatialization.enabled && (mode === 'speakers' ? (
          <>
            <div className="inspector-section">
              <div className="section-heading-row">
                <div>
                  <span className="eyebrow">IMPLANTATION VIRTUELLE</span>
                  <h2>Placement</h2>
                </div>
                <button
                  type="button"
                  className={`icon-button small ${linked ? 'is-highlighted' : ''}`}
                  onClick={() => setLinked((value) => !value)}
                  title={!pairedChannel ? 'Le canal central n’a pas de paire' : linked ? 'Dissocier la paire' : 'Lier symétriquement la paire'}
                  disabled={!pairedChannel}
                >
                  {linked ? <Link2 size={16} /> : <Unlink2 size={16} />}
                </button>
              </div>
              <div className="input-layout-control">
                <span className="eyebrow">FORMAT D’ENTRÉE</span>
                <SegmentedControl<InputLayout>
                  ariaLabel="Format des canaux d’entrée"
                  value={scene.inputLayout}
                  onChange={setInputLayout}
                  options={[
                    { value: 'stereo', label: 'Stéréo' },
                    {
                      value: '5.1-surround',
                      label: '5.1',
                      disabled: surroundUnavailable,
                      title: surroundUnavailable ? 'Le 5.1 requiert une source externe 6 canaux avec masque 0x3F ou 0x60F.' : undefined,
                    },
                  ]}
                />
                {surroundUnavailable && <small className="control-warning" role="status">5.1 disponible avec une source externe 6 canaux (masque 0x3F ou 0x60F) · aucun upmix.</small>}
              </div>
              <div className="speaker-selector">
                {scene.speakers.map((item) => (
                  <button key={item.id} type="button" className={`${selected === item.id ? 'is-selected' : ''} ${!isSpeakerRouted(scene.inputLayout, item.id) ? 'is-unrouted' : ''}`} onClick={() => setSelected(item.id)}>
                    <i className={`speaker-color speaker-${item.id.toLowerCase()}`} />
                    <span><strong>{item.id}</strong><small>{!isSpeakerRouted(scene.inputLayout, item.id) ? 'Non routée en stéréo' : item.label}</small></span>
                  </button>
                ))}
              </div>
              <RangeControl label="Azimut" value={geometry.azimuth} min={-180} max={180} step={1} unit="°" onChange={(value) => updatePolar('azimuth', value)} hint="Référence : L/R ±30°, C 0°, surrounds ±110°." />
              <RangeControl label="Distance" value={geometry.distance} min={0.7} max={5} step={0.1} unit="m" onChange={(value) => updatePolar('distance', value)} />
              <RangeControl
                label="Hauteur"
                value={speaker.position.y}
                min={0.6}
                max={2.2}
                step={0.05}
                unit="m"
                onChange={(value) => updateSpeaker(selected, { position: { ...speaker.position, y: value } })}
              />
              <RangeControl label="Gain" value={speaker.gainDb} min={-12} max={6} step={0.5} unit="dB" onChange={(value) => updateSpeaker(selected, { gainDb: value })} />
              <Toggle checked={!speaker.muted} onChange={(checked) => updateSpeaker(selected, { muted: !checked })} label={`Canal ${selected} actif`} />
              {scene.inputLayout === '5.1-surround' && (
                <div className="lfe-editor">
                  <span><small>LFE · EFFETS BASSE FRÉQUENCE</small><i>Canal non positionnel</i></span>
                  <RangeControl label="Gain LFE" value={scene.lfe.gainDb} min={-12} max={6} step={0.5} unit="dB" onChange={(gainDb) => patchScene({ lfe: { ...scene.lfe, gainDb } })} />
                  <Toggle checked={!scene.lfe.muted} onChange={(checked) => patchScene({ lfe: { ...scene.lfe, muted: !checked } })} label="Canal LFE actif" />
                </div>
              )}
            </div>
            <div className="inspector-note">
              <Sparkles size={16} />
              <p><strong>Conseil d’écoute</strong> Une distance de 2 m et un angle de ±30° offrent un point de départ naturel.</p>
            </div>
          </>
        ) : (
          <>
            <div className="inspector-section">
              <span className="eyebrow">AURALISATION</span>
              <h2>Salle rectangulaire</h2>
              <Toggle
                checked={scene.room.enabled}
                onChange={(enabled) => patchScene({ room: { ...scene.room, enabled } })}
                label="Activer la pièce"
                description="Réflexions précoces et champ diffus multibande"
              />
              <RangeControl label="Largeur" value={scene.room.dimensions.width} min={2.5} max={12} step={0.1} unit="m" onChange={(value) => setRoomDimension('width', value)} />
              <RangeControl label="Longueur" value={scene.room.dimensions.length} min={2.5} max={16} step={0.1} unit="m" onChange={(value) => setRoomDimension('length', value)} />
              <RangeControl label="Hauteur" value={scene.room.dimensions.height} min={2} max={5} step={0.1} unit="m" onChange={(value) => setRoomDimension('height', value)} />
              <div className="listener-position-editor">
                <span><small>POINT D’ÉCOUTE STATIQUE</small><i>Orientation 3DoF uniquement</i></span>
                <RangeControl label="Décalage gauche / droite" value={scene.listener.position.x} min={-scene.room.dimensions.width / 2 + 0.1} max={scene.room.dimensions.width / 2 - 0.1} step={0.05} unit="m" onChange={(x) => patchScene({ listener: { ...scene.listener, position: { ...scene.listener.position, x } } })} />
                <RangeControl label="Hauteur d’oreille" value={scene.listener.position.y} min={0.4} max={scene.room.dimensions.height - 0.1} step={0.05} unit="m" onChange={(y) => patchScene({ listener: { ...scene.listener, position: { ...scene.listener.position, y } } })} />
                <RangeControl label="Décalage arrière / avant" value={scene.listener.position.z} min={-scene.room.dimensions.length / 2 + 0.1} max={scene.room.dimensions.length / 2 - 0.1} step={0.05} unit="m" onChange={(z) => patchScene({ listener: { ...scene.listener, position: { ...scene.listener.position, z } } })} />
              </div>
              <div className="reflection-order-control">
                <span><small>ORDRE DES RÉFLEXIONS</small><i>Coût / précision</i></span>
                <SegmentedControl<ReflectionOrderOption>
                  ariaLabel="Ordre maximal des sources-images"
                  value={String(scene.room.earlyReflectionOrder) as ReflectionOrderOption}
                  onChange={(value) => patchScene({ room: { ...scene.room, earlyReflectionOrder: Number(value) as 0 | 1 | 2 } })}
                  options={[
                    { value: '0', label: '0 · Direct' },
                    { value: '1', label: '1 · Léger' },
                    { value: '2', label: '2 · Détaillé' },
                  ]}
                />
              </div>
              <RangeControl
                label="Fenêtre précoce"
                value={scene.room.earlyWindowMs}
                min={20}
                max={80}
                step={5}
                unit="ms"
                onChange={(value) => patchScene({ room: { ...scene.room, earlyWindowMs: value } })}
              />
              <Toggle
                checked={scene.room.lateReverbEnabled}
                onChange={(lateReverbEnabled) => patchScene({ room: { ...scene.room, lateReverbEnabled } })}
                label="Réverbération tardive"
                description="FDN multibande, 16 lignes de retard"
                disabled={!scene.room.enabled}
              />
              <div className="surface-editor">
                <div className="surface-editor-heading">
                  <span><small>SURFACES</small><strong>{ROOM_SURFACES.find((item) => item.key === selectedSurface)?.label}</strong></span>
                  <i>Valeurs indicatives</i>
                </div>
                <div className="surface-selector" aria-label="Surface acoustique à éditer">
                  {ROOM_SURFACES.map((item) => (
                    <button key={item.key} type="button" className={selectedSurface === item.key ? 'is-selected' : ''} onClick={() => setSelectedSurface(item.key)}>
                      <strong>{item.axis}</strong><span>{item.label.replace('Mur ', '')}</span>
                    </button>
                  ))}
                </div>
                <label className="material-select">
                  <span>Matériau indicatif</span>
                  <select value={MATERIAL_PRESETS.some((preset) => preset.id === surface.materialId) ? surface.materialId : ''} onChange={(event) => patchScene({ room: applyMaterialPreset(scene.room, selectedSurface, event.target.value) })}>
                    <option value="" disabled>Personnalisé</option>
                    {MATERIAL_PRESETS.map((preset) => <option key={preset.id} value={preset.id}>{preset.label}</option>)}
                  </select>
                </label>
                <AcousticBandControls title="Absorption" values={surface.absorption} onChange={(band, value) => updateSurfaceBand('absorption', band, value)} />
                <AcousticBandControls title="Diffusion" values={surface.diffusion} onChange={(band, value) => updateSurfaceBand('diffusion', band, value)} />
              </div>
            </div>
            <div className="inspector-note subtle">
              <p><strong>Modèle perceptuel</strong> Cette vue fournit une auralisation plausible, pas une prédiction acoustique certifiable.</p>
            </div>
          </>
        ))}

        {windowSpatialization.enabled && (
          <>
            <div className="inspector-section window-spatialization-editor">
              <div className="window-runtime-summary">
                <span className={`runtime-dot ${engine.windowAudio.running ? 'is-live' : ''}`} />
                <span>
                  <small>CAPTURE PAR APPLICATION</small>
                  <strong>
                    {!engine.windowAudio.supported
                      ? 'En attente du moteur compatible'
                      : engine.windowAudio.running && engine.windowAudio.sourceCount > 0 && windowRenderingEffective
                        ? `${engine.windowAudio.sourceCount} source${engine.windowAudio.sourceCount > 1 ? 's' : ''} active${engine.windowAudio.sourceCount > 1 ? 's' : ''}`
                        : engine.windowAudio.running && engine.windowAudio.sourceCount > 0
                          ? 'Mix global de sécurité'
                        : engine.windowAudio.running
                          ? 'En attente d’une application audio'
                        : 'Prête à démarrer'}
                  </strong>
                </span>
              </div>
              {engine.windowAudio.lastError && (
                <small className="control-warning" role="status">{engine.windowAudio.lastError}</small>
              )}
              {windowEndpointFallback && !engine.windowAudio.lastError && (
                <small className="control-warning" role="status">
                  Séparation incomplète ou en cours : le mix global reste audible afin de ne couper aucune source.
                </small>
              )}
              <Toggle
                checked={windowSpatialization.followWindowPosition}
                onChange={(followWindowPosition) => applyWindowSpatialization({ followWindowPosition })}
                label="Suivre les déplacements"
                description="La paire stéréo suit le centre et la largeur de chaque fenêtre."
              />
              <div className="control-stack">
                <span className="control-label">Placement des émetteurs L/R</span>
                <SegmentedControl
                  value={windowSpatialization.emitterPlacementMode}
                  options={[
                    { value: 'proportional', label: 'Largeur réglable' },
                    { value: 'window-edges', label: 'Bords de fenêtre' },
                  ]}
                  onChange={(emitterPlacementMode) => applyWindowSpatialization({ emitterPlacementMode })}
                  ariaLabel="Placement des émetteurs stéréo de chaque fenêtre"
                />
                <small className="control-hint">
                  Chaque bord est projeté indépendamment sur son écran lorsque la fenêtre chevauche plusieurs moniteurs.
                </small>
              </div>
              <RangeControl
                label="Sources simultanées"
                value={windowSpatialization.maxSources}
                min={1}
                max={8}
                step={1}
                unit=""
                onChange={(maxSources) => applyWindowSpatialization({ maxSources })}
              />
              {windowSpatialization.emitterPlacementMode === 'proportional' && (
                <RangeControl
                  label="Largeur stéréo par défaut"
                  value={windowSpatialization.stereoSpread}
                  min={0}
                  max={1}
                  step={0.05}
                  unit=""
                  onChange={(stereoSpread) => applyWindowSpatialization({ stereoSpread })}
                  hint="0 centre les deux canaux ; 1 les place sur les bords de la fenêtre."
                />
              )}

              {windowMode === 'displays' ? (
                <>
                  <div className="section-heading-row window-section-heading">
                    <div>
                      <span className="eyebrow">AGENCEMENT WINDOWS</span>
                      <h2>Écrans physiques</h2>
                    </div>
                    <span className="window-count">{displays.length}</span>
                  </div>
                  {displays.length === 0 ? (
                    <div className="window-empty-state">
                      <Monitor size={22} />
                      <strong>Aucun écran reçu</strong>
                      <small>Le moteur publiera ici l’agencement retourné par Windows.</small>
                    </div>
                  ) : (
                    <>
                      <div className="display-selector" aria-label="Écran à calibrer">
                        {displays.map((display) => (
                          <button
                            key={display.displayId}
                            type="button"
                            className={selectedDisplay?.displayId === display.displayId ? 'is-selected' : ''}
                            onClick={() => setSelectedDisplayId(display.displayId)}
                          >
                            <Monitor size={14} />
                            <span>
                              <strong>{display.name}</strong>
                              <small>
                                {display.boundsPx.width}×{display.boundsPx.height}
                                {display.isPrimary ? ' · Principal' : ''}
                              </small>
                            </span>
                            <i>{display.calibrated || windowSpatialization.displayCalibrations.some((item) => item.displayId === display.displayId) ? 'CAL' : 'AUTO'}</i>
                          </button>
                        ))}
                      </div>
                      {selectedDisplay && (
                        <div className="display-calibration">
                          <span>
                            <small>PLAN PHYSIQUE</small>
                            <button
                              type="button"
                              className="text-link"
                              onClick={() => applyWindowSpatialization({
                                displayCalibrations: windowSpatialization.displayCalibrations
                                  .filter((item) => item.displayId !== selectedDisplay.displayId),
                              })}
                            >
                              Réinitialiser
                            </button>
                          </span>
                          <RangeControl
                            label="Décalage horizontal"
                            value={(displayCalibration ?? selectedDisplay).center.x}
                            min={-5}
                            max={5}
                            step={0.02}
                            unit="m"
                            onChange={(x) => updateDisplayCalibration(selectedDisplay, {
                              center: { ...ensureDisplayCalibration(selectedDisplay).center, x },
                            })}
                          />
                          <RangeControl
                            label="Hauteur du centre"
                            value={(displayCalibration ?? selectedDisplay).center.y}
                            min={0.3}
                            max={3}
                            step={0.02}
                            unit="m"
                            onChange={(y) => updateDisplayCalibration(selectedDisplay, {
                              center: { ...ensureDisplayCalibration(selectedDisplay).center, y },
                            })}
                          />
                          <RangeControl
                            label="Distance"
                            value={(displayCalibration ?? selectedDisplay).center.z}
                            min={0.4}
                            max={5}
                            step={0.02}
                            unit="m"
                            onChange={(z) => updateDisplayCalibration(selectedDisplay, {
                              center: { ...ensureDisplayCalibration(selectedDisplay).center, z },
                            })}
                          />
                          <RangeControl
                            label="Angle horizontal"
                            value={displayYawDegrees(
                              (displayCalibration ?? selectedDisplay).orientation,
                            )}
                            min={-90}
                            max={90}
                            step={1}
                            unit="°"
                            onChange={(yaw) => updateDisplayCalibration(selectedDisplay, {
                              orientation: displayOrientationFromYaw(yaw),
                            })}
                          />
                          <RangeControl
                            label="Largeur physique"
                            value={(displayCalibration ?? selectedDisplay).widthM}
                            min={0.2}
                            max={3}
                            step={0.01}
                            unit="m"
                            onChange={(widthM) => updateDisplayCalibration(selectedDisplay, { widthM })}
                          />
                          <RangeControl
                            label="Hauteur physique"
                            value={(displayCalibration ?? selectedDisplay).heightM}
                            min={0.15}
                            max={2}
                            step={0.01}
                            unit="m"
                            onChange={(heightM) => updateDisplayCalibration(selectedDisplay, { heightM })}
                          />
                        </div>
                      )}
                    </>
                  )}
                </>
              ) : (
                <>
                  <div className="section-heading-row window-section-heading">
                    <div>
                      <span className="eyebrow">SESSIONS / PROCESSUS STÉRÉO</span>
                      <h2>Sources audio</h2>
                    </div>
                    <span className="window-count">{windowSources.length}/{windowSpatialization.maxSources}</span>
                  </div>
                  {windowSourceEntries.length === 0 ? (
                    <div className="window-empty-state">
                      <AppWindow size={22} />
                      <strong>Aucune source audio</strong>
                      <small>Lancez un son dans une application visible pour créer sa paire L/R.</small>
                    </div>
                  ) : (
                    <>
                      <div className="window-source-selector" aria-label="Source audio à configurer">
                        {windowSourceEntries.map((entry) => (
                          <button
                            key={entry.key}
                            type="button"
                            className={selectedSourceEntry?.key === entry.key ? 'is-selected' : ''}
                            onClick={() => setSelectedSourceId(entry.key)}
                          >
                            <span className={`runtime-dot ${entry.source?.active ? 'is-live' : ''}`} />
                            <span>
                              <strong>
                                {entry.source?.applicationName
                                  || entry.applicationId
                                  || (entry.source ? `PID ${entry.source.processId}` : 'Application mémorisée')}
                              </strong>
                              <small>
                                {entry.source
                                  ? `${entry.source.windowTitle || 'Fenêtre principale'} · L/R`
                                  : entry.rule?.enabled
                                    ? 'Hors ligne · règle mémorisée'
                                    : 'Désactivée · règle mémorisée'}
                              </small>
                            </span>
                          </button>
                        ))}
                      </div>
                      {selectedSourceEntry && (
                        <div className="source-rule-editor">
                          <Toggle
                            checked={sourceRule?.enabled ?? true}
                            onChange={(enabled) => updateSourceRule(
                              selectedSourceEntry.applicationId,
                              selectedSource,
                              { enabled },
                            )}
                            label="Spatialiser cette session audio"
                            description={selectedSource
                              ? `Arbre de processus · PID ${selectedSource.processId}`
                              : selectedSourceEntry.applicationId}
                          />
                          <RangeControl
                            label="Gain"
                            value={sourceRule?.gainDb ?? selectedSource?.gainDb ?? 0}
                            min={-60}
                            max={12}
                            step={0.5}
                            unit="dB"
                            onChange={(gainDb) => updateSourceRule(
                              selectedSourceEntry.applicationId,
                              selectedSource,
                              { gainDb },
                            )}
                          />
                          {windowSpatialization.emitterPlacementMode === 'proportional' && (
                            <RangeControl
                              label="Écartement L/R"
                              value={sourceRule?.stereoSpread ?? windowSpatialization.stereoSpread}
                              min={0}
                              max={1}
                              step={0.05}
                              unit=""
                              onChange={(stereoSpread) => updateSourceRule(
                                selectedSourceEntry.applicationId,
                                selectedSource,
                                { stereoSpread },
                              )}
                            />
                          )}
                          <label className="material-select">
                            <span>Écran de repli</span>
                            <select
                              value={sourceRule?.fallbackDisplayId ?? selectedSource?.displayId ?? ''}
                              onChange={(event) => updateSourceRule(
                                selectedSourceEntry.applicationId,
                                selectedSource,
                                { fallbackDisplayId: event.target.value || null },
                              )}
                            >
                              <option value="">Placement frontal automatique</option>
                              {displays.map((display) => (
                                <option key={display.displayId} value={display.displayId}>{display.name}</option>
                              ))}
                            </select>
                          </label>
                          {sourceRule && (
                            <div className="source-rule-actions">
                              <button
                                type="button"
                                className="text-link danger-link"
                                onClick={() => removeSourceRule(selectedSourceEntry.applicationId)}
                              >
                                <Trash2 size={13} />
                                Supprimer la règle
                              </button>
                            </div>
                          )}
                        </div>
                      )}
                    </>
                  )}
                </>
              )}
            </div>
            <div className="inspector-note">
              <Sparkles size={16} />
              <p><strong>Stéréo conservée</strong> Chaque session et son arbre de processus utilisent deux émetteurs HRTF, placés sur la largeur de la fenêtre associée.</p>
            </div>
          </>
        )}
      </aside>
    </div>
  );
}

function AcousticBandControls({ title, values, onChange }: {
  title: string;
  values: [number, number, number];
  onChange: (band: AcousticBand, value: number) => void;
}) {
  const labels = ['Grave', 'Médium', 'Aigu'] as const;
  return (
    <fieldset className="acoustic-bands">
      <legend>{title}</legend>
      {labels.map((label, index) => (
        <RangeControl
          key={label}
          label={label}
          value={values[index]}
          min={0}
          max={1}
          step={0.01}
          unit=""
          onChange={(value) => onChange(index as AcousticBand, value)}
        />
      ))}
    </fieldset>
  );
}
