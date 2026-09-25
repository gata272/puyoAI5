import assert from 'node:assert/strict';
import { createBenchmarkZip, crc32, compressBytesForZip, createBenchmarkZipBuilder } from '../benchmark-zip.js';

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

// Regression for the original 0/100 freeze: compress a realistic multi-megabyte
// log while the readable side is consumed concurrently.
const largeText = JSON.stringify({
    game: 0,
    decisionLog: Array.from({ length: 12000 }, (_, turn) => ({
        turn,
        board: 'RBGY'.repeat(20),
        candidates: Array.from({ length: 8 }, (__, i) => ({ x: i, score: turn * 0.01 + i })),
        note: 'benchmark-log-data-'.repeat(8)
    }))
});
const largeBytes = new TextEncoder().encode(largeText);
assert.ok(largeBytes.length > 1_000_000, `large test payload too small: ${largeBytes.length}`);
const compressedLarge = await compressBytesForZip(largeBytes);
assert.ok(compressedLarge.bytes.length > 0);
assert.ok(compressedLarge.method === 0 || compressedLarge.method === 8);

const builder = createBenchmarkZipBuilder({ games: 1, seed: 1 });
await builder.appendStandardFiles();
await builder.appendEntry('game_0001.json', largeBytes);
const largeBlob = builder.finalize();
assert.ok(largeBlob.size > 22);
assert.throws(() => builder.finalize(), /すでに完成/);

// Sequence regression: 100 independent game entries must progress to completion.
const sequenceProgress = [];
const sequenceRecords = Array.from({ length: 100 }, (_, index) =>
    JSON.stringify({ game: index, maxChain: index % 13, decisionLog: [{ turn: 1, score: index }] })
);
const sequenceBlob = await createBenchmarkZip(
    { games: 100, seed: 7 },
    async (index) => sequenceRecords[index],
    (completed, total) => sequenceProgress.push([completed, total])
);
assert.ok(sequenceBlob.size > 22);
assert.equal(sequenceProgress.length, 100);
assert.deepEqual(sequenceProgress[0], [1, 100]);
assert.deepEqual(sequenceProgress.at(-1), [100, 100]);

console.log(`benchmark ZIP writer test passed (${bytes.length} bytes; large=${largeBytes.length} bytes -> ${largeBlob.size} bytes)`);
