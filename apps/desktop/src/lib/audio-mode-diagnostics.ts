const FALLBACK_PREFIX = 'AUDIO_MODE_FALLBACK ';
const REASON_MARKER = ' reason=';
const MAX_DIAGNOSTIC_LENGTH = 240;

const sanitizeDiagnostic = (value: string): string => value
  .replace(/[\u0000-\u001f\u007f]+/g, ' ')
  .replace(/\s+/g, ' ')
  .trim()
  .slice(0, MAX_DIAGNOSTIC_LENGTH);

export function extractExclusiveFallbackReason(lastError: string | null): string | null {
  if (!lastError) return null;
  const fallbackStart = lastError.indexOf(FALLBACK_PREFIX);
  if (fallbackStart < 0) return null;
  const reasonStart = lastError.indexOf(REASON_MARKER, fallbackStart);
  if (reasonStart < 0) return null;
  const reason = sanitizeDiagnostic(lastError.slice(reasonStart + REASON_MARKER.length));
  return reason || null;
}

export function describeExclusiveFallback(lastError: string | null): string {
  const reason = extractExclusiveFallbackReason(lastError);
  if (!reason) return 'Le moteur est resté actif en faible latence partagée.';

  const normalized = reason.toLowerCase();
  if (normalized.includes('0x88890008') || normalized.includes('unsupported format') ||
      normalized.includes('does not support exclusive') || normalized.includes('supports neither exclusive')) {
    return 'Aucun des formats exclusifs proposés n’est accepté par cette sortie. Le moteur reste actif en faible latence partagée.';
  }
  if (normalized.includes('0x8889000a') || normalized.includes('device in use')) {
    return 'La sortie est déjà réservée par une autre application. Le moteur reste actif en faible latence partagée.';
  }
  if (normalized.includes('0x8889000e') || normalized.includes('exclusive mode not allowed')) {
    return 'Windows ou les propriétés du périphérique interdisent le contrôle exclusif. Le moteur reste actif en faible latence partagée.';
  }
  return `Le moteur reste actif en faible latence partagée. Diagnostic WASAPI : ${reason}`;
}
