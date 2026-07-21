import { access, copyFile, mkdir, readdir, readFile, rename, rm } from 'node:fs/promises';
import { createWriteStream } from 'node:fs';
import { createHash } from 'node:crypto';
import { basename, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const wasmSource = join(root, 'node_modules', '@mediapipe', 'tasks-vision', 'wasm');
const wasmTarget = join(root, 'public', 'mediapipe');
const modelTarget = join(root, 'public', 'models', 'face_landmarker.task');
const modelTemporary = `${modelTarget}.download`;
const modelUrl =
  'https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task';
const modelBytes = 3_758_596;
const modelSha256 = '64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff';
const repositoryRoot = join(root, '..', '..');
const hrtfSource = join(repositoryRoot, 'resources', 'hrtf');
const hrtfTarget = join(root, 'src-tauri', 'resources', 'hrtf');
const legacyWebHrtfTarget = join(root, 'public', 'hrtf');

await mkdir(wasmTarget, { recursive: true });
await mkdir(dirname(modelTarget), { recursive: true });

const mediapipeRuntimeFiles = ['vision_wasm_module_internal.wasm'];
for (const filename of mediapipeRuntimeFiles) {
  await copyFile(join(wasmSource, filename), join(wasmTarget, filename));
}
for (const filename of await readdir(wasmTarget)) {
  if (/^vision_wasm.*\.(?:js|wasm)$/.test(filename) && !mediapipeRuntimeFiles.includes(filename)) {
    await rm(join(wasmTarget, filename), { force: true });
  }
}

const verifyModel = async (path) => {
  const data = await readFile(path);
  const digest = createHash('sha256').update(data).digest('hex');
  if (data.byteLength !== modelBytes || digest !== modelSha256) {
    throw new Error(`Modèle MediaPipe invalide (${data.byteLength} octets, SHA-256 ${digest}).`);
  }
};

let modelValid = false;
try {
  await access(modelTarget);
  await verifyModel(modelTarget);
  modelValid = true;
  console.info('Le modèle Face Landmarker vérifié est déjà présent.');
} catch {
  await rm(modelTarget, { force: true });
}

if (!modelValid) {
  console.info('Téléchargement du modèle officiel Face Landmarker…');
  const response = await fetch(modelUrl);
  if (!response.ok || !response.body) {
    throw new Error(`Téléchargement impossible (${response.status})`);
  }
  await rm(modelTemporary, { force: true });
  await pipeline(Readable.fromWeb(response.body), createWriteStream(modelTemporary));
  try {
    await verifyModel(modelTemporary);
    await rename(modelTemporary, modelTarget);
  } catch (error) {
    await rm(modelTemporary, { force: true });
    throw error;
  }
}

// Les SOFA sont lus exclusivement par le moteur natif. Les laisser sous `public/`
// les incorporerait inutilement au frontend Tauri en plus des ressources MSI.
await rm(legacyWebHrtfTarget, { recursive: true, force: true });
await mkdir(join(hrtfTarget, 'data'), { recursive: true });
const hrtfManifest = JSON.parse(await readFile(join(hrtfSource, 'profiles.json'), 'utf8'));
if (hrtfManifest?.schemaVersion !== 1 || !Array.isArray(hrtfManifest.profiles) || hrtfManifest.profiles.length !== 6) {
  throw new Error('Le manifeste HRTF doit contenir exactement six profils de schéma v1.');
}

const expectedOutputs = new Set();
for (const profile of hrtfManifest.profiles) {
  if (
    typeof profile.id !== 'string' ||
    typeof profile.output !== 'string' || basename(profile.output) !== profile.output ||
    !profile.output.toLowerCase().endsWith('.sofa') || expectedOutputs.has(profile.output) ||
    !Number.isSafeInteger(profile.sofaBytes) || profile.sofaBytes < 512 ||
    typeof profile.sofaSha256 !== 'string' || !/^[a-f0-9]{64}$/.test(profile.sofaSha256)
  ) {
    throw new Error(`Entrée HRTF invalide pour ${String(profile?.id ?? 'profil inconnu')}.`);
  }
  expectedOutputs.add(profile.output);
  const sourcePath = join(hrtfSource, 'data', profile.output);
  const bytes = await readFile(sourcePath);
  const digest = createHash('sha256').update(bytes).digest('hex');
  if (bytes.byteLength !== profile.sofaBytes || digest !== profile.sofaSha256) {
    throw new Error(`SOFA invalide pour ${profile.id} (${bytes.byteLength} octets, SHA-256 ${digest}).`);
  }
}

for (const filename of await readdir(join(hrtfTarget, 'data'))) {
  if (filename.toLowerCase().endsWith('.sofa') && !expectedOutputs.has(filename)) {
    await rm(join(hrtfTarget, 'data', filename), { force: true });
  }
}
for (const profile of hrtfManifest.profiles) {
  await copyFile(join(hrtfSource, 'data', profile.output), join(hrtfTarget, 'data', profile.output));
}
for (const filename of ['profiles.json', 'NOTICE.md']) {
  await copyFile(join(hrtfSource, filename), join(hrtfTarget, filename));
}

console.info('Ressources MediaPipe et HRTF locales prêtes.');
