export type FaceLandmarkerDelegate = 'GPU' | 'CPU';

/**
 * These are MediaPipe's internal lifecycle thresholds, not the confidence
 * carried by HeadPoseSampleV1. In VIDEO mode the tracking threshold is what
 * lets the graph abandon a stale region of interest and run face detection
 * again. Setting it to zero makes a lost tracker look successful forever: it
 * keeps extrapolating landmarks over the background and cannot reacquire the
 * real face.
 *
 * Keep the model defaults here so absence is reported explicitly. Once a
 * valid matrix is returned, the application still accepts a pose whose
 * diagnostic confidence is zero (see faceTrackingConfidence/PosePredictor).
 */
export function createFaceLandmarkerOptions(modelAssetPath: string, delegate: FaceLandmarkerDelegate) {
  return {
    baseOptions: { modelAssetPath, delegate },
    runningMode: 'VIDEO' as const,
    numFaces: 1,
    minFaceDetectionConfidence: 0.5,
    minFacePresenceConfidence: 0.5,
    minTrackingConfidence: 0.5,
    outputFaceBlendshapes: false,
    outputFacialTransformationMatrixes: true,
  };
}
