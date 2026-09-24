/* Debug-mode settings and benchmark controls. */
(function (global) {
    'use strict';

    const STORAGE_KEY = 'puyoAI.debugMode';
    const DEVELOPER_STORAGE_KEY = 'puyoAI.developerMode';
    const WEIGHTS_STORAGE_KEY = 'puyoAI.developerWeights';
    const BENCHMARK_DB_NAME = 'puyoAI-benchmark-logs';
    const BENCHMARK_DB_VERSION = 1;
    const DEFAULTS = {
        games: 5,
        turns: 60,
        seed: 20260908,
        depth: 10,
        beamWidth: 12,
        recordDecisionLog: true
    };

    const STATE = {
        debugMode: false,
        worker: null,
        ready: false,
        running: false,
        developerMode: false,
        weights: [],
        benchmarkProgressLines: [],
        lastBenchmarkLog: '',
        benchmarkRunId: '',
        benchmarkResult: null,
        benchmarkDB: null,
        exportingZip: false
    };

    function $(id) { return document.getElementById(id); }

    function readBool() {
        return localStorage.getItem(STORAGE_KEY) === 'true';
    }

    function setDebugMode(enabled) {
        STATE.debugMode = !!enabled;
        localStorage.setItem(STORAGE_KEY, STATE.debugMode ? 'true' : 'false');
        const checkbox = $('debug-mode-checkbox');
        if (checkbox) checkbox.checked = STATE.debugMode;
        const panel = $('debug-panel');
        if (panel) panel.hidden = !STATE.debugMode;
        const badge = $('debug-mode-badge');
        if (badge) badge.hidden = !STATE.debugMode;
    }

    function setStatus(text) {
        const el = $('benchmark-status');
        if (el) el.textContent = text;
    }

    function setRunning(running) {
        STATE.running = running;
        const button = $('run-benchmark-button');
        if (button) {
            button.disabled = !!running;
            button.textContent = running
                ? '測定中…'
                : (STATE.ready ? '最大連鎖ベンチマーク開始' : 'ベンチマーク再初期化');
        }
    }

    function setWorkerFailure(message) {
        STATE.ready = false;
        STATE.running = false;
        setStatus(message);
        const button = $('run-benchmark-button');
        if (button) {
            button.disabled = false;
            button.textContent = 'ベンチマーク再初期化';
        }
    }

    function readConfig() {
        const read = (id, fallback, min, max) => {
            const n = Number.parseInt($(id)?.value ?? fallback, 10);
            if (!Number.isFinite(n)) return fallback;
            return Math.max(min, Math.min(max, n));
        };
        return {
            games: read('benchmark-games', DEFAULTS.games, 1, 5000),
            turns: read('benchmark-turns', DEFAULTS.turns, 1, 500),
            seed: read('benchmark-seed', DEFAULTS.seed, -2147483648, 2147483647),
            depth: read('benchmark-depth', DEFAULTS.depth, 1, 50),
            beamWidth: read('benchmark-beam', DEFAULTS.beamWidth, 1, 500),
            recordDecisionLog: $('benchmark-record-log')?.checked !== false
        };
    }

    function formatBytes(bytes) {
        const value = Number(bytes) || 0;
        if (value < 1024) return `${value} B`;
        if (value < 1024 ** 2) return `${(value / 1024).toFixed(1)} KB`;
        if (value < 1024 ** 3) return `${(value / 1024 ** 2).toFixed(1)} MB`;
        return `${(value / 1024 ** 3).toFixed(2)} GB`;
    }

    function formatPercent(count, games) {
        return `${((count / games) * 100).toFixed(1)}% (${count}/${games})`;
    }

    function formatGameOverReasons(reasons) {
        if (!reasons) return '旧バージョンの結果';
        const labels = [
            ['no_safe_move', '安全手なし'],
            ['selected_death_with_safe_move', '安全手ありで死亡手を選択'],
            ['no_geometric_move', '配置不能'],
            ['invalid_move', '無効手'],
            ['other', 'その他']
        ];
        const parts = labels
            .filter(([key]) => Number(reasons[key] || 0) > 0)
            .map(([key, label]) => `${label}: ${reasons[key]}`);
        return parts.length ? parts.join(' / ') : 'なし';
    }

    function formatDeathDiagnostics(result) {
        const safe = result.averageSafeMovesBeforeDeath;
        const counts = result.diagnosticCounts;
        if (!Array.isArray(safe) || !Array.isArray(counts)) return '';
        const parts = [];
        for (let i = 0; i < safe.length; i += 1) {
            if (Number(counts[i] || 0) <= 0) continue;
            parts.push(`死亡-${i}手前: ${Number(safe[i]).toFixed(1)}手`);
        }
        if (!parts.length) return '';
        return `<tr><th>死亡前の平均安全手数</th><td>${parts.join(' / ')}</td></tr>`;
    }

    function buildBenchmarkLog(result) {
        const lines = [
            '[PuyoAI Benchmark Summary]',
            `version=${result.version ?? 'unknown'}`,
            `games=${result.games} turns=${result.turns} seed=${result.seed}`,
            `depth=${result.depth} beam=${result.beamWidth}`,
            `recordDecisionLog=${result.recordDecisionLog ? 'true' : 'false'}`,
            `averageMaxChain=${Number(result.averageMaxChain).toFixed(3)}`,
            `medianMaxChain=${Number(result.medianMaxChain).toFixed(3)}`,
            `p90MaxChain=${Number(result.p90MaxChain).toFixed(3)}`,
            `maxChain=${result.maxChain}`,
            `5+=${result.atLeast5}/${result.games}`,
            `8+=${result.atLeast8}/${result.games}`,
            `10+=${result.atLeast10}/${result.games}`,
            `12+=${result.atLeast12}/${result.games}`,
            `averageScore=${Number(result.averageScore).toFixed(3)}`,
            `averageTurns=${Number(result.averageTurns).toFixed(3)}`,
            `gamesOver=${result.gamesOver}/${result.games}`,
            `gameOverReasons=${JSON.stringify(result.gameOverReasons)}`,
            `averageSafeMovesBeforeDeath=${JSON.stringify(result.averageSafeMovesBeforeDeath)}`,
            `averageGeometricMovesBeforeDeath=${JSON.stringify(result.averageGeometricMovesBeforeDeath)}`,
            `diagnosticCounts=${JSON.stringify(result.diagnosticCounts)}`,
            `averageThinkMs=${Number(result.averageThinkMs).toFixed(3)}`,
            `benchmarkComputeMs=${Number(result.totalWallMs).toFixed(3)}`,
            `deterministic=${result.deterministic}`,
            `logStorage=${result.logStorage || 'unknown'}`,
            `decisionLogGames=${result.decisionLogGames ?? 0}`,
            '',
            'Detailed per-turn logs are stored separately as game_XXXX.json inside the ZIP.'
        ];
        return `${lines.join('\n')}\n`;
    }

    function updateLogButtons(visible) {
        const actions = $('benchmark-log-actions');
        if (actions) actions.hidden = !visible;
    }

    function renderResult(result) {
        const el = $('benchmark-result');
        if (!el) return;
        el.innerHTML = `
            <div class="benchmark-summary-grid">
                <div><span>平均最大連鎖</span><strong>${Number(result.averageMaxChain).toFixed(2)}</strong></div>
                <div><span>中央値</span><strong>${Number(result.medianMaxChain).toFixed(2)}</strong></div>
                <div><span>90%点</span><strong>${Number(result.p90MaxChain).toFixed(2)}</strong></div>
                <div><span>最大</span><strong>${result.maxChain}</strong></div>
            </div>
            <table class="benchmark-table">
                <tbody>
                    <tr><th>5連鎖以上</th><td>${formatPercent(result.atLeast5, result.games)}</td></tr>
                    <tr><th>8連鎖以上</th><td>${formatPercent(result.atLeast8, result.games)}</td></tr>
                    <tr><th>10連鎖以上</th><td>${formatPercent(result.atLeast10, result.games)}</td></tr>
                    <tr><th>12連鎖以上</th><td>${formatPercent(result.atLeast12, result.games)}</td></tr>
                    <tr><th>平均スコア</th><td>${Number(result.averageScore).toFixed(1)}</td></tr>
                    <tr><th>平均生存ターン</th><td>${Number(result.averageTurns).toFixed(1)} / ${result.turns}</td></tr>
                    <tr><th>ゲームオーバー</th><td>${result.gamesOver} / ${result.games}</td></tr>
                    <tr><th>ゲームオーバー原因</th><td>${formatGameOverReasons(result.gameOverReasons)}</td></tr>
                    ${formatDeathDiagnostics(result)}
                    <tr><th>平均思考時間</th><td>${Number(result.averageThinkMs).toFixed(2)} ms / 手</td></tr>
                    <tr><th>AI測定時間</th><td>${(Number(result.totalWallMs) / 1000).toFixed(2)} s</td></tr>
                    <tr><th>ログ保存</th><td>1ゲームずつIndexedDBへ保存</td></tr>
                    <tr><th>設定</th><td>depth ${result.depth} / beam ${result.beamWidth}</td></tr>
                    <tr><th>Seed</th><td>${result.seed}</td></tr>
                </tbody>
            </table>
            <p class="benchmark-note">同じ Seed・試行数・ターン数なら、異なるAI設定でも同じツモ列が使われます。詳細ログはゲーム単位で保存されるため、測定終了時に巨大なJSONを一括生成しません。</p>
            <div id="benchmark-log-actions" class="benchmark-log-actions" hidden>
                <button type="button" onclick="copyBenchmarkLog()">結果サマリーをコピー</button>
                <button type="button" onclick="downloadBenchmarkLog()">詳細ログをZIP保存</button>
            </div>
        `;
    }

    function openBenchmarkDB() {
        if (STATE.benchmarkDB) return Promise.resolve(STATE.benchmarkDB);
        if (!('indexedDB' in global)) {
            return Promise.reject(new Error('このブラウザではIndexedDBが利用できません'));
        }
        return new Promise((resolve, reject) => {
            const request = global.indexedDB.open(BENCHMARK_DB_NAME, BENCHMARK_DB_VERSION);
            request.onupgradeneeded = () => {
                const db = request.result;
                if (!db.objectStoreNames.contains('runs')) {
                    db.createObjectStore('runs', { keyPath: 'id' });
                }
                if (!db.objectStoreNames.contains('games')) {
                    const games = db.createObjectStore('games', { keyPath: 'id' });
                    games.createIndex('runId', 'runId', { unique: false });
                }
            };
            request.onsuccess = () => {
                STATE.benchmarkDB = request.result;
                STATE.benchmarkDB.onversionchange = () => STATE.benchmarkDB?.close();
                resolve(STATE.benchmarkDB);
            };
            request.onerror = () => reject(request.error || new Error('IndexedDBを開けませんでした'));
        });
    }

    async function prepareBenchmarkRun(runId, config) {
        const db = await openBenchmarkDB();
        await new Promise((resolve, reject) => {
            const tx = db.transaction(['runs', 'games'], 'readwrite');
            tx.objectStore('games').clear();
            tx.objectStore('runs').put({
                id: 'latest',
                runId,
                status: 'running',
                config,
                completedGames: 0,
                createdAt: new Date().toISOString()
            });
            tx.oncomplete = resolve;
            tx.onerror = () => reject(tx.error || new Error('ベンチマーク保存領域の初期化に失敗しました'));
            tx.onabort = () => reject(tx.error || new Error('ベンチマーク保存領域の初期化が中断されました'));
        });
    }

    async function storeBenchmarkGame(runId, gameIndex, resultJson) {
        const db = await openBenchmarkDB();
        await new Promise((resolve, reject) => {
            const tx = db.transaction(['games', 'runs'], 'readwrite');
            tx.objectStore('games').put({
                id: `${runId}:${gameIndex}`,
                runId,
                gameIndex,
                json: resultJson,
                savedAt: Date.now()
            });
            const metaStore = tx.objectStore('runs');
            const metaRequest = metaStore.get('latest');
            metaRequest.onsuccess = () => {
                const current = metaRequest.result || { id: 'latest' };
                metaStore.put({
                    ...current,
                    runId,
                    completedGames: gameIndex + 1,
                    lastSavedGame: gameIndex
                });
            };
            metaRequest.onerror = () => reject(metaRequest.error || new Error('ベンチマーク状態の更新に失敗しました'));
            tx.oncomplete = resolve;
            tx.onerror = () => reject(tx.error || new Error(`ゲーム${gameIndex + 1}のログ保存に失敗しました`));
            tx.onabort = () => reject(tx.error || new Error(`ゲーム${gameIndex + 1}のログ保存が中断されました`));
        });
    }

    async function updateBenchmarkRunMeta(fields) {
        const db = await openBenchmarkDB();
        await new Promise((resolve, reject) => {
            const tx = db.transaction('runs', 'readwrite');
            const store = tx.objectStore('runs');
            const request = store.get('latest');
            request.onsuccess = () => {
                const current = request.result || { id: 'latest' };
                store.put({ ...current, ...fields });
            };
            request.onerror = () => reject(request.error || new Error('ベンチマーク状態の読み込みに失敗しました'));
            tx.oncomplete = resolve;
            tx.onerror = () => reject(tx.error || new Error('ベンチマーク状態の保存に失敗しました'));
            tx.onabort = () => reject(tx.error || new Error('ベンチマーク状態の保存が中断されました'));
        });
    }

    async function restoreLatestBenchmarkRun() {
        try {
            const db = await openBenchmarkDB();
            const meta = await new Promise((resolve, reject) => {
                const tx = db.transaction('runs', 'readonly');
                const request = tx.objectStore('runs').get('latest');
                request.onsuccess = () => resolve(request.result || null);
                request.onerror = () => reject(request.error || new Error('前回のベンチマーク情報を読めませんでした'));
            });
            if (!meta || !meta.result || meta.status !== 'complete') return;
            STATE.benchmarkRunId = meta.runId || '';
            STATE.benchmarkResult = meta.result;
            STATE.lastBenchmarkLog = buildBenchmarkLog(meta.result);
            renderResult(meta.result);
            updateLogButtons(true);
            setStatus('前回のベンチマーク結果を復元しました');
        } catch (error) {
            console.warn('[Benchmark log storage]', error);
        }
    }

    let benchmarkZipObjectUrl = '';

    function revokeBenchmarkZipUrl() {
        if (!benchmarkZipObjectUrl) return;
        try { global.URL.revokeObjectURL(benchmarkZipObjectUrl); } catch (_) {}
        benchmarkZipObjectUrl = '';
    }

    async function saveBenchmarkZipBlob(blob, filename) {
        const file = typeof File === 'function'
            ? new File([blob], filename, { type: 'application/zip' })
            : null;

        if (file && typeof navigator.share === 'function') {
            let canShareFiles = false;
            try { canShareFiles = typeof navigator.canShare !== 'function' || navigator.canShare({ files: [file] }); } catch (_) {}
            if (canShareFiles) {
                try {
                    await navigator.share({
                        files: [file],
                        title: 'PuyoAI ベンチマークログ'
                    });
                    setStatus('共有シートを開きました。「ファイルに保存」でZIPを保存できます。');
                    return true;
                } catch (error) {
                    if (error?.name === 'AbortError') return false;
                    console.warn('[Benchmark ZIP share fallback]', error);
                }
            }
        }

        revokeBenchmarkZipUrl();
        benchmarkZipObjectUrl = global.URL.createObjectURL(blob);
        const link = document.createElement('a');
        link.href = benchmarkZipObjectUrl;
        link.download = filename;
        link.rel = 'noopener';
        link.style.display = 'none';
        document.body.appendChild(link);
        try {
            link.click();
        } finally {
            link.remove();
        }
        setStatus('ZIPの保存を開始しました。ダウンロード一覧またはファイルアプリを確認してください。');
        return true;
    }

    function renderBenchmarkZipActions(container, blob, filename) {
        revokeBenchmarkZipUrl();
        container.innerHTML = '';

        const title = document.createElement('div');
        title.className = 'benchmark-export-ready';
        title.textContent = `ZIPの準備が完了しました（${formatBytes(blob.size)}）`;

        const saveButton = document.createElement('button');
        saveButton.type = 'button';
        saveButton.textContent = 'ZIPを保存 / ファイルに保存';
        saveButton.className = 'benchmark-zip-save-link';
        saveButton.addEventListener('click', () => {
            void saveBenchmarkZipBlob(blob, filename);
        });

        const retryButton = document.createElement('button');
        retryButton.type = 'button';
        retryButton.textContent = 'ZIPをもう一度作成';
        retryButton.className = 'benchmark-zip-share-button';
        retryButton.addEventListener('click', () => {
            void exportBenchmarkZipInPage(STATE.benchmarkRunId);
        });

        const note = document.createElement('div');
        note.className = 'benchmark-export-note';
        note.textContent = 'iPhone / iPadでは保存ボタンから共有シートを開き、「ファイルに保存」を選択できます。';

        container.append(title, saveButton, retryButton, note);
        container.hidden = false;
    }

    async function exportBenchmarkZipInPage(runId) {
        const actions = $('benchmark-log-actions');
        if (!actions || !runId) {
            setStatus('保存できるベンチマーク結果がありません');
            return;
        }
        if (!STATE.worker || !STATE.ready) {
            setStatus('ベンチマークWorkerが準備できていません。ページを再読み込みして再試行してください。');
            return;
        }
        if (STATE.running) {
            setStatus('ベンチマーク実行中はZIPを保存できません');
            return;
        }
        if (STATE.exportingZip) {
            setStatus('ZIPを作成中です。完了するまでお待ちください。');
            return;
        }

        STATE.exportingZip = true;
        revokeBenchmarkZipUrl();
        actions.innerHTML = '<div class="benchmark-export-progress">ZIPを作成しています…</div>';
        actions.hidden = false;
        setStatus('詳細ログをWorkerでZIPにまとめています…');
        STATE.worker.postMessage({ type: 'exportZip', runId });
    }

    function initWorker() {
        if (STATE.worker) return;

        STATE.ready = false;
        setStatus('ベンチマークWASMを初期化中…');

        let worker;
        try {
            worker = new Worker('./benchmark-worker.js', { type: 'module' });
        } catch (error) {
            STATE.worker = null;
            setWorkerFailure(`ベンチマークWorker作成失敗: ${error?.message || error}`);
            return;
        }

        STATE.worker = worker;
        let initialized = false;
        const initTimer = setTimeout(() => {
            if (initialized || STATE.worker !== worker) return;
            try { worker.terminate(); } catch (_) {}
            STATE.worker = null;
            setWorkerFailure(
                'ベンチマークWASMの初期化がタイムアウトしました。' +
                'ページを再読み込みして再試行してください。'
            );
        }, 30000);

        worker.onerror = (event) => {
            clearTimeout(initTimer);
            const message = event?.message || 'Workerスクリプトの読み込みに失敗しました';
            try { worker.terminate(); } catch (_) {}
            if (STATE.worker === worker) STATE.worker = null;
            setWorkerFailure(`ベンチマークWorkerエラー: ${message}`);
            console.error('[Benchmark Worker]', event);
        };

        worker.onmessageerror = (event) => {
            setWorkerFailure('ベンチマークWorkerとの通信に失敗しました');
            console.error('[Benchmark Worker messageerror]', event);
        };

        worker.onmessage = async (event) => {
            const msg = event.data || {};
            if (msg.type === 'initializing') {
                setStatus(msg.message || 'ベンチマークWASMを初期化中…');
                return;
            }
            if (msg.type === 'ready') {
                initialized = true;
                clearTimeout(initTimer);
                STATE.ready = true;
                setRunning(false);
                if (!STATE.running) setStatus('ベンチマーク準備完了');
                return;
            }
            if (msg.type === 'started') {
                setRunning(true);
                setStatus('同一ツモ列で測定しています…');
                return;
            }
            if (msg.type === 'game') {
                try {
                    setStatus(`ゲーム ${Number(msg.gameIndex) + 1} / ${Number(msg.runId === STATE.benchmarkRunId ? (STATE.benchmarkResult?.games || readConfig().games) : readConfig().games)} のログを保存中…`);
                    await storeBenchmarkGame(msg.runId, Number(msg.gameIndex), msg.resultJson);
                    worker.postMessage({ type: 'gameStored', runId: msg.runId, gameIndex: Number(msg.gameIndex), ok: true });
                } catch (error) {
                    const message = error?.message || String(error);
                    try { await updateBenchmarkRunMeta({ status: 'storage_error', error: message, completedGames: Number(msg.gameIndex) }); } catch (_) {}
                    worker.postMessage({ type: 'gameStored', runId: msg.runId, gameIndex: Number(msg.gameIndex), ok: false, message });
                }
                return;
            }
            if (msg.type === 'exportStarted') {
                if (msg.runId !== STATE.benchmarkRunId) return;
                const actions = $('benchmark-log-actions');
                const progress = actions?.querySelector('.benchmark-export-progress');
                if (progress) progress.textContent = `ZIPを作成中… 0 / ${Number(msg.totalGames) || 0} ゲーム`;
                setStatus(`ZIPを作成中… 0 / ${Number(msg.totalGames) || 0} ゲーム`);
                return;
            }
            if (msg.type === 'exportProgress') {
                if (msg.runId !== STATE.benchmarkRunId) return;
                const completed = Number(msg.completed) || 0;
                const total = Number(msg.total) || 0;
                const actions = $('benchmark-log-actions');
                const progress = actions?.querySelector('.benchmark-export-progress');
                if (progress) progress.textContent = `ZIPを作成中… ${completed} / ${total} ゲーム`;
                setStatus(`ZIPを作成中… ${completed} / ${total} ゲーム`);
                return;
            }
            if (msg.type === 'exportComplete') {
                if (msg.runId !== STATE.benchmarkRunId || !msg.blob || typeof msg.blob.size !== 'number') return;
                const actions = $('benchmark-log-actions');
                if (actions) renderBenchmarkZipActions(actions, msg.blob, String(msg.filename || 'puyoAI-benchmark.zip'));
                STATE.exportingZip = false;
                setStatus('ZIPの準備が完了しました。保存ボタンをタップしてください。');
                return;
            }
            if (msg.type === 'exportError') {
                if (msg.runId !== STATE.benchmarkRunId) return;
                const actions = $('benchmark-log-actions');
                if (actions) actions.innerHTML = '<button type="button" onclick="downloadBenchmarkLog()">詳細ログをZIP保存</button>';
                if (actions) actions.hidden = false;
                STATE.exportingZip = false;
                setStatus(`ZIP作成に失敗しました: ${msg.message || '不明なエラー'}`);
                console.error('[Benchmark ZIP export]', msg.message);
                return;
            }
            if (msg.type === 'progress') {
                STATE.benchmarkProgressLines.push(String(msg.message || ''));
                console.log(msg.message);
                const match = String(msg.message || '').match(/Game (\d+)\/(\d+)/);
                if (match) {
                    setStatus(`ベンチマーク進行中… ${match[1]} / ${match[2]} ゲーム`);
                }
                return;
            }
            if (msg.type === 'result') {
                try {
                    const result = msg.result;
                    if (!result || typeof result !== 'object') throw new Error('最終結果が不正です');
                    renderResult(result);
                    STATE.benchmarkResult = result;
                    STATE.lastBenchmarkLog = buildBenchmarkLog(result);
                    await updateBenchmarkRunMeta({
                        status: 'complete',
                        result,
                        completedGames: result.completedGames
                    });
                    updateLogButtons(true);
                    setRunning(false);
                    setStatus('測定完了。詳細ログはIndexedDBへ保存済みです。');
                } catch (error) {
                    setRunning(false);
                    setStatus(`結果の保存に失敗しました: ${error?.message || error}`);
                    console.error(error);
                }
                return;
            }
            if (msg.type === 'error') {
                const message = msg.message || 'ベンチマークエラー';
                try { await updateBenchmarkRunMeta({ status: 'error', error: message }); } catch (_) {}
                if (!initialized) {
                    clearTimeout(initTimer);
                    try { worker.terminate(); } catch (_) {}
                    if (STATE.worker === worker) STATE.worker = null;
                    setWorkerFailure(message);
                } else {
                    setRunning(false);
                    setStatus(message);
                }
                console.error(message);
            }
        };
    }

    function setDeveloperMode(enabled) {
        STATE.developerMode = !!enabled;
        try { localStorage.setItem(DEVELOPER_STORAGE_KEY, STATE.developerMode ? 'true' : 'false'); } catch (_) {}
        const checkbox = $('developer-mode-checkbox');
        if (checkbox) checkbox.checked = STATE.developerMode;
        const panel = $('developer-panel');
        if (panel) panel.hidden = !STATE.developerMode;
        const badge = $('developer-mode-badge');
        if (badge) badge.hidden = !STATE.developerMode;
        if (STATE.developerMode) requestWeights();
    }

    function renderWeights(weights) {
        STATE.weights = weights || [];
        const container = $('developer-weights');
        if (!container) return;
        container.innerHTML = '';
        for (const item of STATE.weights) {
            const row = document.createElement('div');
            row.className = 'developer-weight-row';
            const label = document.createElement('label');
            label.textContent = item.name;
            const input = document.createElement('input');
            input.type = 'number';
            input.step = 'any';
            input.dataset.weightIndex = String(item.index);
            input.value = Number(item.value).toString();
            row.append(label, input);
            container.appendChild(row);
        }
        const status = $('developer-status');
        if (status) status.textContent = `${STATE.weights.length}個の重みを読み込みました`;
    }

    global.renderDeveloperWeights = renderWeights;

    function requestWeights() {
        if (typeof global.requestAIWeights === 'function') {
            global.requestAIWeights();
        } else {
            const status = $('developer-status');
            if (status) status.textContent = '通常AIワーカーを初期化中…';
            if (typeof global.toggleAI === 'function') {
                // toggleAI is not forced here; the normal worker is initialized
                // only when AI is actually enabled.
            }
        }
    }

    global.toggleDeveloperMode = function () {
        const checkbox = $('developer-mode-checkbox');
        if (!STATE.debugMode) {
            if (checkbox) checkbox.checked = false;
            setDeveloperMode(false);
            return;
        }
        setDeveloperMode(!!checkbox?.checked);
    };

    global.applyDeveloperWeights = function () {
        const inputs = document.querySelectorAll('#developer-weights input[data-weight-index]');
        const values = [];
        for (const input of inputs) {
            const index = Number.parseInt(input.dataset.weightIndex, 10);
            const value = Number(input.value);
            if (!Number.isFinite(index) || !Number.isFinite(value)) {
                const status = $('developer-status');
                if (status) status.textContent = '数値が不正な項目があります';
                return;
            }
            values[index] = value;
        }
        try { localStorage.setItem(WEIGHTS_STORAGE_KEY, JSON.stringify(values)); } catch (_) {}
        if (typeof global.applyAIWeights === 'function') {
            global.applyAIWeights(values);
        }
        const status = $('developer-status');
        if (status) status.textContent = '重みを適用・保存しました';
    };

    global.resetDeveloperWeights = function () {
        try { localStorage.removeItem(WEIGHTS_STORAGE_KEY); } catch (_) {}
        if (typeof global.resetAIWeights === 'function') {
            global.resetAIWeights();
        }
        requestWeights();
        const status = $('developer-status');
        if (status) status.textContent = 'ama基準値に戻しました';
    };


    global.toggleDebugMode = function () {
        const checkbox = $('debug-mode-checkbox');
        setDebugMode(!!checkbox?.checked);
        if (STATE.debugMode) initWorker();
    };

    global.runChainBenchmark = async function () {
        if (!STATE.debugMode) {
            setStatus('設定でデバッグモードをONにしてください');
            return;
        }
        if (STATE.running) return;
        initWorker();
        if (!STATE.ready) {
            setStatus('WASMベンチマークを初期化中です。準備完了後にもう一度押してください。');
            return;
        }

        const config = readConfig();
        const runId = `${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;
        try {
            await prepareBenchmarkRun(runId, config);
        } catch (error) {
            setStatus(`ログ保存領域を準備できません: ${error?.message || error}`);
            return;
        }

        STATE.benchmarkRunId = runId;
        STATE.benchmarkResult = null;
        STATE.benchmarkProgressLines = [];
        STATE.lastBenchmarkLog = '';
        updateLogButtons(false);
        setRunning(true);
        setStatus('測定開始…ゲームごとにログを保存します。');
        $('benchmark-result').innerHTML = '<div class="benchmark-empty">結果を計算中…<br>ログはゲームごとにIndexedDBへ保存しています。</div>';
        STATE.worker.postMessage({ type: 'run', ...config, runId });
    };

    global.copyBenchmarkLog = async function () {
        if (!STATE.lastBenchmarkLog) return;
        try {
            await navigator.clipboard.writeText(STATE.lastBenchmarkLog);
            setStatus('結果サマリーをクリップボードにコピーしました');
        } catch (error) {
            const area = document.createElement('textarea');
            area.value = STATE.lastBenchmarkLog;
            area.style.position = 'fixed';
            area.style.opacity = '0';
            document.body.appendChild(area);
            area.focus();
            area.select();
            let ok = false;
            try { ok = document.execCommand('copy'); } catch (_) {}
            area.remove();
            setStatus(ok ? '結果サマリーをクリップボードにコピーしました' : 'コピーに失敗しました');
        }
    };

    global.downloadBenchmarkLog = function () {
        const result = STATE.benchmarkResult;
        const runId = STATE.benchmarkRunId;
        if (!result || !runId) {
            setStatus('保存できるベンチマーク結果がありません');
            return;
        }
        void exportBenchmarkZipInPage(runId);
    };

    global.addEventListener('beforeunload', revokeBenchmarkZipUrl);

    global.initializeDebugMode = function () {
        setDebugMode(readBool());
        try { setDeveloperMode(localStorage.getItem(DEVELOPER_STORAGE_KEY) === 'true'); } catch (_) { setDeveloperMode(false); }
        const checkbox = $('debug-mode-checkbox');
        if (checkbox) checkbox.checked = STATE.debugMode;
        for (const [id, value] of Object.entries({
            'benchmark-games': DEFAULTS.games,
            'benchmark-turns': DEFAULTS.turns,
            'benchmark-seed': DEFAULTS.seed,
            'benchmark-depth': DEFAULTS.depth,
            'benchmark-beam': DEFAULTS.beamWidth
        })) {
            const input = $(id);
            if (input && !input.value) input.value = value;
        }
        restoreLatestBenchmarkRun();
        if (STATE.debugMode) initWorker();
    };

    document.addEventListener('DOMContentLoaded', global.initializeDebugMode);
})(window);
