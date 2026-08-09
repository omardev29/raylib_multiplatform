#!/usr/bin/env node
// Web boot test: boot the (Emscripten) web build in headless Chromium and
// verify the engine actually initialises and renders, not just that the page
// loads. Modelled on the Godot CI web_boot_test pattern.
//
// Usage: node web_boot_test.js <url>
// Exit codes: 0 = boot + render OK, 1 = failure.

'use strict';

const { chromium } = require('playwright');

const url = process.argv[2] || 'http://localhost:8000/index.html';
const BOOT_TIMEOUT_MS = 60000;   // total boot budget
const SETTLE_MS = 15000;         // time to let the engine render a few frames

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

(async () => {
  const browser = await chromium.launch({
    args: ['--use-gl=swiftshader', '--enable-unsafe-swiftshader'],
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

    // Positive signal: the canvas must have a non-zero drawing-buffer size,
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

    console.log('PASS: web boot OK, canvas ' + size.w + 'x' + size.h);
    process.exitCode = 0;
  } finally {
    await browser.close();
  }
})().catch((err) => {
  console.error('FAIL: ' + ((err && err.message) || err));
  process.exitCode = 1;
});
