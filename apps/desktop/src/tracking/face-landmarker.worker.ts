/// <reference lib="webworker" />

import { FaceLandmarker } from '@mediapipe/tasks-vision';
import { faceTrackingConfidence } from './face-confidence';
import { createFaceLandmarkerOptions, type FaceLandmarkerDelegate } from './face-landmarker-config';
import { isFiniteQuaternion, quaternionFromMediaPipeMatrix } from './pose-math';
import { PoseReacquisitionGuard } from './pose-reacquisition';
import { createLocalVisionFileset, installMediaPipeModuleFactory, supportsMediaPipeWasm } from './mediapipe-runtime';
import type { Quaternion } from '../types/contracts';

type WorkerRequest =
  | { type: 'init'; wasmRoot: string; modelAssetPath: string }
  | { type: 'frame'; frame: ImageBitmap; timestampMs: number }
  | { type: 'dispose' };

type WorkerResponse =
  | { type: 'ready'; delegate: 'GPU' | 'CPU' }
  | {
      type: 'result';
      timestampMs: number;
      processingMs: number;
      quaternion: Quaternion | null;
      confidence: number;
    }
  | { type: 'error'; message: string };

let landmarker: FaceLandmarker | null = null;
const reacquisitionGuard = new PoseReacquisitionGuard();

const post = (message: WorkerResponse) => self.postMessage(message);

const createLandmarker = async (wasmRoot: string, modelAssetPath: string) => {
  reacquisitionGuard.reset();
  if (!supportsMediaPipeWasm()) {
    throw new Error('Cette version de WebView2 ne prend pas en charge WebAssembly SIMD. Mettez Microsoft Edge WebView2 à jour.');
  }
  const vision = createLocalVisionFileset(wasmRoot);
  const options = (delegate: FaceLandmarkerDelegate) => createFaceLandmarkerOptions(modelAssetPath, delegate);

  try {
    installMediaPipeModuleFactory(self);
    landmarker = await FaceLandmarker.createFromOptions(vision, options('GPU'));
    post({ type: 'ready', delegate: 'GPU' });
  } catch {
    // tasks-vision consomme puis efface ModuleFactory après l'initialisation.
    // La réinstaller rend le repli CPU indépendant d'un échec WebGL partiel.
    installMediaPipeModuleFactory(self);
    landmarker = await FaceLandmarker.createFromOptions(vision, options('CPU'));
    post({ type: 'ready', delegate: 'CPU' });
  }
};

self.onmessage = async (event: MessageEvent<WorkerRequest>) => {
  const message = event.data;
  if (message.type === 'init') {
    try {
      await createLandmarker(message.wasmRoot, message.modelAssetPath);
    } catch (error) {
      post({ type: 'error', message: error instanceof Error ? error.message : String(error) });
    }
    return;
  }

  if (message.type === 'dispose') {
    reacquisitionGuard.reset();
    landmarker?.close();
    landmarker = null;
    self.close();
    return;
  }

  if (!landmarker) {
    message.frame.close();
    post({ type: 'error', message: 'Le suivi facial n’est pas initialisé.' });
    return;
  }

  const started = performance.now();
  try {
    const result = landmarker.detectForVideo(message.frame, message.timestampMs);
    const matrix = result.facialTransformationMatrixes?.[0]?.data;
    const matrixValues = matrix ? Array.from(matrix) : null;
    const landmarks = result.faceLandmarks?.[0];
    const confidence = faceTrackingConfidence(matrixValues, landmarks);
    const candidatePose = confidence > 0 && matrixValues ? quaternionFromMediaPipeMatrix(matrixValues) : null;
    const detectedPose = candidatePose && isFiniteQuaternion(candidatePose) ? candidatePose : null;
    const confirmedPose = reacquisitionGuard.update(detectedPose);
    post({
      type: 'result',
      timestampMs: message.timestampMs,
      processingMs: performance.now() - started,
      quaternion: confirmedPose,
      confidence: confirmedPose ? confidence : 0,
    });
  } catch (error) {
    post({ type: 'error', message: error instanceof Error ? error.message : String(error) });
  } finally {
    message.frame.close();
  }
};

export {};
