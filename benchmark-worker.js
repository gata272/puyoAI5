/* Deterministic maximum-chain benchmark worker. */
import createPuyoAI from './puyoAI_wasm.mjs';

let moduleInstance = null;
let runBenchmark = null;
let initializing = null;

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

            runBenchmark = moduleInstance.cwrap(
                'run_chain_benchmark',
                'string',
                ['number', 'number', 'number', 'number', 'number', 'number']
            );

            if (typeof runBenchmark !== 'function') {
                throw new Error('run_chain_benchmark のエクスポートが見つかりません');
            }

            self.postMessage({ type: 'ready' });
        } catch (error) {
            moduleInstance = null;
            runBenchmark = null;
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
    // Do not rethrow here. A rejected top-level worker promise can otherwise
    // obscure the actual initialization error in some browsers.
    return null;
});

self.onmessage = async (event) => {
    const msg = event.data || {};
    if (msg.type !== 'run') return;

    const initialized = await ready;
    if (!initialized && typeof runBenchmark !== 'function') {
        self.postMessage({
            type: 'error',
            message: 'ベンチマークWASMが初期化されていないため測定を開始できません'
        });
        return;
    }

    try {
        self.postMessage({ type: 'started' });
        const resultJson = runBenchmark(
            msg.games | 0,
            msg.turns | 0,
            msg.seed | 0,
            msg.depth | 0,
            msg.beamWidth | 0,
            msg.recordDecisionLog === false ? 0 : 1
        );
        if (typeof resultJson !== 'string' || resultJson.length === 0) {
            throw new Error('ベンチマーク結果が空です');
        }
        self.postMessage({ type: 'result', resultJson });
    } catch (error) {
        self.postMessage({
            type: 'error',
            message: `ベンチマーク実行失敗: ${error?.message || error}`
        });
    }
};
