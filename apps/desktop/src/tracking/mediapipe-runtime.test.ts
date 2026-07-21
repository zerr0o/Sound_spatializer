import { describe, expect, it } from 'vitest';
import { createLocalVisionFileset, installMediaPipeModuleFactory, supportsMediaPipeWasm } from './mediapipe-runtime';

describe('runtime MediaPipe local', () => {
  it('utilise le chargeur ES6 incorporé sans import dynamique depuis public', () => {
    expect(createLocalVisionFileset('/mediapipe/')).toEqual({
      wasmLoaderPath: '',
      wasmBinaryPath: '/mediapipe/vision_wasm_module_internal.wasm',
    });
  });

  it('réinstalle la factory avant chaque création de tâche', () => {
    const scope = { Module: { stale: true } } as unknown as WorkerGlobalScope;
    installMediaPipeModuleFactory(scope);
    expect((scope as unknown as { Module?: unknown }).Module).toBeUndefined();
    expect((scope as unknown as { ModuleFactory?: unknown }).ModuleFactory).toBeTypeOf('function');
  });

  it('refuse une racine de ressources vide', () => {
    expect(() => createLocalVisionFileset('///')).toThrow('chemin local');
  });

  it('détecte proprement un WebView sans WebAssembly SIMD', () => {
    expect(supportsMediaPipeWasm(() => true)).toBe(true);
    expect(supportsMediaPipeWasm(() => false)).toBe(false);
    expect(
      supportsMediaPipeWasm(() => {
        throw new Error('WebAssembly indisponible');
      }),
    ).toBe(false);
  });
});
