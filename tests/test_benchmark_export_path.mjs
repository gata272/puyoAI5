import fs from 'node:fs';
import assert from 'node:assert/strict';

const debug = fs.readFileSync(new URL('../debug-mode.js', import.meta.url), 'utf8');
const sw = fs.readFileSync(new URL('../sw.js', import.meta.url), 'utf8');
const workflow = fs.readFileSync(new URL('../.github/workflows/build-wasm.yml', import.meta.url), 'utf8');
const benchmarkWorker = fs.readFileSync(new URL('../benchmark-worker.js', import.meta.url), 'utf8');
const exportWorker = fs.readFileSync(new URL('../benchmark-export-worker.js', import.meta.url), 'utf8');

assert.ok(debug.includes("new Worker('./benchmark-export-worker.js', { type: 'module' })"));
assert.ok(debug.includes("worker.postMessage({ type: 'startExport', runId, result })"));
assert.ok(debug.includes("type: 'appendGame'"));
assert.ok(debug.includes("jsonBytes: buffer"));
assert.ok(debug.includes("readBenchmarkGame(runId, gameIndex)"));
assert.ok(debug.includes('navigator.share'));
assert.ok(debug.includes('90000);'));
assert.ok(!debug.includes("STATE.worker.postMessage({ type: 'exportZip', runId })"));
assert.ok(!debug.includes('benchmark-export.html'));

assert.ok(exportWorker.includes("import { createBenchmarkZipBuilder } from './benchmark-zip.js';"));
assert.ok(exportWorker.includes("msg.type === 'startExport'"));
assert.ok(exportWorker.includes("msg.type === 'appendGame'"));
assert.ok(exportWorker.includes("msg.type === 'finishExport'"));
assert.ok(!exportWorker.includes('indexedDB'));

assert.ok(benchmarkWorker.includes("msg.type === 'gameStored'"));
assert.ok(!benchmarkWorker.includes('createBenchmarkZip'));
assert.ok(!benchmarkWorker.includes('indexedDB'));
assert.ok(!benchmarkWorker.includes("msg.type === 'exportZip'"));

assert.ok(sw.includes('benchmark-export-worker.js'));
assert.ok(sw.includes('benchmark-zip.js'));
assert.ok(sw.includes('puyo-sim-v12-inline-benchmark-export-dedicated-worker'));
assert.ok(!sw.includes('benchmark-export.html'));
assert.ok(!sw.includes('benchmark-export.js'));

assert.ok(workflow.includes('node tests/test_benchmark_zip.mjs'));
assert.ok(workflow.includes('node tests/test_benchmark_export_path.mjs'));
assert.ok(workflow.includes('test -f _site/benchmark-export-worker.js'));

console.log('benchmark export path regression: OK');
