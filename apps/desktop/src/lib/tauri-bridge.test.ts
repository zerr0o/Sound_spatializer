import { invoke } from '@tauri-apps/api/core';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { desktopBridge } from './tauri-bridge';

vi.mock('@tauri-apps/api/core', () => ({
  invoke: vi.fn(),
}));

describe('accusés de réception du bridge moteur', () => {
  beforeEach(() => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', {
      configurable: true,
      value: {},
    });
    vi.mocked(invoke).mockReset();
  });

  afterEach(() => {
    Reflect.deleteProperty(window, '__TAURI_INTERNALS__');
  });

  it('ne réordonne jamais une commande dont le résultat est indéterminé', async () => {
    vi.mocked(invoke).mockRejectedValue(
      'ENGINE_COMMAND_OUTCOME_UNKNOWN commandId=11: confirmation tardive',
    );
    const warnings: string[] = [];
    const unsubscribe = desktopBridge.subscribeEngineCommandWarnings(
      (detail) => warnings.push(detail),
    );

    await expect(
      desktopBridge.sendCommand({ version: 1, type: 'stop' }),
    ).rejects.toMatch(/^ENGINE_COMMAND_OUTCOME_UNKNOWN /);
    expect(invoke).toHaveBeenCalledTimes(1);
    expect(warnings).toHaveLength(0);
    unsubscribe();
  });

  it('expose la génération ACK sans modifier le contrat void historique', async () => {
    vi.mocked(invoke)
      .mockResolvedValueOnce(17)
      .mockResolvedValueOnce(18);

    await expect(desktopBridge.sendCommandWithGeneration({
      version: 1,
      type: 'start',
    })).resolves.toBe(17);
    await expect(desktopBridge.sendCommand({
      version: 1,
      type: 'stop',
    })).resolves.toBeUndefined();
  });

  it('propage la génération zéro d’un moteur legacy', async () => {
    vi.mocked(invoke).mockResolvedValue(0);

    await expect(desktopBridge.sendCommandWithGeneration({
      version: 1,
      type: 'stop',
    })).resolves.toBe(0);
  });

  it('ne laisse pas une commande isolée entrelacer une transaction', async () => {
    let releaseFirst: (() => void) | undefined;
    vi.mocked(invoke)
      .mockImplementationOnce(() => new Promise<void>((resolve) => {
        releaseFirst = resolve;
      }))
      .mockResolvedValue(undefined);

    const transaction = desktopBridge.runCommandTransaction(async (send) => {
      await send({ version: 1, type: 'start' });
      await send({ version: 1, type: 'set-bypass', enabled: true });
    });
    await vi.waitFor(() => expect(invoke).toHaveBeenCalledTimes(1));
    const trailingStop = desktopBridge.sendCommand({ version: 1, type: 'stop' });
    expect(invoke).toHaveBeenCalledTimes(1);

    releaseFirst?.();
    await Promise.all([transaction, trailingStop]);

    const commandTypes = vi.mocked(invoke).mock.calls.map(([, args]) =>
      JSON.parse(String((args as { payload: string }).payload)).type,
    );
    expect(commandTypes).toEqual(['start', 'set-bypass', 'stop']);
  });

  it('considère appliquée une commande non persistée et remonte l’avertissement', async () => {
    vi.mocked(invoke).mockRejectedValue(
      'ENGINE_COMMAND_APPLIED_NOT_PERSISTED commandId=12: disque plein',
    );
    const warnings: string[] = [];
    const unsubscribe = desktopBridge.subscribeEngineCommandWarnings(
      (detail) => warnings.push(detail),
    );

    await expect(
      desktopBridge.sendCommand({ version: 1, type: 'start' }),
    ).resolves.toBeUndefined();
    expect(invoke).toHaveBeenCalledTimes(1);
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toMatch(
      /^ENGINE_COMMAND_APPLIED_NOT_PERSISTED /,
    );
    unsubscribe();
  });

  it('rend non fiable la génération d’une commande appliquée mais non persistée', async () => {
    vi.mocked(invoke).mockRejectedValue(
      'ENGINE_COMMAND_APPLIED_NOT_PERSISTED commandId=13: disque plein',
    );

    await expect(desktopBridge.sendCommandWithGeneration({
      version: 1,
      type: 'start',
    })).resolves.toBe(0);
  });
});
