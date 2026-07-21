import moduleFactory from '@mediapipe/tasks-vision/vision_wasm_module_internal.js';

type MediaPipeModuleFactory = typeof moduleFactory;

interface MediaPipeWorkerScope {
  ModuleFactory?: MediaPipeModuleFactory;
  Module?: unknown;
}

export interface LocalVisionFileset {
  wasmLoaderPath: string;
  wasmBinaryPath: string;
}

// Probe SIMD officielle de tasks-vision. Le chargeur ES6 fourni par MediaPipe
// cible SIMD ; WebView2 Evergreen (cible Windows 11) le prend en charge.
const wasmSimdProbe = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11,
]);

const trimTrailingSlashes = (value: string) => value.replace(/\/+$/, '');

export const supportsMediaPipeWasm = (validate: (bytes: BufferSource) => boolean = WebAssembly.validate) => {
  try {
    return validate(wasmSimdProbe);
  } catch {
    return false;
  }
};

export const createLocalVisionFileset = (wasmRoot: string): LocalVisionFileset => {
  const normalizedRoot = trimTrailingSlashes(wasmRoot);
  if (!normalizedRoot) throw new Error('Le chemin local du runtime MediaPipe est vide.');
  return {
    // Le chargeur ES6 est incorporé au bundle du Worker. Une chaîne vide
    // empêche tasks-vision de tenter importScripts() ou import() depuis public/.
    wasmLoaderPath: '',
    wasmBinaryPath: `${normalizedRoot}/vision_wasm_module_internal.wasm`,
  };
};

export const installMediaPipeModuleFactory = (scope: object) => {
  const target = scope as MediaPipeWorkerScope;
  target.Module = undefined;
  target.ModuleFactory = moduleFactory;
};
