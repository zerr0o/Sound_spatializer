import { describe, expect, it, vi } from 'vitest';
import { LatestFrameSlot } from './latest-frame-slot';

const frame = () => ({ close: vi.fn() });

describe('LatestFrameSlot', () => {
  it('remplace et ferme les images intermédiaires pendant une inférence lente', () => {
    const slot = new LatestFrameSlot<ReturnType<typeof frame>>();
    const first = frame();
    const latest = frame();

    expect(slot.offer(first, 10)).toBe(true);
    expect(slot.offer(latest, 20)).toBe(true);
    expect(first.close).toHaveBeenCalledOnce();
    expect(slot.take()).toEqual({ frame: latest, timestampMs: 20 });
    expect(latest.close).not.toHaveBeenCalled();
  });

  it('rejette une conversion terminée hors ordre', () => {
    const slot = new LatestFrameSlot<ReturnType<typeof frame>>();
    const latest = frame();
    const stale = frame();

    expect(slot.offer(latest, 40)).toBe(true);
    expect(slot.offer(stale, 30)).toBe(false);
    expect(stale.close).toHaveBeenCalledOnce();
    expect(slot.take()?.frame).toBe(latest);
  });

  it('ferme la frame en attente et réinitialise la timeline à l’arrêt', () => {
    const slot = new LatestFrameSlot<ReturnType<typeof frame>>();
    const pending = frame();
    const nextSession = frame();

    slot.offer(pending, 1_000);
    slot.reset();
    expect(pending.close).toHaveBeenCalledOnce();
    expect(slot.offer(nextSession, 5)).toBe(true);
  });
});
