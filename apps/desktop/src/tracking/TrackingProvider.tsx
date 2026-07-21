import { createContext, type PropsWithChildren, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { desktopBridge, QpcClock } from '../lib/tauri-bridge';
import { useAppStore } from '../store/app-store';
import type { HeadPoseSampleV1, Quaternion, TrackingState } from '../types/contracts';
import { requestTrackingCamera } from './camera-constraints';
import { MediaTimestampMapper, videoCallbackTimestampToPerformance, videoFrameTimestampToPerformance } from './frame-timestamps';
import { LatestFrameSlot } from './latest-frame-slot';
import { angularVelocity, calibrateQuaternion, eulerFromQuaternion, identityQuaternion } from './pose-math';
import { nextPoseSequence } from './pose-sequence';
import { captureTrackingLossOrigin, resolveMissingTrackingPose } from './tracking-loss-transition';

interface TrackingController {
  start: () => Promise<void>;
  stop: () => void;
  calibrate: () => Promise<boolean>;
  running: boolean;
  videoElement: HTMLVideoElement | null;
}

interface WorkerResult {
  type: 'result';
  timestampMs: number;
  processingMs: number;
  quaternion: Quaternion | null;
  confidence: number;
}

interface TrackProcessor<T> {
  readable: ReadableStream<T>;
}

interface TrackProcessorConstructor {
  new (options: { track: MediaStreamTrack }): TrackProcessor<VideoFrame>;
}

const TrackingContext = createContext<TrackingController | null>(null);

export function TrackingProvider({ children }: PropsWithChildren) {
  const patchTracking = useAppStore((state) => state.patchTracking);
  const setTrackingPose = useAppStore((state) => state.setTrackingPose);
  const patchScene = useAppStore((state) => state.patchScene);
  const scene = useAppStore((state) => state.scene);
  const notify = useAppStore((state) => state.notify);
  const [running, setRunning] = useState(false);
  const [videoElement, setVideoElement] = useState<HTMLVideoElement | null>(null);
  const workerRef = useRef<Worker | null>(null);
  const streamRef = useRef<MediaStream | null>(null);
  const readerRef = useRef<ReadableStreamDefaultReader<VideoFrame> | null>(null);
  const animationRef = useRef<number | null>(null);
  const animationModeRef = useRef<'video' | 'animation' | null>(null);
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const busyRef = useRef(false);
  const processorFramesRef = useRef(new LatestFrameSlot<VideoFrame>());
  const fallbackFramesRef = useRef(new LatestFrameSlot<ImageBitmap>());
  const resumeFramePumpRef = useRef<() => void>(() => undefined);
  const framePumpGenerationRef = useRef(0);
  const trackingSessionRef = useRef(0);
  const startInFlightRef = useRef(false);
  const cancelInitializationWaitRef = useRef<(() => void) | null>(null);
  const lastPoseRef = useRef<Quaternion | null>(null);
  const lastRawPoseRef = useRef<Quaternion | null>(null);
  const calibrationRef = useRef<Quaternion>(identityQuaternion);
  const lastPoseTimeRef = useRef(0);
  const lastFaceTimeRef = useRef(0);
  const lossOriginPoseRef = useRef<Quaternion | null>(null);
  const lastTrackingStateRef = useRef<TrackingState>('lost');
  const fpsSamplesRef = useRef<number[]>([]);
  const qpcClockRef = useRef(new QpcClock());
  const processorTimestampClockRef = useRef(new MediaTimestampMapper());
  const callbackTimestampClockRef = useRef(new MediaTimestampMapper());
  const poseDeliveryRef = useRef<{
    failures: number;
    reportedMessage: string | null;
    recoveryStartedAt: number | null;
  }>({ failures: 0, reportedMessage: null, recoveryStartedAt: null });

  const handlePoseDelivery = useCallback(
    (error: unknown | null) => {
      const delivery = poseDeliveryRef.current;
      if (!error) {
        delivery.failures = 0;
        if (!delivery.reportedMessage) {
          delivery.recoveryStartedAt = null;
          return;
        }
        const now = performance.now();
        if (delivery.recoveryStartedAt === null) {
          delivery.recoveryStartedAt = now;
          return;
        }
        if (now - delivery.recoveryStartedAt < 1_000) return;
        const currentError = useAppStore.getState().tracking.error;
        if (currentError === delivery.reportedMessage) patchTracking({ error: null });
        delivery.reportedMessage = null;
        delivery.recoveryStartedAt = null;
        return;
      }

      delivery.recoveryStartedAt = null;
      delivery.failures += 1;
      if (delivery.failures < 3 || delivery.reportedMessage) return;
      const detail = error instanceof Error ? error.message : String(error);
      const message = `Transmission de la pose au moteur impossible : ${detail}`;
      delivery.reportedMessage = message;
      patchTracking({ error: message });
      notify({ tone: 'warning', title: 'Suivi audio interrompu', detail: message });
    },
    [notify, patchTracking],
  );

  useEffect(() => desktopBridge.subscribeHeadPoseDelivery(handlePoseDelivery), [handlePoseDelivery]);

  const emitPose = useCallback(
    (quaternion: Quaternion | null, timestampMs: number, confidence: number, processingMs: number) => {
      busyRef.current = false;
      // Start the freshest pending inference before UI bookkeeping and IPC.
      resumeFramePumpRef.current();
      const previous = lastPoseRef.current;
      if (quaternion) lastRawPoseRef.current = quaternion;
      let resolved: Quaternion;
      let trackingState: TrackingState = 'tracked';

      if (quaternion) {
        resolved = calibrateQuaternion(calibrationRef.current, quaternion);
        lastFaceTimeRef.current = timestampMs;
        lossOriginPoseRef.current = null;
      } else {
        if (!lossOriginPoseRef.current) {
          lossOriginPoseRef.current = captureTrackingLossOrigin(previous, lastTrackingStateRef.current);
        }
        const absentFor = timestampMs - lastFaceTimeRef.current;
        const missing = resolveMissingTrackingPose(lossOriginPoseRef.current, absentFor);
        resolved = missing.quaternion;
        trackingState = missing.trackingState;
      }

      const outputPose = resolved;
      const deltaSeconds = (timestampMs - lastPoseTimeRef.current) / 1000;
      // A pose following any missing frame starts a new continuous tracking
      // segment. Do not turn the neutral/held -> face jump into a huge angular
      // velocity which the audio predictor would otherwise extrapolate.
      const continuousTracking = trackingState === 'tracked' && lastTrackingStateRef.current === 'tracked';
      const sample: HeadPoseSampleV1 = {
        version: 1,
        sequence: nextPoseSequence(),
        timestampQpc: qpcClockRef.current.fromPerformanceTime(timestampMs),
        quaternion: outputPose,
        angularVelocity: continuousTracking ? angularVelocity(previous, outputPose, deltaSeconds) : { x: 0, y: 0, z: 0 },
        confidence,
        trackingState,
      };
      lastPoseRef.current = outputPose;
      lastPoseTimeRef.current = timestampMs;
      lastTrackingStateRef.current = trackingState;
      const now = performance.now();
      fpsSamplesRef.current = [...fpsSamplesRef.current.filter((value) => now - value < 1_000), now];
      setTrackingPose(sample, eulerFromQuaternion(outputPose));
      patchTracking({
        fps: fpsSamplesRef.current.length,
        processingMs,
        state: 'running',
        error: poseDeliveryRef.current.reportedMessage,
      });
      desktopBridge.pushHeadPose(sample);
    },
    [patchTracking, setTrackingPose],
  );

  const postFrame = useCallback((frame: ImageBitmap, timestampMs: number) => {
    const worker = workerRef.current;
    if (!worker || busyRef.current) {
      frame.close();
      return;
    }
    busyRef.current = true;
    worker.postMessage({ type: 'frame', frame, timestampMs }, [frame]);
  }, []);

  const startFallbackPump = useCallback(
    (video: HTMLVideoElement) => {
      const generation = ++framePumpGenerationRef.current;
      const frames = fallbackFramesRef.current;
      frames.reset();
      let captureInFlight = false;

      const drainLatest = () => {
        if (generation !== framePumpGenerationRef.current || busyRef.current) return;
        const latest = frames.take();
        if (latest) postFrame(latest.frame, latest.timestampMs);
      };
      resumeFramePumpRef.current = drainLatest;

      const scheduleNextFrame = () => {
        if (!streamRef.current || generation !== framePumpGenerationRef.current) return;
        if ('requestVideoFrameCallback' in video) {
          animationModeRef.current = 'video';
          animationRef.current = video.requestVideoFrameCallback((now, metadata) => {
            const timestampMs = videoCallbackTimestampToPerformance(now, metadata, callbackTimestampClockRef.current);
            pump(timestampMs);
          });
        } else {
          animationModeRef.current = 'animation';
          animationRef.current = requestAnimationFrame((now) => pump(now));
        }
      };
      const pump = (timestampMs: number) => {
        if (!streamRef.current || generation !== framePumpGenerationRef.current) return;
        // Keep listening to camera presentation while MediaPipe is busy. The
        // slot below retains only the most recent completed capture.
        scheduleNextFrame();
        if (captureInFlight || video.readyState < HTMLMediaElement.HAVE_CURRENT_DATA) return;
        captureInFlight = true;
        void createImageBitmap(video)
          .then((frame) => {
            if (generation !== framePumpGenerationRef.current) {
              frame.close();
              return;
            }
            frames.offer(frame, timestampMs);
            drainLatest();
          })
          .catch(() => {
            // La frame peut devenir invalide pendant un changement de caméra.
          })
          .finally(() => {
            captureInFlight = false;
          });
      };
      scheduleNextFrame();
    },
    [postFrame],
  );

  const startProcessorPump = useCallback(
    async (track: MediaStreamTrack, Processor: TrackProcessorConstructor) => {
      const generation = ++framePumpGenerationRef.current;
      const frames = processorFramesRef.current;
      frames.reset();
      let conversionInFlight = false;
      const processor = new Processor({ track });
      const reader = processor.readable.getReader();
      readerRef.current = reader;

      const drainLatest = () => {
        if (generation !== framePumpGenerationRef.current || busyRef.current || conversionInFlight) return;
        const latest = frames.take();
        if (!latest) return;
        conversionInFlight = true;
        void createImageBitmap(latest.frame)
          .then((bitmap) => {
            if (generation !== framePumpGenerationRef.current) bitmap.close();
            else postFrame(bitmap, latest.timestampMs);
          })
          .catch(() => {
            // Le flux peut être invalidé pendant un hotplug ou un arrêt.
          })
          .finally(() => {
            latest.frame.close();
            conversionInFlight = false;
            if (generation === framePumpGenerationRef.current) drainLatest();
          });
      };
      resumeFramePumpRef.current = drainLatest;

      try {
        while (generation === framePumpGenerationRef.current && streamRef.current && track.readyState === 'live') {
          const { done, value } = await reader.read();
          if (done) break;
          // cancel() can race with an already-resolving read. Never let a
          // frame owned by the previous session enter the shared latest slot
          // after stop/restart.
          if (generation !== framePumpGenerationRef.current || !streamRef.current || track.readyState !== 'live') {
            value.close();
            break;
          }
          const observedAtMs = performance.now();
          const timestampMs = videoFrameTimestampToPerformance(
            value.timestamp,
            observedAtMs,
            processorTimestampClockRef.current,
          );
          frames.offer(value, timestampMs);
          drainLatest();
        }
      } catch {
        // reader.cancel() rejette sur certaines versions de WebView2.
      } finally {
        if (readerRef.current === reader) readerRef.current = null;
        if (generation === framePumpGenerationRef.current) frames.reset();
      }
    },
    [postFrame],
  );

  const stop = useCallback(() => {
    trackingSessionRef.current += 1;
    cancelInitializationWaitRef.current?.();
    cancelInitializationWaitRef.current = null;
    framePumpGenerationRef.current += 1;
    resumeFramePumpRef.current = () => undefined;
    processorFramesRef.current.reset();
    fallbackFramesRef.current.reset();
    readerRef.current?.cancel().catch(() => undefined);
    readerRef.current = null;
    if (animationRef.current !== null) {
      if (animationModeRef.current === 'video' && videoRef.current && 'cancelVideoFrameCallback' in videoRef.current) {
        videoRef.current.cancelVideoFrameCallback(animationRef.current);
      } else {
        cancelAnimationFrame(animationRef.current);
      }
      animationRef.current = null;
      animationModeRef.current = null;
    }
    workerRef.current?.postMessage({ type: 'dispose' });
    workerRef.current = null;
    streamRef.current?.getTracks().forEach((track) => track.stop());
    streamRef.current = null;
    setVideoElement((current) => {
      if (current) current.srcObject = null;
      return null;
    });
    videoRef.current = null;
    busyRef.current = false;
    lastPoseRef.current = null;
    lastRawPoseRef.current = null;
    lastPoseTimeRef.current = 0;
    lastFaceTimeRef.current = 0;
    lossOriginPoseRef.current = null;
    lastTrackingStateRef.current = 'lost';
    fpsSamplesRef.current = [];
    processorTimestampClockRef.current.reset();
    callbackTimestampClockRef.current.reset();
    setRunning(false);
    patchTracking({ state: 'idle', fps: 0, cameraFrameRate: null });
  }, [patchTracking]);

  const start = useCallback(async () => {
    if (running || streamRef.current || startInFlightRef.current) return;
    startInFlightRef.current = true;
    const session = ++trackingSessionRef.current;
    let attemptStream: MediaStream | null = null;
    let attemptVideo: HTMLVideoElement | null = null;
    let attemptWorker: Worker | null = null;
    const sessionIsCurrent = () => session === trackingSessionRef.current;
    const discardCancelledAttempt = () => {
      if (attemptWorker) {
        attemptWorker.postMessage({ type: 'dispose' });
        if (workerRef.current === attemptWorker) workerRef.current = null;
      }
      if (attemptVideo) attemptVideo.srcObject = null;
      attemptStream?.getTracks().forEach((track) => track.stop());
      if (streamRef.current === attemptStream) streamRef.current = null;
      if (videoRef.current === attemptVideo) videoRef.current = null;
    };
    patchTracking({ state: 'requesting', error: null });
    try {
      const stream = await requestTrackingCamera((constraints) => navigator.mediaDevices.getUserMedia(constraints));
      attemptStream = stream;
      if (!sessionIsCurrent()) {
        discardCancelledAttempt();
        return;
      }
      processorTimestampClockRef.current.reset();
      callbackTimestampClockRef.current.reset();
      streamRef.current = stream;
      const track = stream.getVideoTracks()[0];
      const settings = track.getSettings();
      const video = document.createElement('video');
      attemptVideo = video;
      video.muted = true;
      video.autoplay = true;
      video.playsInline = true;
      video.srcObject = stream;
      await video.play();
      if (!sessionIsCurrent()) {
        discardCancelledAttempt();
        return;
      }
      setVideoElement(video);
      videoRef.current = video;
      patchTracking({
        state: 'starting',
        permission: 'granted',
        cameraLabel: track.label || 'Caméra locale',
        cameraFrameRate: settings.frameRate ?? null,
      });
      await qpcClockRef.current.synchronize();
      if (!sessionIsCurrent()) {
        discardCancelledAttempt();
        return;
      }

      const worker = new Worker(new URL('./face-landmarker.worker.ts', import.meta.url), { type: 'module' });
      attemptWorker = worker;
      workerRef.current = worker;
      let cancelInitializationWait: () => void = () => undefined;
      const initialization = new Promise<void>((resolve, reject) => {
        let settled = false;
        let timeout = 0;
        const settle = (error?: Error) => {
          if (settled) return;
          settled = true;
          window.clearTimeout(timeout);
          if (error) reject(error);
          else resolve();
        };
        timeout = window.setTimeout(() => settle(new Error('Initialisation MediaPipe trop longue.')), 20_000);
        cancelInitializationWait = () => settle();
        worker.onmessage = (event: MessageEvent<WorkerResult | { type: 'ready'; delegate: string } | { type: 'error'; message: string }>) => {
          const message = event.data;
          if (message.type === 'ready') {
            settle();
          } else if (message.type === 'error') {
            busyRef.current = false;
            settle(new Error(message.message));
          }
        };
        worker.postMessage({ type: 'init', wasmRoot: '/mediapipe', modelAssetPath: '/models/face_landmarker.task' });
      });
      cancelInitializationWaitRef.current = cancelInitializationWait;
      try {
        await initialization;
      } finally {
        if (cancelInitializationWaitRef.current === cancelInitializationWait) {
          cancelInitializationWaitRef.current = null;
        }
      }
      if (!sessionIsCurrent()) {
        discardCancelledAttempt();
        return;
      }
      worker.onmessage = (event: MessageEvent<WorkerResult | { type: 'error'; message: string }>) => {
        // Une inférence déjà lancée peut répondre juste après stop(). Elle ne
        // doit ni republier une pose ni remettre l'interface à l'état actif.
        if (!sessionIsCurrent()) return;
        const message = event.data;
        if (message.type === 'result') {
          emitPose(message.quaternion, message.timestampMs, message.confidence, message.processingMs);
        } else if (message.type === 'error') {
          busyRef.current = false;
          resumeFramePumpRef.current();
          patchTracking({ state: 'error', error: message.message });
        }
      };

      setRunning(true);
      patchTracking({ state: 'running' });
      const Processor = (window as unknown as { MediaStreamTrackProcessor?: TrackProcessorConstructor })
        .MediaStreamTrackProcessor;
      if (Processor) void startProcessorPump(track, Processor);
      else startFallbackPump(video);
    } catch (error) {
      stop();
      const denied = error instanceof DOMException && (error.name === 'NotAllowedError' || error.name === 'SecurityError');
      patchTracking({
        state: denied ? 'unavailable' : 'error',
        permission: denied ? 'denied' : 'prompt',
        error: denied
          ? 'Accès caméra refusé. Autorisez la caméra dans les réglages de confidentialité Windows.'
          : error instanceof Error
            ? error.message
            : String(error),
      });
      throw error;
    } finally {
      startInFlightRef.current = false;
    }
  }, [emitPose, patchTracking, running, startFallbackPump, startProcessorPump, stop]);

  const calibrate = useCallback(async () => {
    const pose = useAppStore.getState().tracking.pose;
    const rawPose = lastRawPoseRef.current;
    if (!pose || !rawPose || pose.trackingState !== 'tracked') {
      notify({ tone: 'warning', title: 'Visage non détecté', detail: 'Regardez droit devant vous puis réessayez.' });
      return false;
    }
    calibrationRef.current = rawPose;
    lastPoseRef.current = identityQuaternion;
    setTrackingPose({ ...pose, quaternion: identityQuaternion, angularVelocity: { x: 0, y: 0, z: 0 } }, { yaw: 0, pitch: 0, roll: 0 });
    patchScene({ listener: { ...scene.listener, neutralPose: identityQuaternion } });
    await desktopBridge.sendCommand({ version: 1, type: 'calibrate-neutral-pose', quaternion: identityQuaternion });
    notify({ tone: 'success', title: 'Position neutre calibrée', detail: 'Les enceintes restent désormais ancrées face à vous.' });
    return true;
  }, [notify, patchScene, scene.listener, setTrackingPose]);

  useEffect(() => stop, [stop]);

  return (
    <TrackingContext.Provider value={{ start, stop, calibrate, running, videoElement }}>
      {children}
    </TrackingContext.Provider>
  );
}

export const useTrackingController = () => {
  const context = useContext(TrackingContext);
  if (!context) throw new Error('useTrackingController doit être utilisé dans TrackingProvider.');
  return context;
};
