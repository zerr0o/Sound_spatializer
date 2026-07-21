import type { EulerAngles, Quaternion, Vector3 } from '../types/contracts';

const EPSILON = 1e-8;

export const normalizeQuaternion = (q: Quaternion): Quaternion => {
  const length = Math.hypot(q.x, q.y, q.z, q.w);
  if (length < EPSILON) return { x: 0, y: 0, z: 0, w: 1 };
  const clean = (value: number) => Math.abs(value) < EPSILON ? 0 : value;
  return { x: clean(q.x / length), y: clean(q.y / length), z: clean(q.z / length), w: clean(q.w / length) };
};

export const isFiniteQuaternion = (q: Quaternion): boolean => {
  const length = Math.hypot(q.x, q.y, q.z, q.w);
  return Number.isFinite(q.x) && Number.isFinite(q.y) && Number.isFinite(q.z) && Number.isFinite(q.w)
    && Number.isFinite(length) && length > EPSILON;
};

/** Extrait la rotation caméra d'une matrice faciale MediaPipe 4×4 column-major. */
export const cameraQuaternionFromMediaPipeMatrix = (matrix: readonly number[]): Quaternion => {
  if (matrix.length < 16 || matrix.some((value) => !Number.isFinite(value))) {
    return { x: 0, y: 0, z: 0, w: 1 };
  }

  const m00 = matrix[0];
  const m01 = matrix[4];
  const m02 = matrix[8];
  const m10 = matrix[1];
  const m11 = matrix[5];
  const m12 = matrix[9];
  const m20 = matrix[2];
  const m21 = matrix[6];
  const m22 = matrix[10];
  const trace = m00 + m11 + m22;
  let x: number;
  let y: number;
  let z: number;
  let w: number;

  if (trace > 0) {
    const s = Math.sqrt(trace + 1) * 2;
    w = 0.25 * s;
    x = (m21 - m12) / s;
    y = (m02 - m20) / s;
    z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const s = Math.sqrt(1 + m00 - m11 - m22) * 2;
    w = (m21 - m12) / s;
    x = 0.25 * s;
    y = (m01 + m10) / s;
    z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const s = Math.sqrt(1 + m11 - m00 - m22) * 2;
    w = (m02 - m20) / s;
    x = (m01 + m10) / s;
    y = 0.25 * s;
    z = (m12 + m21) / s;
  } else {
    const s = Math.sqrt(1 + m22 - m00 - m11) * 2;
    w = (m10 - m01) / s;
    x = (m02 + m20) / s;
    y = (m12 + m21) / s;
    z = 0.25 * s;
  }

  return normalizeQuaternion({ x, y, z, w });
};

/**
 * MediaPipe métrique est Y-up et regarde vers -Z. Une webcam frontale non miroir
 * diffère du monde audio (+Z vers la webcam) par une réflexion de X.
 */
export const cameraPoseToAudioPose = (camera: Quaternion): Quaternion =>
  normalizeQuaternion({ x: camera.x, y: -camera.y, z: -camera.z, w: camera.w });

export const quaternionFromMediaPipeMatrix = (matrix: readonly number[]): Quaternion =>
  cameraPoseToAudioPose(cameraQuaternionFromMediaPipeMatrix(matrix));

export const eulerFromQuaternion = (input: Quaternion): EulerAngles => {
  const q = normalizeQuaternion(input);
  const sinPitch = 2 * (q.w * q.x - q.z * q.y);
  const pitch = Math.asin(Math.max(-1, Math.min(1, sinPitch)));
  const yaw = Math.atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.x * q.x + q.y * q.y));
  const roll = Math.atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.x * q.x + q.z * q.z));
  const toDegrees = 180 / Math.PI;
  return { yaw: yaw * toDegrees, pitch: pitch * toDegrees, roll: roll * toDegrees };
};

export const angularVelocity = (
  previous: Quaternion | null,
  current: Quaternion,
  deltaSeconds: number,
): Vector3 => {
  if (!previous || deltaSeconds <= 0 || deltaSeconds > 0.25) return { x: 0, y: 0, z: 0 };

  const before = normalizeQuaternion(previous);
  let after = normalizeQuaternion(current);
  if (before.x * after.x + before.y * after.y + before.z * after.z + before.w * after.w < 0) {
    after = { x: -after.x, y: -after.y, z: -after.z, w: -after.w };
  }
  const inverse = { x: -before.x, y: -before.y, z: -before.z, w: before.w };
  let delta = multiplyQuaternions(after, inverse);
  if (delta.w < 0) delta = { x: -delta.x, y: -delta.y, z: -delta.z, w: -delta.w };
  const angle = 2 * Math.acos(Math.max(-1, Math.min(1, delta.w)));
  const denominator = Math.sqrt(Math.max(EPSILON, 1 - delta.w * delta.w));
  if (angle < EPSILON) return { x: 0, y: 0, z: 0 };
  const scale = angle / (denominator * deltaSeconds);
  return { x: delta.x * scale, y: delta.y * scale, z: delta.z * scale };
};

export const multiplyQuaternions = (a: Quaternion, b: Quaternion): Quaternion => normalizeQuaternion({
  x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
  y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
  z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
});

/** Exprime une pose caméra dans le repère monde calibré où reference devient identité. */
export const calibrateQuaternion = (reference: Quaternion, current: Quaternion): Quaternion =>
  multiplyQuaternions(
    { x: -reference.x, y: -reference.y, z: -reference.z, w: reference.w },
    current,
  );

export const identityQuaternion: Quaternion = { x: 0, y: 0, z: 0, w: 1 };
