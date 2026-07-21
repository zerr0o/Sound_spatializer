import { describe, expect, it } from 'vitest';
import type { Quaternion } from '../types/contracts';
import { eulerFromQuaternion, identityQuaternion } from './pose-math';
import { captureTrackingLossOrigin, resolveMissingTrackingPose } from './tracking-loss-transition';

const yaw = (degrees: number): Quaternion => {
  const halfAngle = degrees * Math.PI / 360;
  return { x: 0, y: Math.sin(halfAngle), z: 0, w: Math.cos(halfAngle) };
};

describe('transition lors de la perte du visage', () => {
  it('signale lost au démarrage sans pose précédente', () => {
    expect(resolveMissingTrackingPose(null, 0)).toEqual({
      quaternion: identityQuaternion,
      trackingState: 'lost',
    });
  });

  it('ne transforme jamais l’identité synthétique de démarrage en origine de perte', () => {
    const firstMissing = resolveMissingTrackingPose(null, 0);
    const origin = captureTrackingLossOrigin(firstMissing.quaternion, firstMissing.trackingState);
    const secondMissing = resolveMissingTrackingPose(origin, 33);

    expect(origin).toBeNull();
    expect(secondMissing.trackingState).toBe('lost');
  });

  it('capture la dernière pose uniquement après un vrai suivi', () => {
    const trackedPose = yaw(25);

    expect(captureTrackingLossOrigin(trackedPose, 'tracked')).toEqual(trackedPose);
    expect(captureTrackingLossOrigin(trackedPose, 'held')).toBeNull();
    expect(captureTrackingLossOrigin(trackedPose, 'returning')).toBeNull();
    expect(captureTrackingLossOrigin(trackedPose, 'lost')).toBeNull();
  });

  it('fige la pose pendant 150 ms', () => {
    const origin = yaw(60);
    expect(resolveMissingTrackingPose(origin, 150)).toEqual({ quaternion: origin, trackingState: 'held' });
  });

  it('interpole toujours depuis l’origine figée avec une progression absolue', () => {
    const origin = yaw(60);
    const halfway = resolveMissingTrackingPose(origin, 650);
    const threeQuarters = resolveMissingTrackingPose(origin, 900);

    expect(halfway.trackingState).toBe('returning');
    expect(eulerFromQuaternion(halfway.quaternion).yaw).toBeCloseTo(30, 5);
    expect(eulerFromQuaternion(threeQuarters.quaternion).yaw).toBeCloseTo(15, 5);
  });

  it('atteint exactement le neutre après 1,15 s', () => {
    expect(resolveMissingTrackingPose(yaw(60), 1_150)).toEqual({
      quaternion: identityQuaternion,
      trackingState: 'lost',
    });
  });
});
