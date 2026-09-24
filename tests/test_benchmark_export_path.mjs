import fs from 'node:fs';
import assert from 'node:assert/strict';

const debug = fs.readFileSync(new URL('../debug-mode.js', import.meta.url), 'utf8');
const sw = fs.readFileSync(new URL('../sw.js', import.meta.url), 'utf8');
const workflow = fs.readFileSync(new URL('../.github/workflows/build-wasm.yml', import.meta.url), 'utf8');
const worker = fs.readFileSync(new URL('../benchmark-worker.js', import.meta.url), 'utf8');

assert.ok(debug.includes("STATE.worker.postMessage({ type: 'exportZip', runId })"));
assert.ok(debug.includes("global.downloadBenchmarkLog = function ()"));
assert.ok(debug.includes("textContent = 'ZIPを保存 / ファイルに保存'"));
assert.ok(debug.includes("type: 'exportZip'"));
assert.ok(debug.includes("navigator.share"));
assert.ok(!debug.includes('benchmark-export.html'));
assert.ok(sw.includes('benchmark-worker.js'));
assert.ok(sw.includes('benchmark-zip.js'));
assert.ok(sw.includes('puyo-sim-v11-inline-benchmark-export-worker'));
assert.ok(!sw.includes('benchmark-export.html'));
assert.ok(!sw.includes('benchmark-export.js'));
assert.ok(workflow.includes('test -f _site/benchmark-zip.js'));
assert.ok(workflow.includes('stale benchmark-export.html reference remains'));
assert.ok(worker.includes("import { createBenchmarkZip } from './benchmark-zip.js';"));
assert.ok(worker.includes("type: 'exportComplete'"));
assert.ok(worker.includes("type: 'exportError'"));

console.log('benchmark export path regression: OK');
