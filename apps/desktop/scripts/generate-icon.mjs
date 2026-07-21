import { mkdir, writeFile } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const size = 32;
const pixels = Buffer.alloc((size * 4 + 1) * size);
for (let y = 0; y < size; y += 1) {
  pixels[y * (size * 4 + 1)] = 0;
  for (let x = 0; x < size; x += 1) {
    const offset = y * (size * 4 + 1) + 1 + x * 4;
    const rounded = Math.hypot(Math.max(0, Math.abs(x - 15.5) - 11), Math.max(0, Math.abs(y - 15.5) - 11)) <= 4;
    const wave = [10, 15, 20].includes(x) && Math.abs(y - 15.5) <= (x === 15 ? 9 : 5);
    const border = rounded && (x < 3 || x > 28 || y < 3 || y > 28);
    pixels[offset] = wave ? 104 : border ? 22 : 8;
    pixels[offset + 1] = wave ? 229 : border ? 43 : 15;
    pixels[offset + 2] = wave ? 207 : border ? 52 : 21;
    pixels[offset + 3] = rounded ? 255 : 0;
  }
}

const crcTable = Array.from({ length: 256 }, (_, value) => {
  let crc = value;
  for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  return crc >>> 0;
});
const crc32 = (buffer) => {
  let crc = 0xffffffff;
  for (const byte of buffer) crc = (crc >>> 8) ^ crcTable[(crc ^ byte) & 0xff];
  return (crc ^ 0xffffffff) >>> 0;
};
const chunk = (type, data) => {
  const name = Buffer.from(type);
  const length = Buffer.alloc(4); length.writeUInt32BE(data.length);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([name, data])));
  return Buffer.concat([length, name, data, crc]);
};
const header = Buffer.alloc(13);
header.writeUInt32BE(size, 0); header.writeUInt32BE(size, 4); header[8] = 8; header[9] = 6;
const png = Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk('IHDR', header), chunk('IDAT', deflateSync(pixels)), chunk('IEND', Buffer.alloc(0)),
]);
const icoHeader = Buffer.alloc(22);
icoHeader.writeUInt16LE(0, 0); icoHeader.writeUInt16LE(1, 2); icoHeader.writeUInt16LE(1, 4);
icoHeader[6] = size; icoHeader[7] = size; icoHeader.writeUInt16LE(1, 10); icoHeader.writeUInt16LE(32, 12);
icoHeader.writeUInt32LE(png.length, 14); icoHeader.writeUInt32LE(22, 18);
const target = join(dirname(fileURLToPath(import.meta.url)), '..', 'src-tauri', 'icons', 'icon.ico');
await mkdir(dirname(target), { recursive: true });
await writeFile(target, Buffer.concat([icoHeader, png]));
console.info(`Icône générée : ${target}`);
