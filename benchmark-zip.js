/* Small, dependency-free ZIP writer used by benchmark export workers. */

const ZIP_UINT16_MAX = 0xFFFF;
const ZIP_UINT32_MAX = 0xFFFFFFFF;
const COMPRESSION_CHUNK_SIZE = 64 * 1024;

export function crc32(bytes) {
    if (!crc32.table) {
        const table = new Uint32Array(256);
        for (let n = 0; n < 256; n += 1) {
            let c = n;
            for (let k = 0; k < 8; k += 1) {
                c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
            }
            table[n] = c >>> 0;
        }
        crc32.table = table;
    }
    let c = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; i += 1) {
        c = crc32.table[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
    }
    return (c ^ 0xFFFFFFFF) >>> 0;
}

function dosDateTime(date = new Date()) {
    const year = Math.min(2107, Math.max(1980, date.getFullYear()));
    return {
        dosTime:
            (date.getHours() << 11) |
            (date.getMinutes() << 5) |
            Math.floor(date.getSeconds() / 2),
        dosDate:
            ((year - 1980) << 9) |
            ((date.getMonth() + 1) << 5) |
            date.getDate()
    };
}

function makeZipLocalHeader(nameBytes, method, crc, compressedSize, size, dosTime, dosDate) {
    const header = new Uint8Array(30 + nameBytes.length);
    const view = new DataView(header.buffer);
    view.setUint32(0, 0x04034B50, true);
    view.setUint16(4, 20, true);
    view.setUint16(6, 0x0800, true); // UTF-8 names, known sizes.
    view.setUint16(8, method, true);
    view.setUint16(10, dosTime, true);
    view.setUint16(12, dosDate, true);
    view.setUint32(14, crc >>> 0, true);
    view.setUint32(18, compressedSize >>> 0, true);
    view.setUint32(22, size >>> 0, true);
    view.setUint16(26, nameBytes.length, true);
    header.set(nameBytes, 30);
    return header;
}

function makeZipCentralHeader(nameBytes, method, crc, compressedSize, size, dosTime, dosDate, localOffset) {
    const header = new Uint8Array(46 + nameBytes.length);
    const view = new DataView(header.buffer);
    view.setUint32(0, 0x02014B50, true);
    view.setUint16(4, 20, true);
    view.setUint16(6, 20, true);
    view.setUint16(8, 0x0800, true);
    view.setUint16(10, method, true);
    view.setUint16(12, dosTime, true);
    view.setUint16(14, dosDate, true);
    view.setUint32(16, crc >>> 0, true);
    view.setUint32(20, compressedSize >>> 0, true);
    view.setUint32(24, size >>> 0, true);
    view.setUint16(28, nameBytes.length, true);
    view.setUint32(42, localOffset >>> 0, true);
    header.set(nameBytes, 46);
    return header;
}

function makeZipEnd(entries, centralDirectorySize, centralDirectoryOffset) {
    const end = new Uint8Array(22);
    const view = new DataView(end.buffer);
    view.setUint32(0, 0x06054B50, true);
    view.setUint16(4, 0, true);
    view.setUint16(6, 0, true);
    view.setUint16(8, entries, true);
    view.setUint16(10, entries, true);
    view.setUint32(12, centralDirectorySize >>> 0, true);
    view.setUint32(16, centralDirectoryOffset >>> 0, true);
    return end;
}

function concatChunks(chunks, totalLength) {
    const output = new Uint8Array(totalLength);
    let offset = 0;
    for (const chunk of chunks) {
        output.set(chunk, offset);
        offset += chunk.length;
    }
    return output;
}

/**
 * Compress one payload for ZIP method 8 without deadlocking the browser stream.
 *
 * CompressionStream has backpressure between its writable and readable sides.
 * The old implementation waited for writer.close() before consuming readable,
 * which can stall forever for sufficiently large logs.  This implementation
 * starts the reader first and writes in bounded chunks so both sides advance.
 */
export async function compressBytesForZip(bytes) {
    if (!(bytes instanceof Uint8Array)) {
        throw new TypeError('圧縮対象はUint8Arrayである必要があります');
    }
    if (bytes.length === 0 || typeof globalThis.CompressionStream !== 'function') {
        return { method: 0, bytes };
    }

    let stream;
    let writer;
    let reader;
    try {
        stream = new globalThis.CompressionStream('deflate');
        writer = stream.writable.getWriter();
        reader = stream.readable.getReader();

        const outputChunks = [];
        let outputLength = 0;
        let readError = null;

        const readPromise = (async () => {
            try {
                for (;;) {
                    const { done, value } = await reader.read();
                    if (done) break;
                    if (!value || value.byteLength === 0) continue;
                    const chunk = value instanceof Uint8Array
                        ? value
                        : new Uint8Array(value);
                    outputChunks.push(chunk);
                    outputLength += chunk.byteLength;
                }
            } catch (error) {
                readError = error;
            }
        })();

        try {
            for (let offset = 0; offset < bytes.length; offset += COMPRESSION_CHUNK_SIZE) {
                await writer.write(bytes.subarray(offset, Math.min(offset + COMPRESSION_CHUNK_SIZE, bytes.length)));
            }
            await writer.close();
        } catch (error) {
            try { await writer.abort(error); } catch (_) {}
            throw error;
        }

        await readPromise;
        if (readError) throw readError;

        const wrapped = concatChunks(outputChunks, outputLength);
        // CompressionStream("deflate") produces a zlib-wrapped DEFLATE stream.
        // ZIP method 8 expects the raw DEFLATE payload, so remove the zlib header
        // and Adler-32 trailer. Keep the entry uncompressed when it saves no space.
        if (wrapped.length <= 6) return { method: 0, bytes };
        const raw = wrapped.subarray(2, wrapped.length - 4);
        return raw.length < bytes.length
            ? { method: 8, bytes: raw }
            : { method: 0, bytes };
    } catch (_) {
        return { method: 0, bytes };
    } finally {
        try { reader?.releaseLock(); } catch (_) {}
        try { writer?.releaseLock(); } catch (_) {}
    }
}

/**
 * Incremental ZIP builder. The caller can append one game at a time and finalize
 * only after the last game. This is used by the dedicated export Worker so the
 * benchmark Worker and IndexedDB are never coupled during ZIP creation.
 */
export function createBenchmarkZipBuilder(result) {
    if (!result || typeof result !== 'object') {
        throw new Error('ベンチマーク結果が不正です');
    }

    const encoder = new TextEncoder();
    const parts = [];
    const central = [];
    const { dosTime, dosDate } = dosDateTime();
    let offset = 0;
    let finalized = false;

    async function appendEntry(name, source) {
        if (finalized) throw new Error('ZIPはすでに完成しています');
        const bytes = typeof source === 'string' ? encoder.encode(source) : source;
        if (!(bytes instanceof Uint8Array)) {
            throw new Error(`ZIPエントリがUint8Arrayではありません: ${name}`);
        }
        if (bytes.length > ZIP_UINT32_MAX || offset > ZIP_UINT32_MAX) {
            throw new Error('ZIP形式の4GiB制限を超えています。試行数を分割してください');
        }

        const nameBytes = encoder.encode(name);
        if (nameBytes.length > ZIP_UINT16_MAX) {
            throw new Error(`ファイル名が長すぎます: ${name}`);
        }

        const crc = crc32(bytes);
        const compressed = await compressBytesForZip(bytes);
        if (compressed.bytes.length > ZIP_UINT32_MAX) {
            throw new Error('圧縮後のサイズがZIPの上限を超えています');
        }

        const local = makeZipLocalHeader(
            nameBytes,
            compressed.method,
            crc,
            compressed.bytes.length,
            bytes.length,
            dosTime,
            dosDate
        );
        const nextOffset = offset + local.length + compressed.bytes.length;
        if (nextOffset > ZIP_UINT32_MAX) {
            throw new Error('ZIP形式の4GiB制限を超えています。試行数を分割してください');
        }

        parts.push(local, compressed.bytes);
        central.push(makeZipCentralHeader(
            nameBytes,
            compressed.method,
            crc,
            compressed.bytes.length,
            bytes.length,
            dosTime,
            dosDate,
            offset
        ));
        offset = nextOffset;

        return {
            name,
            method: compressed.method,
            originalSize: bytes.length,
            compressedSize: compressed.bytes.length
        };
    }

    async function appendStandardFiles() {
        await appendEntry('summary.json', `${JSON.stringify(result, null, 2)}\n`);
        await appendEntry('README.txt', [
            'PuyoAI benchmark log archive',
            '',
            'summary.json: benchmark-wide summary.',
            'game_XXXX.json: one game, including per-turn decision logs when enabled.',
            'Logs are stored independently in IndexedDB while the benchmark runs.',
            ''
        ].join('\n'));
    }

    function finalize() {
        if (finalized) throw new Error('ZIPはすでに完成しています');
        finalized = true;
        const centralOffset = offset;
        const centralSize = central.reduce((sum, item) => sum + item.length, 0);
        if (centralOffset > ZIP_UINT32_MAX || centralSize > ZIP_UINT32_MAX || central.length > ZIP_UINT16_MAX) {
            throw new Error('ZIP形式の上限を超えています。試行数を分割してください');
        }
        parts.push(...central, makeZipEnd(central.length, centralSize, centralOffset));
        return new Blob(parts, { type: 'application/zip' });
    }

    return {
        appendEntry,
        appendStandardFiles,
        finalize,
        get entryCount() { return central.length; },
        get currentOffset() { return offset; }
    };
}

/**
 * Backward-compatible convenience API used by unit tests and any external code.
 * It still processes one game at a time and therefore does not build the raw
 * benchmark JSON in one giant object.
 */
export async function createBenchmarkZip(result, readGame, onProgress = () => {}) {
    if (typeof readGame !== 'function') {
        throw new Error('ゲームログ読み込み関数が指定されていません');
    }

    const builder = createBenchmarkZipBuilder(result);
    await builder.appendStandardFiles();

    const totalGames = Math.max(0, Number(result.games) || 0);
    for (let game = 0; game < totalGames; game += 1) {
        const record = await readGame(game);
        const json = typeof record === 'string' ? record : record?.json;
        if (typeof json !== 'string') {
            throw new Error(`ゲーム${game + 1}のログがIndexedDBにありません`);
        }
        const suffix = String(game + 1).padStart(4, '0');
        await builder.appendEntry(`game_${suffix}.json`, json);
        onProgress(game + 1, totalGames);
    }

    return builder.finalize();
}
