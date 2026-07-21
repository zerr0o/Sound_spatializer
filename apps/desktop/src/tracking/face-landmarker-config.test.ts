import { describe, expect, it } from 'vitest';
import { createFaceLandmarkerOptions } from './face-landmarker-config';

describe('configuration Face Landmarker', () => {
  it('conserve les seuils de cycle de vie nécessaires à la réacquisition', () => {
    const options = createFaceLandmarkerOptions('/models/face_landmarker.task', 'GPU');

    expect(options).toMatchObject({
      minFaceDetectionConfidence: 0.5,
      minFacePresenceConfidence: 0.5,
      minTrackingConfidence: 0.5,
      runningMode: 'VIDEO',
      numFaces: 1,
      outputFaceBlendshapes: false,
      outputFacialTransformationMatrixes: true,
    });
  });

  it('ne désactive jamais le seuil interne du tracker vidéo', () => {
    const options = createFaceLandmarkerOptions('/models/face_landmarker.task', 'CPU');

    expect(options.minTrackingConfidence).toBeGreaterThan(0);
    expect(options.minFacePresenceConfidence).toBeGreaterThan(0);
  });

  it('conserve le modèle et le delegate demandés', () => {
    expect(createFaceLandmarkerOptions('/local/model.task', 'CPU').baseOptions).toEqual({
      modelAssetPath: '/local/model.task',
      delegate: 'CPU',
    });
  });
});
