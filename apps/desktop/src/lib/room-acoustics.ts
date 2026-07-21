import type { RoomConfig, SurfaceAcoustics } from '../types/contracts';

export type RoomSurfaceKey = keyof RoomConfig['surfaces'];
export type AcousticBand = 0 | 1 | 2;

export const ROOM_SURFACES: { key: RoomSurfaceKey; label: string; axis: string }[] = [
  { key: 'left', label: 'Mur gauche', axis: '−X' },
  { key: 'right', label: 'Mur droit', axis: '+X' },
  { key: 'rear', label: 'Mur arrière', axis: '−Z' },
  { key: 'front', label: 'Mur avant', axis: '+Z' },
  { key: 'floor', label: 'Sol', axis: '−Y' },
  { key: 'ceiling', label: 'Plafond', axis: '+Y' },
];

export const MATERIAL_PRESETS: { id: string; label: string; acoustics: SurfaceAcoustics }[] = [
  { id: 'plaster', label: 'Plâtre peint', acoustics: { materialId: 'plaster', absorption: [0.1, 0.05, 0.04], diffusion: [0.05, 0.05, 0.05] } },
  { id: 'wood', label: 'Parquet bois', acoustics: { materialId: 'wood', absorption: [0.15, 0.11, 0.1], diffusion: [0.1, 0.1, 0.1] } },
  { id: 'carpet', label: 'Moquette épaisse', acoustics: { materialId: 'carpet', absorption: [0.12, 0.38, 0.62], diffusion: [0.04, 0.06, 0.08] } },
  { id: 'curtain', label: 'Rideau lourd', acoustics: { materialId: 'curtain', absorption: [0.35, 0.55, 0.72], diffusion: [0.05, 0.05, 0.05] } },
  { id: 'acoustic-panel', label: 'Panneau poreux', acoustics: { materialId: 'acoustic-panel', absorption: [0.3, 0.75, 0.9], diffusion: [0.03, 0.04, 0.05] } },
];

const clamp01 = (value: number) => Math.max(0, Math.min(1, Number.isFinite(value) ? value : 0));

export const setSurfaceBand = (
  room: RoomConfig,
  surfaceKey: RoomSurfaceKey,
  property: 'absorption' | 'diffusion',
  band: AcousticBand,
  value: number,
): RoomConfig => {
  const surface = room.surfaces[surfaceKey];
  const bands = [...surface[property]] as [number, number, number];
  bands[band] = clamp01(value);
  return {
    ...room,
    surfaces: {
      ...room.surfaces,
      [surfaceKey]: { ...surface, [property]: bands },
    },
  };
};

export const applyMaterialPreset = (room: RoomConfig, surfaceKey: RoomSurfaceKey, presetId: string): RoomConfig => {
  const preset = MATERIAL_PRESETS.find((entry) => entry.id === presetId);
  if (!preset) return room;
  return {
    ...room,
    surfaces: {
      ...room.surfaces,
      [surfaceKey]: {
        ...preset.acoustics,
        absorption: [...preset.acoustics.absorption],
        diffusion: [...preset.acoustics.diffusion],
      },
    },
  };
};
