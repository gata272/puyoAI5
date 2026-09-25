/* Dedicated benchmark ZIP export worker.
 *
 * This worker intentionally does NOT open IndexedDB. The page reads one game
 * at a time and transfers its UTF-8 bytes here. That avoids Safari/WebKit
 * issues around IndexedDB access from a long-lived Worker and keeps the
 * benchmark/WASM Worker independent from export work.
 */
import { createBenchmarkZipBuilder } from './benchmark-zip.js';

let exportRunId = null;
let builder = null;
let expectedGameIndex = 0;
let totalGames = 0;
let busy = false;

function fail(runId, error) {
    builder = null;
    exportRunId = null;
    expectedGameIndex = 0;
    totalGames = 0;
    busy = false;
    self.postMessage({
        type: 'exportError',
        runId,
        message: error?.message || String(error)
    });
}

async function startExport(runId, result) {
    if (busy) {
        throw new Error('ZIPを書き出し中です。完了するまでお待ちください。');
    }
    if (!runId || typeof runId !== 'string') {
        throw new Error('ZIP出力のrunIdが不正です');
    }
    if (!result || typeof result !== 'object') {
        throw new Error('ZIP出力のベンチマーク結果が不正です');
    }

    busy = true;
    exportRunId = runId;
    expectedGameIndex = 0;
    builder = createBenchmarkZipBuilder(result);
    await builder.appendStandardFiles();

    totalGames = Math.max(0, Number(result.games) || 0);
    if (totalGames === 0) {
        throw new Error('ZIPに保存するゲームログがありません');
    }

    self.postMessage({
        type: 'exportStarted',
        runId,
        totalGames
    });
}

async function appendGame(runId, gameIndex, jsonBytes) {
    if (!busy || !builder || runId !== exportRunId) {
        throw new Error('ZIP出力セッションが開始されていません');
    }
    if (!Number.isInteger(gameIndex) || gameIndex !== expectedGameIndex) {
        throw new Error(`ゲーム番号が不正です: expected=${expectedGameIndex}, actual=${gameIndex}`);
    }

    const bytes = jsonBytes instanceof ArrayBuffer
        ? new Uint8Array(jsonBytes)
        : (jsonBytes instanceof Uint8Array ? jsonBytes : null);
    if (!bytes) {
        throw new Error(`ゲーム${gameIndex + 1}のログデータが不正です`);
    }
    if (bytes.length === 0) {
        throw new Error(`ゲーム${gameIndex + 1}のログが空です`);
    }

    const suffix = String(gameIndex + 1).padStart(4, '0');
    await builder.appendEntry(`game_${suffix}.json`, bytes);
    expectedGameIndex += 1;

    self.postMessage({
        type: 'exportProgress',
        runId,
        completed: expectedGameIndex,
        total: totalGames,
        gameIndex
    });
}

async function finishExport(runId) {
    if (!busy || !builder || runId !== exportRunId) {
        throw new Error('ZIP出力セッションが開始されていません');
    }

    const blob = builder.finalize();
    const stamp = new Date().toISOString().replace(/[:.]/g, '-');

    self.postMessage({
        type: 'exportComplete',
        runId,
        filename: `puyoAI-benchmark-${stamp}.zip`,
        blob
    });

    builder = null;
    exportRunId = null;
    expectedGameIndex = 0;
    totalGames = 0;
    busy = false;
}

self.onmessage = async (event) => {
    const msg = event.data || {};
    try {
        if (msg.type === 'startExport') {
            await startExport(msg.runId, msg.result);
            return;
        }

        if (msg.type === 'appendGame') {
            if (busy && !msg.runId) throw new Error('ZIP出力のrunIdがありません');
            if (busy && msg.gameIndex !== expectedGameIndex) {
                throw new Error(`ゲーム番号が不正です: expected=${expectedGameIndex}, actual=${msg.gameIndex}`);
            }
            await appendGame(msg.runId, Number(msg.gameIndex), msg.jsonBytes);
            return;
        }

        if (msg.type === 'finishExport') {
            await finishExport(msg.runId);
            return;
        }
    } catch (error) {
        fail(msg.runId || exportRunId, error);
    }
};
