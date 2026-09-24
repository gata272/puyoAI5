/* Deterministic maximum-chain benchmark worker.
 *
 * The browser benchmark deliberately runs one game per WASM call.  The
 * previous implementation returned one giant JSON string for the entire
 * benchmark, which forced the browser to hold multiple full-size copies while
 * transferring/parsing/stringifying it.  This worker keeps at most one game's
 * detailed JSON in memory and waits for the main thread to persist it before
 * starting the next game.
 */
import createPuyoAI from './puyoAI_wasm.mjs';

let moduleInstance = null;
let runBenchmarkGame = null;
let initializing = null;
let activeRunId = null;
let pendingStoreAck = null;

async function init() {
    if (initializing) return initializing;

    self.postMessage({
        type: 'initializing',
        message: 'WASMモジュールを読み込んでいます…'
    });

    initializing = (async () => {
        try {
            moduleInstance = await createPuyoAI();

            if (!moduleInstance || typeof moduleInstance.cwrap !== 'function') {
                throw new Error('WASMモジュールを初期化できましたが、cwrap が利用できません');
            }

            runBenchmarkGame = moduleInstance.cwrap(
                'run_chain_benchmark_game',
                'string',
                ['number', 'number', 'number', 'number', 'number', 'number', 'number']
            );

            if (typeof runBenchmarkGame !== 'function') {
                throw new Error('run_chain_benchmark_game のエクスポートが見つかりません');
            }

            self.postMessage({ type: 'ready' });
        } catch (error) {
            moduleInstance = null;
            runBenchmarkGame = null;
            throw error;
        }
    })();

    return initializing;
}

const ready = init().catch((error) => {
    self.postMessage({
        type: 'error',
        message: `ベンチマークWASM初期化失敗: ${error?.message || error}`
    });
    return null;
});

function waitForGameStored(runId, gameIndex) {
    return new Promise((resolve) => {
        pendingStoreAck = { runId, gameIndex, resolve };
    });
}

function aggregatePercentile(values, p) {
    if (!values.length) return 0;
    const sorted = values.slice().sort((a, b) => a - b);
    if (sorted.length === 1) return sorted[0];
    const pos = p * (sorted.length - 1);
    const lo = Math.floor(pos);
    const hi = Math.ceil(pos);
    const frac = pos - lo;
    return sorted[lo] * (1 - frac) + sorted[hi] * frac;
}

function createAccumulator(config) {
    return {
        maxChains: [],
        totalScore: 0,
        totalTurns: 0,
        totalThinkMicros: 0,
        totalMoves: 0,
        gamesOver: 0,
        globalMaxChain: 0,
        gameOverReasons: {
            invalid_move: 0,
            no_geometric_move: 0,
            no_safe_move: 0,
            selected_death_with_safe_move: 0,
            other: 0
        },
        diagnosticSafeMoveSum: [0, 0, 0, 0, 0],
        diagnosticGeometricMoveSum: [0, 0, 0, 0, 0],
        diagnosticCounts: [0, 0, 0, 0, 0],
        gameSummaries: [],
        computeWallMs: 0,
        startedAt: performance.now(),
        config
    };
}

function absorbGame(accumulator, gameResult) {
    const maxChain = Number(gameResult.maxChain) || 0;
    const score = Number(gameResult.score) || 0;
    const turns = Number(gameResult.turnsSurvived) || 0;
    const thinkMicros = Number(gameResult.totalThinkMicros) || 0;
    const moves = Number(gameResult.totalMoves) || 0;

    accumulator.maxChains.push(maxChain);
    accumulator.totalScore += score;
    accumulator.totalTurns += turns;
    accumulator.totalThinkMicros += thinkMicros;
    accumulator.totalMoves += moves;
    accumulator.computeWallMs += Number(gameResult.wallMs) || 0;
    accumulator.globalMaxChain = Math.max(accumulator.globalMaxChain, maxChain);

    if (gameResult.gameOver) {
        accumulator.gamesOver += 1;
        const reason = String(gameResult.gameOverReason || 'other');
        if (Object.prototype.hasOwnProperty.call(accumulator.gameOverReasons, reason)) {
            accumulator.gameOverReasons[reason] += 1;
        } else {
            accumulator.gameOverReasons.other += 1;
        }
    }

    if (Array.isArray(gameResult.diagnostics)) {
        for (const item of gameResult.diagnostics) {
            const offset = Number(item.turnsBeforeDeath);
            if (!Number.isInteger(offset) || offset < 0 || offset >= accumulator.diagnosticCounts.length) {
                continue;
            }
            accumulator.diagnosticSafeMoveSum[offset] += Number(item.safeMoves) || 0;
            accumulator.diagnosticGeometricMoveSum[offset] += Number(item.geometricMoves) || 0;
            accumulator.diagnosticCounts[offset] += 1;
        }
    }

    accumulator.gameSummaries.push({
        game: Number(gameResult.game) || accumulator.gameSummaries.length,
        maxChain,
        score,
        loggedTurns: Number(gameResult.loggedTurns) || 0,
        gameOver: !!gameResult.gameOver,
        turnsSurvived: turns,
        gameOverReason: String(gameResult.gameOverReason || 'none')
    });
}

function finalizeResult(accumulator) {
    const { config } = accumulator;
    const games = Math.max(1, accumulator.maxChains.length);
    const countAtLeast = (threshold) => accumulator.maxChains.filter((v) => v >= threshold).length;
    const avg = (value) => value / games;

    const averageSafeMovesBeforeDeath = accumulator.diagnosticSafeMoveSum.map((sum, index) =>
        accumulator.diagnosticCounts[index] > 0 ? sum / accumulator.diagnosticCounts[index] : 0
    );
    const averageGeometricMovesBeforeDeath = accumulator.diagnosticGeometricMoveSum.map((sum, index) =>
        accumulator.diagnosticCounts[index] > 0 ? sum / accumulator.diagnosticCounts[index] : 0
    );

    return {
        version: 8,
        games: config.games,
        turns: config.turns,
        seed: config.seed,
        depth: config.depth,
        beamWidth: config.beamWidth,
        averageMaxChain: avg(accumulator.maxChains.reduce((sum, value) => sum + value, 0)),
        medianMaxChain: aggregatePercentile(accumulator.maxChains, 0.50),
        p90MaxChain: aggregatePercentile(accumulator.maxChains, 0.90),
        maxChain: accumulator.globalMaxChain,
        atLeast5: countAtLeast(5),
        atLeast8: countAtLeast(8),
        atLeast10: countAtLeast(10),
        atLeast12: countAtLeast(12),
        averageScore: avg(accumulator.totalScore),
        averageTurns: avg(accumulator.totalTurns),
        gamesOver: accumulator.gamesOver,
        gameOverReasons: accumulator.gameOverReasons,
        diagnosticHistory: 5,
        averageSafeMovesBeforeDeath,
        averageGeometricMovesBeforeDeath,
        diagnosticCounts: accumulator.diagnosticCounts,
        averageThinkMs: accumulator.totalMoves > 0
            ? accumulator.totalThinkMicros / accumulator.totalMoves / 1000
            : 0,
        totalWallMs: accumulator.computeWallMs,
        recordDecisionLog: config.recordDecisionLog,
        gameSummaries: accumulator.gameSummaries,
        // Detailed turns intentionally live in IndexedDB as one JSON file per game.
        decisionLog: [],
        decisionLogStoredSeparately: true,
        decisionLogGames: accumulator.maxChains.length,
        deterministic: true,
        logStorage: 'indexeddb-per-game',
        completedGames: accumulator.maxChains.length,
        wallClockMsIncludingStorage: performance.now() - accumulator.startedAt
    };
}

async function runGames(config, runId) {
    activeRunId = runId;
    const initialized = await ready;
    if (!initialized && typeof runBenchmarkGame !== 'function') {
        throw new Error('ベンチマークWASMが初期化されていないため測定を開始できません');
    }

    const accumulator = createAccumulator(config);
    self.postMessage({ type: 'started', runId, games: config.games });

    for (let game = 0; game < config.games; game += 1) {
        const resultJson = runBenchmarkGame(
            config.games | 0,
            config.turns | 0,
            config.seed | 0,
            config.depth | 0,
            config.beamWidth | 0,
            config.recordDecisionLog === false ? 0 : 1,
            game | 0
        );

        if (typeof resultJson !== 'string' || resultJson.length === 0) {
            throw new Error(`ゲーム${game + 1}のベンチマーク結果が空です`);
        }

        let gameResult;
        try {
            gameResult = JSON.parse(resultJson);
        } catch (error) {
            throw new Error(`ゲーム${game + 1}のJSON解析に失敗しました: ${error?.message || error}`);
        }
        if (Number(gameResult.game) !== game) {
            throw new Error(`ゲーム番号が不一致です: expected=${game}, actual=${gameResult.game}`);
        }

        // Transfer only the raw JSON once.  The main thread persists it as-is;
        // no second cloned copy of the parsed turn log is sent back for progress.
        self.postMessage({
            type: 'game',
            runId,
            gameIndex: game,
            resultJson
        });

        const stored = await waitForGameStored(runId, game);
        pendingStoreAck = null;
        if (!stored?.ok) {
            throw new Error(stored?.message || `ゲーム${game + 1}のログ保存に失敗しました`);
        }

        absorbGame(accumulator, gameResult);
        const completedGames = game + 1;
        const completed12 = accumulator.maxChains.filter((value) => value >= 12).length;
        const running12Percent = (completed12 / completedGames) * 100;
        const runningAvgTurns = accumulator.totalTurns / completedGames;

        let message = `[Benchmark] Game ${completedGames}/${config.games}`
            + ` | max=${gameResult.maxChain}`
            + ` | survived=${gameResult.turnsSurvived}/${config.turns}`
            + ` | 12+=${completed12}/${completedGames}`
            + ` (${running12Percent.toFixed(1)}%)`
            + ` | avgSurvived=${runningAvgTurns.toFixed(1)}`;
        if (gameResult.gameOver) {
            message += ` | gameOver=${gameResult.gameOverReason}`
                + ` | h2=${gameResult.dangerColumnHeightAtEnd}`
                + ` | maxH=${gameResult.maxHeightAtEnd}`
                + ` | occupied=${gameResult.occupiedAtEnd}`;
            if (Number(gameResult.geometricMovesAtEnd) > 0) {
                message += ` | safeMoves=${gameResult.safeMovesAtEnd}/${gameResult.geometricMovesAtEnd}`;
            }
        }
        self.postMessage({
            type: 'progress',
            runId,
            gameIndex: game,
            message,
            completedGames
        });
    }

    const result = finalizeResult(accumulator);
    self.postMessage({ type: 'result', runId, result });
    activeRunId = null;
}

self.onmessage = async (event) => {
    const msg = event.data || {};

    if (msg.type === 'gameStored') {
        const waiting = pendingStoreAck;
        if (!waiting) return;
        if (waiting.runId !== msg.runId || waiting.gameIndex !== msg.gameIndex) return;
        waiting.resolve({ ok: !!msg.ok, message: msg.message || '' });
        return;
    }

    if (msg.type !== 'run') return;
    if (activeRunId) {
        self.postMessage({ type: 'error', runId: msg.runId, message: 'ベンチマークは既に実行中です' });
        return;
    }

    try {
        await runGames(msg, msg.runId);
    } catch (error) {
        activeRunId = null;
        pendingStoreAck = null;
        self.postMessage({
            type: 'error',
            runId: msg.runId,
            message: `ベンチマーク実行失敗: ${error?.message || error}`
        });
    }
};
