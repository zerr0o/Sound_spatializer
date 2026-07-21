import { describe, expect, it } from 'vitest';
import { faceTrackingConfidence } from './face-confidence';

const identityMatrix = [
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
];

describe('confiance Face Landmarker', () => {
  it('considère une matrice et des landmarks valides comme une détection fiable même si visibility vaut zéro', () => {
    expect(faceTrackingConfidence(identityMatrix, [{ visibility: 0 }, { visibility: 0 }])).toBe(1);
  });

  it('ne dépend pas de la présence du champ visibility', () => {
    expect(faceTrackingConfidence(identityMatrix, [{}, {}])).toBe(1);
  });

  it('rejette une sortie incomplète ou une matrice non finie', () => {
    expect(faceTrackingConfidence(null, [{ visibility: 1 }])).toBe(0);
    expect(faceTrackingConfidence(identityMatrix, [])).toBe(0);
    expect(faceTrackingConfidence([...identityMatrix.slice(0, 15), Number.NaN], [{}])).toBe(0);
  });
});
