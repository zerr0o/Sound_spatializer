import type { Vector3 } from '../types/contracts';

export interface SpeakerPolar {
  azimuth: number;
  distance: number;
}

/** Géométrie horizontale de l’enceinte relative au point d’écoute statique. */
export const speakerPolarFromListener = (speaker: Vector3, listener: Vector3): SpeakerPolar => {
  const x = speaker.x - listener.x;
  const z = speaker.z - listener.z;
  return {
    azimuth: Math.atan2(x, z) * (180 / Math.PI),
    distance: Math.hypot(x, z),
  };
};

export const speakerPositionFromPolar = (
  polar: SpeakerPolar,
  listener: Vector3,
  height: number,
): Vector3 => {
  const radians = polar.azimuth * (Math.PI / 180);
  return {
    x: listener.x + Math.sin(radians) * polar.distance,
    y: height,
    z: listener.z + Math.cos(radians) * polar.distance,
  };
};
