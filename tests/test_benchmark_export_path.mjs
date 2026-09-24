import fs from 'node:fs';
import assert from 'node:assert/strict';

const debug = fs.readFileSync(new URL('../debug-mode.js', import.meta.url), 'utf8');
const sw = fs.readFileSync(new URL('../sw.js', import.meta.url), 'utf8');
const workflow = fs.readFileSync(new URL('../.github/workflows/build-wasm.yml', import.meta.url), 'utf8');

assert.ok(debug.includes("new URL('./benchmark-zip.js', document.baseURI).href"));
assert.ok(debug.includes("global.downloadBenchmarkLog = function ()"));
assert.ok(debug.includes("textContent = 'ZIPを保存'"));
assert.ok(debug.includes("textContent = '共有 / ファイルに保存'"));
assert.ok(!debug.includes('benchmark-export.html'));
assert.ok(!sw.includes('benchmark-export.html'));
assert.ok(!sw.includes('benchmark-export.js'));
assert.ok(workflow.includes('test -f _site/benchmark-zip.js'));
assert.ok(workflow.includes('stale benchmark-export.html reference remains'));

console.log('benchmark export path regression: OK');
