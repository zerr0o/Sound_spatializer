export interface PoseSequenceState {
  last: number;
}

type PoseSequenceGlobal = typeof globalThis & {
  __soundSpatializerPoseSequenceV1?: PoseSequenceState;
};

const globalScope = globalThis as PoseSequenceGlobal;
const sharedState = globalScope.__soundSpatializerPoseSequenceV1 ?? { last: 0 };
globalScope.__soundSpatializerPoseSequenceV1 = sharedState;

const MAX_SEQUENCE = Number.MAX_SAFE_INTEGER;

/** Microsecondes Unix, encore exactement représentables par un number en 2026. */
export function poseSequenceSeed(timeOriginMs: number, elapsedMs: number): number {
  const value = Math.floor((timeOriginMs + elapsedMs) * 1_000);
  if (!Number.isFinite(value)) return 1;
  return Math.max(1, Math.min(MAX_SEQUENCE - 1, value));
}

/**
 * Une instance correspond à un montage du TrackingProvider. L’état global survit
 * aux remplacements de modules Vite ; après un rechargement complet, le seed
 * absolu est nécessairement postérieur à celui de la page précédente.
 */
export class MonotonicPoseSequence {
  private nextValue: number;

  constructor(
    private readonly state: PoseSequenceState = sharedState,
    seed = poseSequenceSeed(performance.timeOrigin, performance.now()),
  ) {
    this.nextValue = Math.max(seed, state.last + 1);
  }

  next(): number {
    const value = Math.max(this.nextValue, this.state.last + 1);
    if (!Number.isSafeInteger(value) || value > MAX_SEQUENCE) {
      throw new Error('Séquence de pose hors de la plage entière sûre.');
    }
    this.state.last = value;
    this.nextValue = value + 1;
    return value;
  }
}

const globalSequence = new MonotonicPoseSequence();

/** Point d’entrée sans état React, donc compatible avec un Fast Refresh. */
export function nextPoseSequence(): number {
  return globalSequence.next();
}
