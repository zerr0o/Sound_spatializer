import { useMemo, useState } from 'react';
import { Camera, Crosshair, Link2, Maximize2, Radio, Rotate3D, Ruler, ScanFace, Sparkles, Unlink2, Volume2 } from 'lucide-react';
import { SpatialScene } from '../components/scene/SpatialScene';
import { RangeControl, SegmentedControl, Toggle } from '../components/ui/Controls';
import { applyMaterialPreset, MATERIAL_PRESETS, ROOM_SURFACES, setSurfaceBand, type AcousticBand, type RoomSurfaceKey } from '../lib/room-acoustics';
import { speakerPolarFromListener, speakerPositionFromPolar } from '../lib/scene-geometry';
import { isTauriRuntime } from '../lib/tauri-bridge';
import { useAppStore } from '../store/app-store';
import { useTrackingController } from '../tracking/TrackingProvider';
import type { Channel, SpeakerConfig } from '../types/contracts';

type InspectorMode = 'speakers' | 'room';
type ReflectionOrderOption = '0' | '1' | '2';

export function ScenePage() {
  const scene = useAppStore((state) => state.scene);
  const tracking = useAppStore((state) => state.tracking);
  const patchScene = useAppStore((state) => state.patchScene);
  const notify = useAppStore((state) => state.notify);
  const { start, calibrate, running } = useTrackingController();
  const [selected, setSelected] = useState<Channel>('L');
  const [linked, setLinked] = useState(true);
  const [mode, setMode] = useState<InspectorMode>('speakers');
  const [selectedSurface, setSelectedSurface] = useState<RoomSurfaceKey>('front');
  const speaker = scene.speakers.find((item) => item.id === selected) ?? scene.speakers[0];
  const geometry = useMemo(
    () => speakerPolarFromListener(speaker.position, scene.listener.position),
    [scene.listener.position, speaker.position],
  );
  const surface = scene.room.surfaces[selectedSurface];

  const updateSpeaker = (id: Channel, update: Partial<SpeakerConfig>) => {
    const speakers = scene.speakers.map((item) => (item.id === id ? { ...item, ...update } : item)) as [SpeakerConfig, SpeakerConfig];
    patchScene({ speakers });
  };

  const updatePolar = (property: 'azimuth' | 'distance', value: number) => {
    const current = speakerPolarFromListener(speaker.position, scene.listener.position);
    const next = { ...current, [property]: value };
    const position = speakerPositionFromPolar(next, scene.listener.position, speaker.position.y);
    let speakers = scene.speakers.map((item) => (item.id === selected ? { ...item, position } : item)) as [SpeakerConfig, SpeakerConfig];
    if (linked) {
      const otherId: Channel = selected === 'L' ? 'R' : 'L';
      speakers = speakers.map((item) =>
        item.id === otherId
          ? { ...item, position: { ...item.position, x: 2 * scene.listener.position.x - position.x, y: position.y, z: position.z } }
          : item,
      ) as [SpeakerConfig, SpeakerConfig];
    }
    patchScene({ speakers });
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
            <span><i className="legend-left" /> Canal gauche</span>
            <span><i className="legend-right" /> Canal droit</span>
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
        </div>
      </section>

      <aside className="inspector panel">
        <SegmentedControl
          ariaLabel="Panneau de configuration de la scène"
          value={mode}
          onChange={setMode}
          options={[
            { value: 'speakers', label: 'Enceintes', icon: <Radio size={15} /> },
            { value: 'room', label: 'Pièce', icon: <Ruler size={15} /> },
          ]}
        />

        {mode === 'speakers' ? (
          <>
            <div className="inspector-section">
              <div className="section-heading-row">
                <div>
                  <span className="eyebrow">GÉOMÉTRIE STÉRÉO</span>
                  <h2>Placement</h2>
                </div>
                <button
                  type="button"
                  className={`icon-button small ${linked ? 'is-highlighted' : ''}`}
                  onClick={() => setLinked((value) => !value)}
                  title={linked ? 'Dissocier les enceintes' : 'Lier symétriquement'}
                >
                  {linked ? <Link2 size={16} /> : <Unlink2 size={16} />}
                </button>
              </div>
              <div className="speaker-selector">
                {scene.speakers.map((item) => (
                  <button key={item.id} type="button" className={selected === item.id ? 'is-selected' : ''} onClick={() => setSelected(item.id)}>
                    <i className={`speaker-color speaker-${item.id.toLowerCase()}`} />
                    <span><strong>{item.id}</strong><small>{item.label}</small></span>
                  </button>
                ))}
              </div>
              <RangeControl label="Azimut" value={geometry.azimuth} min={-75} max={75} step={1} unit="°" onChange={(value) => updatePolar('azimuth', value)} hint="±30° correspond au triangle stéréo ITU-R." />
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
