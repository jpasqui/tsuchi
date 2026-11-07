// Traces the logo's mark into vector shapes, so that it can be shown at any size.
//
//   node scripts/logo-vector.mjs <the logo as a PNG> <where to write the SVG>
//
// The picture is flat colors, so each color's outline is traced. The background's color is
// left out. Look at the result beside the picture before using it.
import { writeFileSync } from 'node:fs';
import ImageTracer from 'imagetracerjs';
import sharp from 'sharp';

const [source, target] = process.argv.slice(2);
const SCALE = 4; // traced from an enlarged copy, which gives smoother curves

const whole = await sharp(source).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
const { width, height } = whole.info;
const px = whole.data;
const back = [px[0], px[1], px[2]];
const far = (i) => Math.hypot(px[i] - back[0], px[i + 1] - back[1], px[i + 2] - back[2]);

// The mark's box: rows down to the tall band of background above the wordmark, as logo.mjs finds it.
let top = -1, bottom = -1, left = width, right = 0;
for (let y = 0, gap = 0; y < height; y += 1) {
  let ink = false;
  for (let x = 0; x < width; x += 1) {
    if (far(4 * (y * width + x)) > 40) {
      ink = true;
      if (top < 0 || gap <= 30) {
        left = Math.min(left, x);
        right = Math.max(right, x);
      }
    }
  }
  if (ink) {
    if (top < 0) top = y;
    else if (gap > 30) break;
    gap = 0;
    bottom = y;
  } else if (top >= 0) gap += 1;
}
const pad = 4;
const box = { left: left - pad, top: top - pad, width: right - left + 1 + 2 * pad, height: bottom - top + 1 + 2 * pad };

const big = await sharp(source)
  .extract(box)
  .resize(box.width * SCALE, box.height * SCALE, { kernel: 'lanczos3' })
  .ensureAlpha()
  .raw()
  .toBuffer({ resolveWithObject: true });

// The logo's seven flat colors: the background, the three layers, the top layer's shaded side,
// the leaf and its veins. With only these, the blended pixels along each edge go to the nearer
// color and no slivers are traced.
const palette = [back, [152, 104, 73], [180, 133, 89], [211, 174, 125], [118, 80, 57], [101, 139, 66], [132, 160, 89]].map(
  ([r, g, b]) => ({ r, g, b, a: 255 }),
);

const svg = ImageTracer.imagedataToSVG(
  { width: big.info.width, height: big.info.height, data: big.data },
  { pal: palette, colorsampling: 0, colorquantcycles: 1, ltres: 0.5, qtres: 0.5, pathomit: 24, blurradius: 0, roundcoords: 1, linefilter: true, strokewidth: 0, viewbox: true, desc: false },
);

// Paths of the background's color go, and the document is sized as the picture was drawn.
const kept = svg.replace(/<path [^>]*\/>/g, (path) => {
  const fill = /fill="rgb\((\d+),(\d+),(\d+)\)"/.exec(path);
  if (!fill) return path;
  const d = Math.hypot(fill[1] - back[0], fill[2] - back[1], fill[3] - back[2]);
  return d < 28 ? '' : path.replace(/ stroke="[^"]*"/, '').replace(/ stroke-width="[^"]*"/, '');
});
writeFileSync(target, kept.replace(/<svg /, `<svg width="${box.width}" height="${box.height}" `));
console.log(`traced ${box.width}x${box.height}, ${(kept.match(/<path /g) || []).length} shapes, ${kept.length} bytes`);
