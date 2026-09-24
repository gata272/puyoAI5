/* Standalone benchmark log exporter. */
import { createBenchmarkZip } from './benchmark-zip.js';

(function () {
    'use strict';

    const DB_NAME = 'puyoAI-benchmark-logs';
    const DB_VERSION = 1;
    const $ = (id) => document.getElementById(id);
    const params = new URLSearchParams(globalThis.location.search);
    const runId = params.get('runId') || '';
    let db = null;
    let objectUrl = '';

    function status(message, isError = false) {
        const el = $('export-status');
        if (el) {
            el.textContent = message;
            el.dataset.state = isError ? 'error' : 'normal';
        }
    }

    function formatBytes(bytes) {
        const value = Number(bytes) || 0;
        if (value < 1024) return `${value} B`;
        if (value < 1024 ** 2) return `${(value / 1024).toFixed(1)} KB`;
        if (value < 1024 ** 3) return `${(value / 1024 ** 2).toFixed(1)} MB`;
        return `${(value / 1024 ** 3).toFixed(2)} GB`;
    }

    function showSaveLink(url, filename, sizeBytes) {
        const link = $('save-zip-link');
        if (!link) return;
        if (objectUrl) URL.revokeObjectURL(objectUrl);
        objectUrl = url;
        link.href = url;
        link.download = filename;
        link.hidden = false;
        link.textContent = `ZIPを保存（${formatBytes(sizeBytes)}）`;
        $('share-zip-button')?.removeAttribute('hidden');
        $('export-actions')?.removeAttribute('hidden');
    }

    function openDB() {
        if (db) return Promise.resolve(db);
        return new Promise((resolve, reject) => {
            if (!('indexedDB' in globalThis)) {
                reject(new Error('このブラウザではIndexedDBが利用できません'));
                return;
            }
            const request = globalThis.indexedDB.open(DB_NAME, DB_VERSION);
            request.onupgradeneeded = () => {
                const target = request.result;
                if (!target.objectStoreNames.contains('runs')) {
                    target.createObjectStore('runs', { keyPath: 'id' });
                }
                if (!target.objectStoreNames.contains('games')) {
                    const games = target.createObjectStore('games', { keyPath: 'id' });
                    games.createIndex('runId', 'runId', { unique: false });
                }
            };
            request.onsuccess = () => {
                db = request.result;
                db.onversionchange = () => db?.close();
                resolve(db);
            };
            request.onerror = () => reject(request.error || new Error('IndexedDBを開けませんでした'));
        });
    }

    async function getRunMeta() {
        const target = await openDB();
        return new Promise((resolve, reject) => {
            const tx = target.transaction('runs', 'readonly');
            const request = tx.objectStore('runs').get('latest');
            request.onsuccess = () => resolve(request.result || null);
            request.onerror = () => reject(request.error || new Error('ベンチマーク情報を読み込めませんでした'));
        });
    }

    async function getGame(gameIndex) {
        const target = await openDB();
        return new Promise((resolve, reject) => {
            const tx = target.transaction('games', 'readonly');
            const request = tx.objectStore('games').get(`${runId}:${gameIndex}`);
            request.onsuccess = () => resolve(request.result || null);
            request.onerror = () => reject(request.error || new Error(`ゲーム${gameIndex + 1}のログを読み込めませんでした`));
        });
    }

    async function prepare() {
        if (!runId) throw new Error('ベンチマークIDが指定されていません');
        const meta = await getRunMeta();
        if (!meta || meta.runId !== runId || meta.status !== 'complete' || !meta.result) {
            throw new Error('完了済みのベンチマーク結果が見つかりません');
        }

        status('ZIPを作成しています…');
        const blob = await createBenchmarkZip(meta.result, getGame, (completed, total) => {
            status(`ZIPを作成中… ${completed} / ${total} ゲーム`);
        });
        const stamp = new Date().toISOString().replace(/[:.]/g, '-');
        const filename = `puyoAI-benchmark-${stamp}.zip`;
        showSaveLink(URL.createObjectURL(blob), filename, blob.size);
        status(`ZIPの準備が完了しました。保存ボタンを押してください。（${formatBytes(blob.size)}）`);

        // Try automatic save for browsers that still honor the original user
        // gesture across this newly opened export page. The visible link always
        // remains as the reliable fallback.
        try {
            $('save-zip-link')?.click();
        } catch (_) {}
    }

    async function shareZip() {
        const link = $('save-zip-link');
        if (!link?.href) return;
        try {
            const response = await fetch(link.href);
            const blob = await response.blob();
            const file = new File([blob], link.download || 'puyoAI-benchmark.zip', { type: 'application/zip' });
            if (!navigator.share || !navigator.canShare?.({ files: [file] })) {
                status('このブラウザでは共有による保存に対応していません', true);
                return;
            }
            await navigator.share({ files: [file], title: 'PuyoAI ベンチマークログ' });
            status('共有シートを開きました');
        } catch (error) {
            if (error?.name === 'AbortError') return;
            console.error('[Benchmark export share]', error);
            status(`共有に失敗しました: ${error?.message || error}`, true);
        }
    }

    window.addEventListener('beforeunload', () => {
        if (objectUrl) URL.revokeObjectURL(objectUrl);
    });
    $('share-zip-button')?.addEventListener('click', shareZip);

    prepare().catch((error) => {
        console.error('[Benchmark export]', error);
        status(`ZIP作成に失敗しました: ${error?.message || error}`, true);
        $('export-error')?.removeAttribute('hidden');
    });
}());
