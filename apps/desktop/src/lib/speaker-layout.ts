import type { Channel, InputLayout } from '../types/contracts';

export const SPEAKER_CHANNELS: readonly Channel[] = ['L', 'R', 'C', 'LS', 'RS'];

export const pairedSpeakerChannel = (channel: Channel): Channel | null => {
  if (channel === 'L') return 'R';
  if (channel === 'R') return 'L';
  if (channel === 'LS') return 'RS';
  if (channel === 'RS') return 'LS';
  return null;
};
export const isSpeakerRouted = (layout: InputLayout, channel: Channel): boolean =>
  layout === '5.1-surround' || channel === 'L' || channel === 'R';

export const SPEAKER_COLORS: Record<Channel, string> = {
  L: '#67ead3',
  R: '#7ea8ff',
  C: '#ffc66d',
  LS: '#b794f6',
  RS: '#f08cc4',
};
