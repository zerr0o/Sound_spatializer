import { describe, expect, it } from 'vitest';
import { isSpeakerRouted, pairedSpeakerChannel, SPEAKER_CHANNELS } from './speaker-layout';

describe('implantation multicanale', () => {
  it('verrouille les cinq canaux positionnels dans l’ordre canonique', () => {
    expect(SPEAKER_CHANNELS).toEqual(['L', 'R', 'C', 'LS', 'RS']);
  });

  it('lie uniquement les paires gauche/droite correspondantes', () => {
    expect(pairedSpeakerChannel('L')).toBe('R');
    expect(pairedSpeakerChannel('R')).toBe('L');
    expect(pairedSpeakerChannel('LS')).toBe('RS');
    expect(pairedSpeakerChannel('RS')).toBe('LS');
    expect(pairedSpeakerChannel('C')).toBeNull();
  });

  it('ne route que L/R en stéréo et les cinq enceintes en 5.1', () => {
    expect(SPEAKER_CHANNELS.filter((channel) => isSpeakerRouted('stereo', channel))).toEqual(['L', 'R']);
    expect(SPEAKER_CHANNELS.every((channel) => isSpeakerRouted('5.1-surround', channel))).toBe(true);
  });
});
