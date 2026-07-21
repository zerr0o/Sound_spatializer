import { describe, expect, it } from 'vitest';
import { MonotonicPoseSequence, poseSequenceSeed, type PoseSequenceState } from './pose-sequence';

describe('séquence monotone de pose', () => {
  it('utilise un seed absolu en microsecondes inférieur à MAX_SAFE_INTEGER', () => {
    const seed = poseSequenceSeed(1_784_640_000_000, 12_345.678);
    expect(seed).toBe(1_784_640_012_345_678);
    expect(Number.isSafeInteger(seed)).toBe(true);
  });

  it('reste strictement croissante entre deux remontages partageant le même contexte', () => {
    const state: PoseSequenceState = { last: 0 };
    const firstMount = new MonotonicPoseSequence(state, 10_000);
    expect(firstMount.next()).toBe(10_000);
    expect(firstMount.next()).toBe(10_001);

    const hotReload = new MonotonicPoseSequence(state, 10_000);
    expect(hotReload.next()).toBe(10_002);
  });

  it('démarre après une ancienne page grâce au seed absolu lors d’un rechargement complet', () => {
    const oldPage = new MonotonicPoseSequence({ last: 0 }, 20_000);
    const previous = oldPage.next();
    const reloadedPage = new MonotonicPoseSequence({ last: 0 }, 21_000);
    expect(reloadedPage.next()).toBeGreaterThan(previous);
  });

  it('préfère le dernier numéro partagé si l’horloge fournie est plus ancienne', () => {
    const state: PoseSequenceState = { last: 30_000 };
    expect(new MonotonicPoseSequence(state, 1).next()).toBe(30_001);
  });
});
