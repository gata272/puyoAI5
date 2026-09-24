import assert from 'node:assert/strict';
import { createBenchmarkZip, crc32 } from '../benchmark-zip.js';

assert.equal(crc32(new TextEncoder().encode('123456789')), 0xCBF43926);

const result = { games: 3, seed: 123, decisionLogStoredSeparately: true };
const records = [
    JSON.stringify({ game: 0, maxChain: 6, decisionLog: [{ turn: 1, x: 2 }] }),
    JSON.stringify({ game: 1, maxChain: 8, decisionLog: [{ turn: 1, x: 3 }] }),
    JSON.stringify({ game: 2, maxChain: 4, decisionLog: [{ turn: 1, x: 1 }] })
];
const progress = [];
const blob = await createBenchmarkZip(
    result,
    async (index) => ({ json: records[index] }),
    (completed, total) => progress.push([completed, total])
);
assert.ok(blob.size > 0);
assert.equal(progress.length, 3);
assert.deepEqual(progress.at(-1), [3, 3]);

const bytes = new Uint8Array(await blob.arrayBuffer());
assert.equal(String.fromCharCode(...bytes.slice(0, 4)), 'PK\x03\x04');
assert.equal(String.fromCharCode(...bytes.slice(-22, -18)), 'PK\x05\x06');
assert.ok(bytes.length > 22);

console.log(`benchmark ZIP writer test passed (${bytes.length} bytes)`);
