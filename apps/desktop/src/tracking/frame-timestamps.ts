const DIRECT_CLOCK_TOLERANCE_MS = 250;
const FUTURE_TOLERANCE_MS = 5;

const isTimestamp = (value: number | null | undefined): value is number =>
  typeof value === 'number' && Number.isFinite(value) && value >= 0;

/**
 * Maps a media timeline onto the monotonic Performance timeline. The offset is
 * deliberately fixed after the first frame so callback jitter does not become
 * artificial head motion or angular velocity.
 */
export class MediaTimestampMapper {
  private offsetMs: number | null = null;

  reset() {
    this.offsetMs = null;
  }

  toPerformanceTime(mediaTimeMs: number, referencePerformanceMs: number): number {
    if (!isTimestamp(mediaTimeMs) || !isTimestamp(referencePerformanceMs)) return referencePerformanceMs;
    if (this.offsetMs === null) this.offsetMs = referencePerformanceMs - mediaTimeMs;
    return Math.min(mediaTimeMs + this.offsetMs, referencePerformanceMs);
  }
}

/** Convert WebCodecs' microsecond VideoFrame timestamp to performance.now(). */
export function videoFrameTimestampToPerformance(
  timestampUs: number | null | undefined,
  observedAtMs: number,
  relativeClock: MediaTimestampMapper,
  performanceTimeOriginMs = performance.timeOrigin,
): number {
  if (!isTimestamp(timestampUs)) return observedAtMs;
  const timestampMs = timestampUs / 1_000;

  // Chromium may expose a timestamp already based on performance.now().
  if (Math.abs(timestampMs - observedAtMs) <= DIRECT_CLOCK_TOLERANCE_MS) return Math.min(timestampMs, observedAtMs);

  // Some producers expose epoch microseconds. Bring those onto the Performance timeline.
  const epochRelativeMs = timestampMs - performanceTimeOriginMs;
  if (Math.abs(epochRelativeMs - observedAtMs) <= DIRECT_CLOCK_TOLERANCE_MS) {
    return Math.min(epochRelativeMs, observedAtMs);
  }

  // Stream-relative timelines need one explicit offset calibration.
  return relativeClock.toPerformanceTime(timestampMs, observedAtMs);
}

/** Select the earliest trustworthy rVFC timestamp, expressed on the Performance clock. */
export function videoCallbackTimestampToPerformance(
  callbackNowMs: number,
  metadata: Pick<
    VideoFrameCallbackMetadata,
    'captureTime' | 'receiveTime' | 'mediaTime' | 'presentationTime' | 'expectedDisplayTime' | 'processingDuration'
  >,
  relativeClock: MediaTimestampMapper,
): number {
  if (isTimestamp(metadata.captureTime) && metadata.captureTime <= callbackNowMs + FUTURE_TOLERANCE_MS) {
    return Math.min(metadata.captureTime, callbackNowMs);
  }
  if (isTimestamp(metadata.receiveTime) && metadata.receiveTime <= callbackNowMs + FUTURE_TOLERANCE_MS) {
    return Math.min(metadata.receiveTime, callbackNowMs);
  }

  const presentationMs = isTimestamp(metadata.presentationTime)
    ? Math.min(metadata.presentationTime, callbackNowMs)
    : isTimestamp(metadata.expectedDisplayTime)
      ? Math.min(metadata.expectedDisplayTime, callbackNowMs)
      : callbackNowMs;
  const decodingMs = isTimestamp(metadata.processingDuration) ? metadata.processingDuration * 1_000 : 0;
  const frameReadyMs = Math.max(0, presentationMs - decodingMs);
  return isTimestamp(metadata.mediaTime)
    ? relativeClock.toPerformanceTime(metadata.mediaTime * 1_000, frameReadyMs)
    : frameReadyMs;
}
