import { describe, expect, it } from 'vitest';
import { speakerPolarFromListener, speakerPositionFromPolar } from './scene-geometry';

describe('géométrie de scène relative à l’auditeur', () => {
  it('préserve angle et distance lorsque le point d’écoute est décalé', () => {
    const listener = { x: 1.25, y: 1.1, z: -0.75 };
    const position = speakerPositionFromPolar({ azimuth: -30, distance: 2 }, listener, 1.2);
    expect(position.x).toBeCloseTo(0.25, 6);
    expect(position.z).toBeCloseTo(-0.75 + Math.sqrt(3), 6);
    expect(speakerPolarFromListener(position, listener).azimuth).toBeCloseTo(-30, 6);
    expect(speakerPolarFromListener(position, listener).distance).toBeCloseTo(2, 6);
  });
});
