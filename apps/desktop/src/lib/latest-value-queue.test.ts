import { describe, expect, it } from 'vitest';
import { LatestValueQueue } from './latest-value-queue';

describe('LatestValueQueue', () => {
  it('conserve uniquement la pose la plus récente pendant un envoi lent', async () => {
    const consumed: number[] = [];
    let releaseFirst: (() => void) | undefined;
    const firstBlocked = new Promise<void>((resolve) => { releaseFirst = resolve; });
    const queue = new LatestValueQueue<number>(async (value) => {
      consumed.push(value);
      if (value === 1) await firstBlocked;
    });

    queue.push(1);
    queue.push(2);
    queue.push(3);
    await Promise.resolve();
    expect(consumed).toEqual([1]);
    releaseFirst?.();
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(consumed).toEqual([1, 3]);
  });

  it('pipeline un nombre borné d’envois puis remplace la seule valeur en attente', async () => {
    const consumed: number[] = [];
    const releases: Array<() => void> = [];
    const queue = new LatestValueQueue<number>(
      async (value) => {
        consumed.push(value);
        await new Promise<void>((resolve) => releases.push(resolve));
      },
      { maxInFlight: 3 },
    );

    for (let value = 1; value <= 7; value += 1) queue.push(value);
    await Promise.resolve();
    expect(consumed).toEqual([1, 2, 3]);

    releases.shift()?.();
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(consumed).toEqual([1, 2, 3, 7]);
    expect(releases).toHaveLength(3);
  });

  it('signale les échecs au lieu de produire un rejet non observé', async () => {
    const failures: unknown[] = [];
    const queue = new LatestValueQueue<number>(
      async () => { throw new Error('IPC indisponible'); },
      { onSettled: (error) => { if (error) failures.push(error); } },
    );

    queue.push(1);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(failures).toHaveLength(1);
    expect(failures[0]).toBeInstanceOf(Error);
  });

  it('continue à vider la file si un observateur de résultat lance une exception', async () => {
    const consumed: number[] = [];
    const queue = new LatestValueQueue<number>(
      async (value) => { consumed.push(value); },
      { maxInFlight: 1, onSettled: () => { throw new Error('observateur défaillant'); } },
    );

    queue.push(1);
    queue.push(2);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(consumed).toEqual([1, 2]);
  });
});
