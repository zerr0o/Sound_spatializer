import { describe, expect, it } from 'vitest';
import {
  angularVelocity,
  calibrateQuaternion,
  cameraPoseToAudioPose,
  eulerFromQuaternion,
  identityQuaternion,
  quaternionFromMediaPipeMatrix,
} from './pose-math';

const yawQuaternion = (degrees: number) => {
  const half = degrees * Math.PI / 360;
  return { x: 0, y: Math.sin(half), z: 0, w: Math.cos(half) };
};

describe('conventions de pose', () => {
  it('convertit la matrice identité MediaPipe en pose neutre', () => {
    const matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    expect(quaternionFromMediaPipeMatrix(matrix)).toEqual(identityQuaternion);
  });

  it('convertit une rotation caméra vers un yaw audio positif quand la tête va à droite', () => {
    // Une rotation droite de l'utilisateur apparaît négative autour de Y caméra.
    const camera = yawQuaternion(-30);
    const audio = cameraPoseToAudioPose(camera);
    expect(eulerFromQuaternion(audio).yaw).toBeCloseTo(30, 5);
  });

  it('exprime la vitesse angulaire dans le monde calibré', () => {
    const velocity = angularVelocity(identityQuaternion, yawQuaternion(10), 0.1);
    expect(velocity.x).toBeCloseTo(0, 6);
    expect(velocity.y).toBeCloseTo((10 * Math.PI / 180) / 0.1, 5);
    expect(velocity.z).toBeCloseTo(0, 6);
  });

  it('ne produit aucun pic quand q change seulement de signe', () => {
    const pose = yawQuaternion(35);
    const velocity = angularVelocity(pose, { x: -pose.x, y: -pose.y, z: -pose.z, w: -pose.w }, 1 / 60);
    expect(Math.hypot(velocity.x, velocity.y, velocity.z)).toBeLessThan(1e-6);
  });

  it('fait de la référence calibrée la pose identité', () => {
    const reference = yawQuaternion(18);
    const calibrated = calibrateQuaternion(reference, reference);
    expect(calibrated.x).toBeCloseTo(0, 6);
    expect(calibrated.y).toBeCloseTo(0, 6);
    expect(calibrated.z).toBeCloseTo(0, 6);
    expect(Math.abs(calibrated.w)).toBeCloseTo(1, 6);
  });
});
