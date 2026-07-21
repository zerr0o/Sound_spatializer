import { describe, expect, it } from 'vitest';
import {
  MediaTimestampMapper,
  videoCallbackTimestampToPerformance,
  videoFrameTimestampToPerformance,
} from './frame-timestamps';

describe('horodatage des frames caméra', () => {
  it('convertit les microsecondes VideoFrame déjà alignées sur performance.now()', () => {
    const clock = new MediaTimestampMapper();
    expect(videoFrameTimestampToPerformance(12_480_000, 12_500, clock, 1_700_000_000_000)).toBe(12_480);
  });

  it('convertit un timestamp epoch puis ancre une timeline relative sans jitter de lecture', () => {
    const clock = new MediaTimestampMapper();
    const timeOrigin = 1_700_000_000_000;
    expect(videoFrameTimestampToPerformance((timeOrigin + 4_980) * 1_000, 5_000, clock, timeOrigin)).toBe(4_980);

    expect(videoFrameTimestampToPerformance(1_000_000, 8_000, clock, timeOrigin)).toBe(8_000);
    expect(videoFrameTimestampToPerformance(1_016_000, 8_035, clock, timeOrigin)).toBe(8_016);
  });

  it('préfère captureTime puis receiveTime dans les métadonnées vidéo', () => {
    const clock = new MediaTimestampMapper();
    const base = {
      mediaTime: 2,
      presentationTime: 5_010,
      expectedDisplayTime: 5_016,
      processingDuration: 0.003,
    };
    expect(videoCallbackTimestampToPerformance(5_012, { ...base, captureTime: 4_990, receiveTime: 5_000 }, clock)).toBe(4_990);
    expect(videoCallbackTimestampToPerformance(5_012, { ...base, receiveTime: 5_000 }, clock)).toBe(5_000);
  });

  it('mappe mediaTime via presentationTime quand aucune capture directe n’est fournie', () => {
    const clock = new MediaTimestampMapper();
    const first = videoCallbackTimestampToPerformance(9_020, {
      mediaTime: 1,
      presentationTime: 9_010,
      expectedDisplayTime: 9_025,
      processingDuration: 0.002,
    }, clock);
    const second = videoCallbackTimestampToPerformance(9_050, {
      mediaTime: 1.016,
      presentationTime: 9_045,
      expectedDisplayTime: 9_058,
      processingDuration: 0.002,
    }, clock);
    expect(first).toBe(9_008);
    expect(second).toBeCloseTo(9_024, 6);
  });
});
