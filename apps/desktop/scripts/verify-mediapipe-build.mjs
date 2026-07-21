import { createHash } from 'node:crypto';
import { readdir, readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');
const assetFiles = await readdir(join(dist, 'assets'));
const workerFiles = assetFiles.filter((name) => /^face-landmarker\.worker-.*\.js$/.test(name));
if (workerFiles.length !== 1) {
  throw new Error(`Un seul bundle Face Landmarker est attendu, reçu : ${workerFiles.join(', ') || 'aucun'}.`);
}

const worker = await readFile(join(dist, 'assets', workerFiles[0]), 'utf8');
for (const forbidden of [
  /\/mediapipe\/[^"']+\.js(?:\?import)?/,
  /vision_wasm_(?:nosimd_)?internal\.js/,
  /127\.0\.0\.1:1420/,
]) {
  if (forbidden.test(worker)) throw new Error(`Import interdit dans le Worker MediaPipe : ${forbidden}.`);
}
if (!worker.includes('vision_wasm_module_internal.wasm') || !worker.includes('ModuleFactory')) {
  throw new Error('Le Worker ne contient pas le chargeur ES6 MediaPipe et son chemin WASM local.');
}

const runtimeFiles = await readdir(join(dist, 'mediapipe'));
const expectedRuntime = ['README.md', 'vision_wasm_module_internal.wasm'];
if (runtimeFiles.sort().join('\n') !== expectedRuntime.sort().join('\n')) {
  throw new Error(`Ressources MediaPipe inattendues : ${runtimeFiles.join(', ')}.`);
}

const digest = (bytes) => createHash('sha256').update(bytes).digest('hex');
const sourceWasm = await readFile(
  join(root, 'node_modules', '@mediapipe', 'tasks-vision', 'wasm', 'vision_wasm_module_internal.wasm'),
);
const packagedWasm = await readFile(join(dist, 'mediapipe', 'vision_wasm_module_internal.wasm'));
if (digest(sourceWasm) !== digest(packagedWasm)) {
  throw new Error('Le binaire WASM MediaPipe empaqueté diffère de la dépendance épinglée.');
}

console.info(`Bundle MediaPipe hors ligne vérifié : ${workerFiles[0]}.`);
