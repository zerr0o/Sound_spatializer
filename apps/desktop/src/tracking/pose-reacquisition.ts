import type { Quaternion } from '../types/contracts';
import { isFiniteQuaternion, normalizeQuaternion } from './pose-math';

const DEFAULT_MISSING_FRAMES = 3;
const DEFAULT_MAX_STEP_DEGREES = 35;

const angularDistanceDegrees = (left: Quaternion, right: Quaternion): number => {
  const a = normalizeQuaternion(left);
  const b = normalizeQuaternion(right);
  // q and -q encode the same orientation.
  const dot = Math.abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return 2 * Math.acos(Math.min(1, Math.max(0, dot))) * 180 / Math.PI;
};

/**
 * After a complete loss, require two coherent model results before exposing a
 * new pose. This runs only during reacquisition: continuous tracking keeps its
 * zero-frame path and brief one/two-frame occlusions keep the existing hold.
 */
export class PoseReacquisitionGuard {
  private missingFrames = 0;
  private confirmationRequired = false;
  private candidate: Quaternion | null = null;

  constructor(
    private readonly missingFramesBeforeConfirmation = DEFAULT_MISSING_FRAMES,
    private readonly maximumCandidateStepDegrees = DEFAULT_MAX_STEP_DEGREES,
  ) {}

  update(pose: Quaternion | null): Quaternion | null {
    if (pose && !isFiniteQuaternion(pose)) pose = null;
    if (!pose) {
      this.missingFrames += 1;
      if (this.confirmationRequired) this.candidate = null;
      if (this.missingFrames >= this.missingFramesBeforeConfirmation) {
        this.confirmationRequired = true;
      }
      return null;
    }

    this.missingFrames = 0;
    if (!this.confirmationRequired) return pose;

    if (!this.candidate || angularDistanceDegrees(this.candidate, pose) > this.maximumCandidateStepDegrees) {
      this.candidate = pose;
      return null;
    }

    this.confirmationRequired = false;
    this.candidate = null;
    return pose;
  }

  reset() {
    this.missingFrames = 0;
    this.confirmationRequired = false;
    this.candidate = null;
  }
}
