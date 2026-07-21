import { describe, expect, it } from 'vitest';
import { describeExclusiveFallback, extractExclusiveFallbackReason } from './audio-mode-diagnostics';

describe('diagnostic du fallback exclusif', () => {
  it('extrait et nettoie la cause même après un autre avertissement', () => {
    const status = 'MMCSS unavailable; AUDIO_MODE_FALLBACK requested=exclusive-pro effective=shared-low-latency reason=initialize renderer failed (HRESULT 0x8889000A)\n';
    expect(extractExclusiveFallbackReason(status)).toBe('initialize renderer failed (HRESULT 0x8889000A)');
    expect(describeExclusiveFallback(status)).toContain('déjà réservée par une autre application');
  });

  it('explique un format non pris en charge', () => {
    const status = 'AUDIO_MODE_FALLBACK requested=exclusive-pro effective=shared-low-latency reason=the physical endpoint supports neither exclusive stereo float32/48 kHz nor signed PCM32/48 kHz';
    expect(describeExclusiveFallback(status)).toContain('Aucun des formats exclusifs');
  });

  it('explique le contrôle exclusif désactivé et garde un repli générique', () => {
    expect(describeExclusiveFallback('AUDIO_MODE_FALLBACK requested=exclusive-pro effective=shared-low-latency reason=HRESULT 0x8889000E'))
      .toContain('interdisent le contrôle exclusif');
    expect(describeExclusiveFallback(null)).toBe('Le moteur est resté actif en faible latence partagée.');
  });
});
