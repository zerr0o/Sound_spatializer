import type { Quaternion, TrackingState } from '../types/contracts';
import { identityQuaternion, normalizeQuaternion } from './pose-math';

export const TRACKING_HOLD_MS = 150;
export const TRACKING_NEUTRAL_RETURN_MS = 1_000;

const slerpToIdentity = (input: Quaternion, amount: number): Quaternion => {
  let q = normalizeQuaternion(input);
  if (q.w < 0) q = { x: -q.x, y: -q.y, z: -q.z, w: -q.w };
  const dot = Math.max(-1, Math.min(1, q.w));
  if (dot > 0.9995) {
    return normalizeQuaternion({ x: q.x * (1 - amount), y: q.y * (1 - amount), z: q.z * (1 - amount), w: q.w + (1 - q.w) * amount });
  }
  const theta = Math.acos(dot);
  const sinTheta = Math.sin(theta);
  const a = Math.sin((1 - amount) * theta) / sinTheta;
  const b = Math.sin(amount * theta) / sinTheta;
  return normalizeQuaternion({ x: q.x * a, y: q.y * a, z: q.z * a, w: q.w * a + b });
};

export interface MissingTrackingPose {
  quaternion: Quaternion;
  trackingState: TrackingState;
}

/** Only a genuinely tracked pose may become the fixed loss origin. */
export const captureTrackingLossOrigin = (
  previousPose: Quaternion | null,
  previousState: TrackingState,
): Quaternion | null => previousState === 'tracked' ? previousPose : null;

/** Resolve a missing frame from the pose frozen at the start of the loss. */
export const resolveMissingTrackingPose = (
  lossOrigin: Quaternion | null,
  absentForMs: number,
): MissingTrackingPose => {
  if (!lossOrigin) return { quaternion: identityQuaternion, trackingState: 'lost' };
  if (absentForMs <= TRACKING_HOLD_MS) return { quaternion: lossOrigin, trackingState: 'held' };
  if (absentForMs < TRACKING_HOLD_MS + TRACKING_NEUTRAL_RETURN_MS) {
    return {
      quaternion: slerpToIdentity(lossOrigin, (absentForMs - TRACKING_HOLD_MS) / TRACKING_NEUTRAL_RETURN_MS),
      trackingState: 'returning',
    };
  }
  return { quaternion: identityQuaternion, trackingState: 'lost' };
};
