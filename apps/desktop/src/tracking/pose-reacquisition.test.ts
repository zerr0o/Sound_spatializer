import { describe, expect, it } from 'vitest';
import type { Quaternion } from '../types/contracts';
import { PoseReacquisitionGuard } from './pose-reacquisition';

const yaw = (degrees: number): Quaternion => {
  const halfAngle = degrees * Math.PI / 360;
  return { x: 0, y: Math.sin(halfAngle), z: 0, w: Math.cos(halfAngle) };
};

describe('confirmation de réacquisition', () => {
  it('ne ralentit ni le suivi continu ni une occlusion très brève', () => {
    const guard = new PoseReacquisitionGuard();

    expect(guard.update(yaw(1))).toEqual(yaw(1));
    expect(guard.update(null)).toBeNull();
    expect(guard.update(null)).toBeNull();
    expect(guard.update(yaw(4))).toEqual(yaw(4));
  });

  it('demande deux poses cohérentes après une perte complète', () => {
    const guard = new PoseReacquisitionGuard();
    guard.update(yaw(0));
    guard.update(null);
    guard.update(null);
    guard.update(null);

    expect(guard.update(yaw(20))).toBeNull();
    expect(guard.update(yaw(22))).toEqual(yaw(22));
    expect(guard.update(yaw(23))).toEqual(yaw(23));
  });

  it('rejette le bruit incohérent jusqu’au retour de deux poses stables', () => {
    const guard = new PoseReacquisitionGuard();
    guard.update(null);
    guard.update(null);
    guard.update(null);

    expect(guard.update(yaw(-80))).toBeNull();
    expect(guard.update(yaw(70))).toBeNull();
    expect(guard.update(yaw(-45))).toBeNull();
    expect(guard.update(yaw(-43))).toEqual(yaw(-43));
  });

  it('exige des confirmations consécutives', () => {
    const guard = new PoseReacquisitionGuard();
    guard.update(null);
    guard.update(null);
    guard.update(null);

    expect(guard.update(yaw(12))).toBeNull();
    expect(guard.update(null)).toBeNull();
    expect(guard.update(yaw(13))).toBeNull();
    expect(guard.update(yaw(14))).toEqual(yaw(14));
  });

  it('réinitialise son cycle lors d’un redémarrage caméra', () => {
    const guard = new PoseReacquisitionGuard();
    guard.update(null);
    guard.update(null);
    guard.update(null);
    guard.update(yaw(10));

    guard.reset();
    expect(guard.update(yaw(15))).toEqual(yaw(15));
  });

  it('traite un quaternion non fini comme une perte', () => {
    const guard = new PoseReacquisitionGuard();
    const invalid = { x: Number.NaN, y: 0, z: 0, w: 1 };

    expect(guard.update(invalid)).toBeNull();
  });
});
