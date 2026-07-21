export const HIGH_FRAME_RATE_VIDEO_CONSTRAINTS = {
  width: { ideal: 640, max: 640 },
  height: { ideal: 360, max: 360 },
  aspectRatio: { ideal: 16 / 9 },
  frameRate: { min: 55, ideal: 60, max: 60 },
  facingMode: { ideal: 'user' },
} satisfies MediaTrackConstraints;

export const COMPATIBLE_VIDEO_CONSTRAINTS = {
  width: { ideal: 640 },
  height: { ideal: 480 },
  frameRate: { min: 30, ideal: 60 },
  facingMode: { ideal: 'user' },
} satisfies MediaTrackConstraints;

type GetUserMedia = (constraints: MediaStreamConstraints) => Promise<MediaStream>;

const errorName = (error: unknown): string | null => {
  if (!error || typeof error !== 'object' || !('name' in error)) return null;
  return typeof error.name === 'string' ? error.name : null;
};

export const isUnsupportedCameraModeError = (error: unknown): boolean => {
  const name = errorName(error);
  // WebView2/Media Foundation remonte parfois NotReadableError lorsqu'un
  // format strict (notamment 60 fps) n'est pas exposé par le pilote, même si
  // la caméra elle-même fonctionne dans un mode 30 fps.
  return name === 'OverconstrainedError' || name === 'NotFoundError' || name === 'NotReadableError';
};

/**
 * Prefer a light native 55–60 fps mode. A compatibility request is issued
 * only when that mode is unavailable. Permission/security failures are
 * returned immediately, so the browser never prompts twice after a refusal.
 */
export async function requestTrackingCamera(getUserMedia: GetUserMedia): Promise<MediaStream> {
  try {
    return await getUserMedia({ video: HIGH_FRAME_RATE_VIDEO_CONSTRAINTS, audio: false });
  } catch (error) {
    if (!isUnsupportedCameraModeError(error)) throw error;
    return getUserMedia({ video: COMPATIBLE_VIDEO_CONSTRAINTS, audio: false });
  }
}
