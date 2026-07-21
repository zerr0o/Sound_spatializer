import { describe, expect, it } from 'vitest';
import { defaultScene } from '../data/defaults';
import { applyMaterialPreset, setSurfaceBand } from './room-acoustics';

describe('édition acoustique des surfaces', () => {
  it('ne modifie que la bande et la surface demandées', () => {
    const original = structuredClone(defaultScene.room);
    const updated = setSurfaceBand(original, 'left', 'diffusion', 1, 0.72);
    expect(updated.surfaces.left.diffusion).toEqual([0.05, 0.72, 0.05]);
    expect(updated.surfaces.right).toEqual(original.surfaces.right);
    expect(original.surfaces.left.diffusion).toEqual([0.05, 0.05, 0.05]);
  });

  it('applique un preset par copie et borne les valeurs manuelles', () => {
    const preset = applyMaterialPreset(defaultScene.room, 'floor', 'carpet');
    expect(preset.surfaces.floor.materialId).toBe('carpet');
    expect(preset.surfaces.floor.absorption).toEqual([0.12, 0.38, 0.62]);
    expect(setSurfaceBand(preset, 'floor', 'absorption', 0, 2).surfaces.floor.absorption[0]).toBe(1);
  });
});
