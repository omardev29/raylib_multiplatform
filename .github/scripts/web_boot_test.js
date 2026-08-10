#!/usr/bin/env node
// Web boot + render test: boot the (Emscripten) web build in headless Chromium
// and verify the engine actually PUTS PIXELS ON THE CANVAS — not merely that
// the page loads and a canvas element exists.
//
// Usage: node web_boot_test.js <url>
// Exit codes: 0 = boot + render OK, 1 = failure.
//
// Why the pixel check lives here and not in tests/smoke_test.h:
// the native targets gate on RAY_TEST_RENDER_OK, which the game emits itself
// when RAY_TEST_MAX_FRAMES is set. Emscripten's getenv() reads an internal ENV
// object that is only reachable from inside the generated module scope, so
// plumbing that variable in from outside would mean exporting emscripten
// runtime internals just for a test. Screenshotting the canvas is both simpler
// and a stronger claim: it checks what the browser actually composited, which
// is what a player would see.

'use strict';

const { chromium } = require('playwright');
const { PNG } = require('pngjs');

const url = process.argv[2] || 'http://localhost:8000/index.html';
const BOOT_TIMEOUT_MS = 60000;   // total boot budget
const SETTLE_MS = 15000;         // time to let the engine render a few frames

// Same thresholds as the native gate in tests/smoke_test.h: enough non-
// background pixels to prove something was drawn, but not so many that the
// "background" is itself the anomaly (a fully garbage frame is just as broken
// as a blank one).
const MIN_RATIO = 0.0005;
const MAX_RATIO = 0.98;

// Console "error" messages matching these are expected noise on a headless
// browser (no real audio device / GPU / gamepad) and must not fail the test.
const BENIGN = [
  /webgl/i,
  /audio/i,
  /alsa/i,
  /pulse/i,
  /no sound/i,
  /gamepad/i,
  /autoplay/i,
];

// Fraction of pixels that are NOT the background colour.
//
// "Background" is the most common colour in the frame, not the corner pixel.
// The corner is the obvious choice and it is wrong here: the composited canvas
// carries a few pixels of border that match nothing else on screen, so keying
// off (0,0) reported 99% of the frame as "content" on a perfectly good build.
// The modal colour is what ClearBackground actually painted, whatever the
// canvas does around the edges.
//
// Colours are bucketed to RGB565, which both makes the histogram a flat 64K
// array and folds in a tolerance for dithering for free.
function contentRatio(pngBuffer) {
  const img = PNG.sync.read(pngBuffer);
  const d = img.data;                       // RGBA8
  const total = img.width * img.height;
  if (total === 0) return { ratio: 0, differing: 0, width: 0, height: 0 };

  const hist = new Uint32Array(65536);
  const key = (o) => ((d[o] >> 3) << 11) | ((d[o + 1] >> 2) << 5) | (d[o + 2] >> 3);

  for (let i = 0; i < total; i++) hist[key(i * 4)]++;

  let bg = 0;
  for (let k = 1; k < 65536; k++) if (hist[k] > hist[bg]) bg = k;

  const differing = total - hist[bg];
  return { ratio: differing / total, differing, width: img.width, height: img.height };
}

(async () => {
  const browser = await chromium.launch({
    // SwiftShader gives a real (software) GL implementation, so this exercises
    // the actual render path rather than a stub. --no-sandbox because CI may
    // run this as root inside a container.
    args: ['--use-gl=swiftshader', '--enable-unsafe-swiftshader', '--no-sandbox'],
  });

  try {
    const page = await browser.newPage();
    const errors = [];
    const logs = [];

    page.on('pageerror', (err) => errors.push('pageerror: ' + err.message));
    page.on('console', (msg) => {
      const text = msg.text();
      logs.push(msg.type() + ': ' + text);
      if (msg.type() === 'error' && !BENIGN.some((re) => re.test(text))) {
        errors.push('console.error: ' + text);
      }
    });
    page.on('requestfailed', (req) => {
      errors.push('requestfailed: ' + req.url() + ' ' + (req.failure() || {}).errorText);
    });

    await page.goto(url, { waitUntil: 'load', timeout: BOOT_TIMEOUT_MS });

    // Wait until the engine has created the canvas.
    const canvas = await page.waitForSelector('canvas', { timeout: BOOT_TIMEOUT_MS });

    // Give the engine time to initialise GL and render a few frames.
    await page.waitForTimeout(SETTLE_MS);

    // Positive signal 1: the canvas must have a non-zero drawing-buffer size,
    // which only happens once the engine has initialised its viewport.
    const size = await page.evaluate(() => {
      const c = document.querySelector('canvas');
      if (!c) return { w: 0, h: 0 };
      return { w: c.width, h: c.height };
    });

    // Echo a slice of the captured console log for diagnosis.
    console.log('---- captured console log (last 40 lines) ----');
    logs.slice(-40).forEach((l) => console.log(l));
    console.log('----------------------------------------------');

    if (errors.length > 0) {
      console.error('FAIL: errors during web boot:');
      errors.forEach((e) => console.error('  ' + e));
      process.exitCode = 1;
      return;
    }
    if (!size || size.w === 0 || size.h === 0) {
      console.error('FAIL: canvas has zero size (' + (size && size.w) + 'x' + (size && size.h) + '); engine did not initialise the viewport');
      process.exitCode = 1;
      return;
    }

    // Positive signal 2: the composited canvas actually has content. Element
    // screenshots go through the browser compositor, so this works on a WebGL
    // canvas without preserveDrawingBuffer — unlike canvas.toDataURL(), which
    // would come back blank.
    const shot = await canvas.screenshot({ type: 'png' });
    // Keep the frame around: a ratio in a log line tells you the check failed,
    // the actual image tells you why.
    try { require('fs').writeFileSync('/tmp/web_canvas.png', shot); } catch (_) { /* best effort */ }
    const { ratio, differing, width, height } = contentRatio(shot);
    console.log(
      'render: ' + width + 'x' + height +
      ' non-background pixels=' + differing +
      ' ratio=' + ratio.toFixed(5));

    if (!(ratio > MIN_RATIO && ratio < MAX_RATIO)) {
      console.error(
        'FAIL: RAY_TEST_RENDER_FAIL canvas is ' +
        (ratio <= MIN_RATIO ? 'blank' : 'uniformly non-background') +
        ' (ratio=' + ratio.toFixed(5) + '); the engine booted but did not draw');
      process.exitCode = 1;
      return;
    }

    console.log('PASS: RAY_TEST_RENDER_OK web boot + render, canvas ' + size.w + 'x' + size.h);
    process.exitCode = 0;
  } finally {
    await browser.close();
  }
})().catch((err) => {
  console.error('FAIL: ' + ((err && err.message) || err));
  process.exitCode = 1;
});
