// Makes the site's logo files from the logo's picture: the mark alone, on a clear background.
//
//   node scripts/logo.mjs <the logo as a PNG>
//
// It writes public/favicon.png, the tab's icon. The header and the first page use the traced
// shapes that scripts/logo-vector.mjs makes.
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';

const site = dirname(dirname(fileURLToPath(import.meta.url)));
const source = process.argv[2];
if (!source) {
  console.error('Give the logo as a PNG.');
  process.exit(1);
}

const { data, info } = await sharp(source).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
const { width, height } = info;
const at = (x, y) => 4 * (y * width + x);
const [br, bg, bb] = [data[0], data[1], data[2]]; // the background is the corner's color

// The wordmark under the mark is left out: only what is above the widest empty band is kept.
const rowHasInk = [];
for (let y = 0; y < height; y += 1) {
  let ink = false;
  for (let x = 0; x < width && !ink; x += 1) {
    const i = at(x, y);
    ink = Math.hypot(data[i] - br, data[i + 1] - bg, data[i + 2] - bb) > 40;
  }
  rowHasInk.push(ink);
}
const first = rowHasInk.indexOf(true);
let last = first;
for (let y = first, gap = 0; y < height; y += 1) {
  if (rowHasInk[y]) {
    if (gap > 30) break; // a band of background this tall is the space above the wordmark
    gap = 0;
    last = y;
  } else gap += 1;
}

let left = width, right = 0;
for (let y = first; y <= last; y += 1) {
  for (let x = 0; x < width; x += 1) {
    const i = at(x, y);
    const far = Math.hypot(data[i] - br, data[i + 1] - bg, data[i + 2] - bb);
    const alpha = Math.min(1, Math.max(0, (far - 10) / 30));
    // A pixel on an edge is the mark's color mixed with the background's. Take the background
    // back out, so that the edge has no pale rim on a dark page.
    if (alpha > 0 && alpha < 1) {
      for (const [c, back] of [[0, br], [1, bg], [2, bb]]) {
        data[i + c] = Math.round(Math.min(255, Math.max(0, (data[i + c] - (1 - alpha) * back) / alpha)));
      }
    }
    data[i + 3] = Math.round(255 * alpha);
    if (far > 40) {
      left = Math.min(left, x);
      right = Math.max(right, x);
    }
  }
}

const mark = await sharp(data, { raw: { width, height, channels: 4 } })
  .extract({ left, top: first, width: right - left + 1, height: last - first + 1 })
  .png()
  .toBuffer();
const side = Math.max(right - left + 1, last - first + 1);
const square = sharp(mark).resize(side, side, { fit: 'contain', background: { r: 0, g: 0, b: 0, alpha: 0 } });
await square.clone().resize(96, 96).png().toFile(join(site, 'public', 'favicon.png'));
console.log(`mark ${right - left + 1}x${last - first + 1} from rows ${first} to ${last}`);
