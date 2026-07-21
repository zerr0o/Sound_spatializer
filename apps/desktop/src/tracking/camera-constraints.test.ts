import { describe, expect, it, vi } from 'vitest';
import {
  COMPATIBLE_VIDEO_CONSTRAINTS,
  HIGH_FRAME_RATE_VIDEO_CONSTRAINTS,
  requestTrackingCamera,
} from './camera-constraints';

const stream = { id: 'camera-stream' } as MediaStream;
const namedError = (name: string) => Object.assign(new Error(name), { name });

describe('négociation de la caméra de tracking', () => {
  it('demande en priorité un mode léger strictement supérieur à 55 i/s', async () => {
    const getUserMedia = vi.fn().mockResolvedValue(stream);

    await expect(requestTrackingCamera(getUserMedia)).resolves.toBe(stream);
    expect(getUserMedia).toHaveBeenCalledOnce();
    expect(getUserMedia).toHaveBeenCalledWith({ video: HIGH_FRAME_RATE_VIDEO_CONSTRAINTS, audio: false });
    expect(HIGH_FRAME_RATE_VIDEO_CONSTRAINTS).toMatchObject({
      width: { ideal: 640, max: 640 },
      height: { ideal: 360, max: 360 },
      frameRate: { min: 55, ideal: 60, max: 60 },
    });
  });

  it.each(['OverconstrainedError', 'NotFoundError', 'NotReadableError'])(
    'se replie sur le profil compatible après %s',
    async (name) => {
      const getUserMedia = vi.fn()
        .mockRejectedValueOnce(namedError(name))
        .mockResolvedValueOnce(stream);

      await expect(requestTrackingCamera(getUserMedia)).resolves.toBe(stream);
      expect(getUserMedia).toHaveBeenCalledTimes(2);
      expect(getUserMedia).toHaveBeenNthCalledWith(2, { video: COMPATIBLE_VIDEO_CONSTRAINTS, audio: false });
    },
  );

  it.each(['NotAllowedError', 'SecurityError'])(
    'ne redemande jamais la caméra après %s',
    async (name) => {
      const failure = namedError(name);
      const getUserMedia = vi.fn().mockRejectedValue(failure);

      await expect(requestTrackingCamera(getUserMedia)).rejects.toBe(failure);
      expect(getUserMedia).toHaveBeenCalledOnce();
    },
  );
});
