interface FaceLandmarkLike {
  visibility?: number;
}

/**
 * Face Landmarker ne fournit pas de score de présence facial exploitable dans
 * ses landmarks. Selon le runtime, `visibility` peut rester à zéro alors que
 * la matrice faciale et tous les landmarks sont valides. La présence conjointe
 * de ces deux sorties est donc le signal de confiance V1.
 */
export function faceTrackingConfidence(
  matrix: readonly number[] | null | undefined,
  landmarks: readonly FaceLandmarkLike[] | null | undefined,
): number {
  if (!matrix || matrix.length < 16 || matrix.some((value) => !Number.isFinite(value))) return 0;
  return landmarks?.length ? 1 : 0;
}
