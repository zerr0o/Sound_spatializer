export interface ClosableFrame {
  close(): void;
}

export interface TimestampedFrame<T extends ClosableFrame> {
  frame: T;
  timestampMs: number;
}

/**
 * Single-slot, latest-wins frame buffer. Replaced and out-of-order frames are
 * closed immediately so neither camera latency nor native image memory can
 * accumulate while MediaPipe is busy.
 */
export class LatestFrameSlot<T extends ClosableFrame> {
  private pending: TimestampedFrame<T> | null = null;
  private newestTimestampMs = Number.NEGATIVE_INFINITY;

  offer(frame: T, timestampMs: number): boolean {
    if (!Number.isFinite(timestampMs) || timestampMs <= this.newestTimestampMs) {
      frame.close();
      return false;
    }

    this.newestTimestampMs = timestampMs;
    this.pending?.frame.close();
    this.pending = { frame, timestampMs };
    return true;
  }

  take(): TimestampedFrame<T> | null {
    const value = this.pending;
    this.pending = null;
    return value;
  }

  reset(): void {
    this.pending?.frame.close();
    this.pending = null;
    this.newestTimestampMs = Number.NEGATIVE_INFINITY;
  }
}
