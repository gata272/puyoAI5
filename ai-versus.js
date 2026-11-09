/* ai-versus.js — ローカルAI対戦
 *
 * 既存のオンライン対戦（online.js / PeerJS）の相手を、ローカルで動く
 * AI に差し替えるモジュール。online.js から見ると PeerJS の接続と
 * 同じ形（open / on('open'|'data') / send）の仮想コネクションを返す
 * ため、対戦フロー（提案→承認→試合開始→盤面同期→おじゃま→勝敗→
 * 連戦）は既存コードがそのまま動く。
 *
 * AI 側のゲーム進行は puyoSim.js と同じルール（盤面 6x14・下12段の
 * み連結判定・スコア表・70点=おじゃま1個・相殺・最大30個落下・
 * 全消しボーナス2100）をヘッドレスで再現し、手の決定だけ既存の
 * WASM AI ワーカー（puyo-ai-worker-wasm.js）に依頼する。
 * プレイヤーと AI はホスト同期された同じネクスト列を共有する
 * （オンライン対戦の SYNC_NEXT と同じ仕組み）。
 */
(function (global) {
    'use strict';

    // ---------- 定数（puyoSim.js と一致させる） ----------
    const W = 6;
    const H = 14;
    const VISIBLE = 12;
    const EMPTY = 0;
    const GARBAGE = 5;
    const BONUS_CHAIN = [0, 8, 16, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 480, 512];
    const BONUS_GROUP = [0, 0, 0, 0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
    const BONUS_COLOR = [0, 0, 3, 6, 12];
    const ALL_CLEAR_BONUS = 2100;
    const NUISANCE_TARGET = 70;
    const MAX_OJAMA_DROP = 30;

    const AI_DEPTH = 3;
    const AI_BEAM_WIDTH = 24;
    const MOVE_HOLD_MS = 420;   // 手を決めてから置くまでの見せ時間
    const CHAIN_STEP_MS = 280;  // 連鎖1段ごとの表示間隔
    const NEXT_GAME_WAIT_MS = 150;

    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

    function emptyBoard() {
        return Array.from({ length: H }, () => Array(W).fill(EMPTY));
    }

    function shuffle(arr) {
        for (let i = arr.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [arr[i], arr[j]] = [arr[j], arr[i]];
        }
        return arr;
    }

    // puyoSim.js の buildBalancedNextBatch と同じ生成（128組・色均等）
    function buildBalancedBatch(pairs) {
        const n = pairs || 128;
        const pool = [];
        for (const c of [1, 2, 3, 4]) {
            for (let i = 0; i < (n * 2) / 4; i++) pool.push(c);
        }
        shuffle(pool);
        const batch = [];
        for (let i = 0; i < pool.length; i += 2) batch.push([pool[i], pool[i + 1]]);
        return batch;
    }

    // ---------- AI ワーカー（手の決定） ----------
    const WorkerBridge = {
        worker: null,
        ready: false,
        readyWaiters: [],
        pendingMove: null,

        ensure() {
            if (this.worker) return;
            // puyo-ai-worker-wasm.js は ES モジュール（import を使用）。
            // puyoAI.js と同じく type:'module' で生成しないと起動しない。
            this.worker = new Worker('./puyo-ai-worker-wasm.js', { type: 'module' });
            this.worker.onmessage = (e) => {
                const msg = e.data || {};
                if (msg.type === 'ready') {
                    this.ready = true;
                    this.readyWaiters.splice(0).forEach((r) => r());
                } else if (msg.type === 'move') {
                    if (this.pendingMove) {
                        const r = this.pendingMove;
                        this.pendingMove = null;
                        r({ x: msg.x, rotation: msg.rotation });
                    }
                } else if (msg.type === 'error') {
                    if (this.pendingMove) {
                        const r = this.pendingMove;
                        this.pendingMove = null;
                        r(null);
                    }
                }
            };
            this.worker.onerror = () => {
                if (this.pendingMove) {
                    const r = this.pendingMove;
                    this.pendingMove = null;
                    r(null);
                }
            };
        },

        waitReady() {
            this.ensure();
            if (this.ready) return Promise.resolve();
            return new Promise((r) => this.readyWaiters.push(r));
        },

        reset() {
            this.ensure();
            this.worker.postMessage({ type: 'reset' });
        },

        think(board, pieces, turn) {
            // ready が来ない場合も 6 秒で見切る（その後はフォールバック手で継続）
            return Promise.race([this.waitReady(), sleep(6000)]).then(
                () =>
                    new Promise((resolve) => {
                        // ワーカーが応答しない場合はタイムアウトで null を返し、
                        // エンジン側の合法手フォールバックで対戦を継続する
                        const timer = setTimeout(() => {
                            if (this.pendingMove === wrapped) {
                                this.pendingMove = null;
                                resolve(null);
                            }
                        }, 4000);
                        const wrapped = (move) => {
                            clearTimeout(timer);
                            resolve(move);
                        };
                        this.pendingMove = wrapped;
                        const boardBuffer = new Uint8Array(W * H);
                        for (let y = 0; y < H; y++) {
                            for (let x = 0; x < W; x++) {
                                boardBuffer[y * W + x] = board[y][x] & 0xff;
                            }
                        }
                        const pieceBuffer = new Uint8Array(6);
                        pieces.forEach((p, i) => {
                            if (!p) return;
                            pieceBuffer[i * 2] = p.main & 0xff;
                            pieceBuffer[i * 2 + 1] = p.sub & 0xff;
                        });
                        this.worker.postMessage({
                            type: 'think',
                            turn: turn,
                            depth: AI_DEPTH,
                            beamWidth: AI_BEAM_WIDTH,
                            debug: false,
                            boardBuffer: boardBuffer.buffer,
                            pieceBuffer: pieceBuffer.buffer,
                        });
                    })
            );
        },
    };

    // ---------- ヘッドレスエンジン（puyoSim.js のルール再現） ----------
    class AIEngine {
        constructor(deliver) {
            this.deliver = deliver; // AI → プレイヤー側へのメッセージ送信
            this.board = emptyBoard();
            this.queue = [];
            this.queuePtr = 0;
            this.current = null; // {main, sub, mainX, mainY, rotation}
            this.score = 0;
            this.lastChain = 0;
            this.pendingOjama = 0;
            this.nuisanceCarry = 0;
            this.turn = 0;
            this.state = 'idle'; // idle | playing | chaining | over
            this.generation = 0; // ゲーム切替時に古いループを止める
        }

        gravity() {
            for (let x = 0; x < W; x++) {
                let write = 0;
                for (let y = 0; y < H; y++) {
                    const c = this.board[y][x];
                    if (c !== EMPTY) {
                        this.board[write++][x] = c;
                    }
                }
                while (write < H) this.board[write++][x] = EMPTY;
            }
        }

        findGroups() {
            const visited = Array.from({ length: H }, () => Array(W).fill(false));
            const groups = [];
            for (let y = 0; y < VISIBLE; y++) {
                for (let x = 0; x < W; x++) {
                    const color = this.board[y][x];
                    if (visited[y][x] || color < 1 || color > 4) continue;
                    const group = [];
                    const stack = [{ x: x, y: y }];
                    visited[y][x] = true;
                    while (stack.length) {
                        const cur = stack.pop();
                        group.push(cur);
                        const dirs = [[1, 0], [-1, 0], [0, 1], [0, -1]];
                        for (const d of dirs) {
                            const nx = cur.x + d[0], ny = cur.y + d[1];
                            if (nx < 0 || nx >= W || ny < 0 || ny >= VISIBLE) continue;
                            if (visited[ny][nx] || this.board[ny][nx] !== color) continue;
                            visited[ny][nx] = true;
                            stack.push({ x: nx, y: ny });
                        }
                    }
                    if (group.length >= 4) groups.push({ group: group, color: color });
                }
            }
            return groups;
        }

        // puyoSim.js calculateScore と同じ（1連鎖目は CHAIN[0] を使う）
        calculateScore(groups, chainStep) {
            let total = 0;
            let bonus = 0;
            const colors = new Set();
            for (const g of groups) {
                total += g.group.length;
                colors.add(g.color);
                bonus += BONUS_GROUP[Math.min(g.group.length, BONUS_GROUP.length - 1)];
            }
            bonus += BONUS_CHAIN[Math.max(0, Math.min(chainStep - 1, BONUS_CHAIN.length - 1))];
            bonus += BONUS_COLOR[Math.min(colors.size, BONUS_COLOR.length - 1)];
            return 10 * total * (bonus === 0 ? 1 : bonus);
        }

        coordsFor(x, y, rotation) {
            let sx = x, sy = y;
            if (rotation === 0) sy = y + 1;
            else if (rotation === 1) sx = x - 1;
            else if (rotation === 2) sy = y - 1;
            else if (rotation === 3) sx = x + 1;
            return [{ x: x, y: y }, { x: sx, y: sy }];
        }

        canPlace(x, y, rotation) {
            const coords = this.coordsFor(x, y, rotation);
            for (const c of coords) {
                if (c.x < 0 || c.x >= W || c.y < 0 || c.y >= H) return false;
                if (this.board[c.y][c.x] !== EMPTY) return false;
            }
            return true;
        }

        // C++ Simulator::findDropY と同じ
        findDropY(x, rotation) {
            let y = H - 2;
            if (!this.canPlace(x, y, rotation)) return -1;
            while (y > 0 && this.canPlace(x, y - 1, rotation)) y--;
            return y;
        }

        legalMoves() {
            const moves = [];
            for (let x = 0; x < W; x++) {
                for (let r = 0; r < 4; r++) {
                    if (this.findDropY(x, r) >= 0) moves.push({ x: x, rotation: r });
                }
            }
            return moves;
        }

        sendUpdate() {
            this.deliver({
                type: 'BOARD_UPDATE',
                board: this.board.map((row) => row.slice()),
                currentPuyo: this.current
                    ? {
                          mainX: this.current.mainX,
                          mainY: this.current.mainY,
                          rotation: this.current.rotation,
                          mainColor: this.current.main,
                          subColor: this.current.sub,
                      }
                    : null,
                gameState: this.state === 'chaining' ? 'chaining' : 'playing',
                score: this.score,
                chainCount: this.lastChain,
                ojamaPending: this.pendingOjama,
            });
        }

        startGame(queue) {
            this.generation++;
            this.board = emptyBoard();
            this.queue = Array.isArray(queue) && queue.length ? queue.slice() : buildBalancedBatch();
            this.queuePtr = 0;
            this.current = null;
            this.score = 0;
            this.lastChain = 0;
            this.pendingOjama = 0;
            this.nuisanceCarry = 0;
            this.turn = 0;
            this.state = 'playing';
            WorkerBridge.reset();
            this.spawnNext(this.generation);
        }

        stopGame() {
            this.generation++;
            this.state = 'idle';
            this.current = null;
        }

        peekPair(offset) {
            while (this.queuePtr + offset >= this.queue.length) {
                this.queue.push.apply(this.queue, buildBalancedBatch());
            }
            const pair = this.queue[this.queuePtr + offset];
            return { main: pair[1], sub: pair[0] };
        }

        spawnNext(gen) {
            if (gen !== this.generation) return;
            const pair = this.peekPair(0);
            this.queuePtr++;
            this.current = { main: pair.main, sub: pair.sub, mainX: 2, mainY: H - 2, rotation: 0 };
            // 出現時衝突 = ゲームオーバー（puyoSim.js generateNewPuyo と同じ）
            if (this.board[H - 2][2] !== EMPTY || this.board[H - 1][2] !== EMPTY) {
                this.gameOver();
                return;
            }
            this.state = 'playing';
            this.sendUpdate();
            setTimeout(() => this.takeTurn(gen), NEXT_GAME_WAIT_MS);
        }

        async takeTurn(gen) {
            if (gen !== this.generation || this.state !== 'playing' || !this.current) return;
            // 現在のペアは spawnNext で消費済み（queuePtr-1）。
            // AI には [現在, ネクスト, ネクネク] を渡す。
            const cur = this.queue[this.queuePtr - 1];
            const pieces = [
                { main: cur[1], sub: cur[0] },
                this.peekPair(0),
                this.peekPair(1),
            ];

            let move = await WorkerBridge.think(this.board, pieces, this.turn);
            if (gen !== this.generation || this.state !== 'playing') return;
            await sleep(MOVE_HOLD_MS);
            if (gen !== this.generation || this.state !== 'playing' || !this.current) return;

            if (!move || this.findDropY(move.x, move.rotation) < 0) {
                const legal = this.legalMoves();
                if (!legal.length) {
                    this.gameOver();
                    return;
                }
                move = legal[0];
            }

            const y = this.findDropY(move.x, move.rotation);
            const coords = this.coordsFor(move.x, y, move.rotation);
            this.board[coords[0].y][coords[0].x] = this.current.main;
            this.board[coords[1].y][coords[1].x] = this.current.sub;
            // 最上段は連鎖判定に使わない（puyoSim.js / Simulator と同じ）
            for (let x = 0; x < W; x++) this.board[H - 1][x] = EMPTY;
            this.current = null;
            this.turn++;
            this.sendUpdate();
            await this.resolve(gen);
        }

        async resolve(gen) {
            this.state = 'chaining';
            let chainStep = 0;
            let attackBuffer = 0;
            while (true) {
                if (gen !== this.generation) return;
                this.gravity();
                const groups = this.findGroups();
                if (!groups.length) {
                    let isEmpty = true;
                    for (const row of this.board) {
                        for (const c of row) if (c !== EMPTY) isEmpty = false;
                    }
                    if (isEmpty) {
                        this.score += ALL_CLEAR_BONUS;
                        attackBuffer += ALL_CLEAR_BONUS;
                    }
                    // ゲームオーバー判定（puyoSim.js runChain と同じ位置）
                    if (this.board[VISIBLE - 1][2] !== EMPTY) {
                        this.gameOver();
                        return;
                    }
                    this.flushAttack(attackBuffer);
                    // 保留おじゃまの落下（最大30個）
                    if (!this.dropPendingOjama()) {
                        this.gameOver();
                        return;
                    }
                    this.lastChain = chainStep;
                    this.state = 'playing';
                    this.spawnNext(gen);
                    return;
                }
                chainStep++;
                const stepScore = this.calculateScore(groups, chainStep);
                this.score += stepScore;
                attackBuffer += stepScore;
                this.lastChain = chainStep;
                // 消去＋隣接おじゃまの巻き込み消去
                const erased = [];
                for (const g of groups) {
                    for (const cell of g.group) {
                        this.board[cell.y][cell.x] = EMPTY;
                        erased.push(cell);
                    }
                }
                for (const cell of erased) {
                    const dirs = [[1, 0], [-1, 0], [0, 1], [0, -1]];
                    for (const d of dirs) {
                        const nx = cell.x + d[0], ny = cell.y + d[1];
                        if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
                            this.board[ny][nx] === GARBAGE) {
                            this.board[ny][nx] = EMPTY;
                        }
                    }
                }
                this.sendUpdate();
                await sleep(CHAIN_STEP_MS);
            }
        }

        // puyoSim.js flushChainOjamaBuffer と同じ換算・相殺
        flushAttack(attackBuffer) {
            this.nuisanceCarry += attackBuffer / NUISANCE_TARGET;
            const attack = Math.floor(this.nuisanceCarry);
            this.nuisanceCarry -= attack;
            if (attack <= 0) return;
            const canceled = Math.min(this.pendingOjama, attack);
            this.pendingOjama -= canceled;
            const outgoing = attack - canceled;
            if (outgoing > 0) {
                this.deliver({ type: 'OJAMA', amount: outgoing });
            }
        }

        // puyoSim.js dropOjamaToBoard と同じ落下パターン
        dropPendingOjama() {
            if (this.pendingOjama <= 0) return true;
            const amount = Math.min(MAX_OJAMA_DROP, this.pendingOjama);
            this.pendingOjama -= amount;
            let emptyCells = 0;
            for (const row of this.board) {
                for (const c of row) if (c === EMPTY) emptyCells++;
            }
            if (amount > emptyCells) return false;
            const placeOne = (x) => {
                let h = 0;
                for (let y = 0; y < H; y++) if (this.board[y][x] !== EMPTY) h++;
                if (h >= H) return false;
                this.board[h][x] = GARBAGE;
                return true;
            };
            const fullRounds = Math.floor(amount / W);
            const remainder = amount % W;
            for (let r = 0; r < fullRounds; r++) {
                const cols = shuffle([0, 1, 2, 3, 4, 5]);
                for (const x of cols) if (!placeOne(x)) return false;
            }
            if (remainder > 0) {
                const cols = shuffle([0, 1, 2, 3, 4, 5]);
                for (let i = 0; i < remainder; i++) {
                    if (!placeOne(cols[i])) return false;
                }
            }
            this.gravity();
            this.sendUpdate();
            return true;
        }

        gameOver() {
            if (this.state === 'over') return;
            this.state = 'over';
            this.current = null;
            this.sendUpdate();
            this.deliver({ type: 'OPPONENT_LOST' });
            if (typeof this.onGameOver === 'function') this.onGameOver();
        }
    }

    // ---------- 仮想コネクション（PeerJS 接続と同じ形） ----------
    global.createAIConnection = function () {
        const handlers = {};
        let engine = null;
        let winTarget = 1;
        let aiWins = 0;
        let playerWins = 0;
        let awaitingStart = true; // 次ゲームの SYNC_NEXT 待ち

        const conn = {
            open: true,
            on(evt, cb) {
                handlers[evt] = cb;
                if (evt === 'open') setTimeout(() => cb(), 0);
            },
            send(data) {
                if (!data || !data.type) return;
                switch (data.type) {
                    case 'PROPOSE_MATCH':
                        winTarget = data.winTarget || 1;
                        aiWins = 0;
                        playerWins = 0;
                        awaitingStart = true;
                        setTimeout(() => deliver({ type: 'ACCEPT_MATCH', winTarget: winTarget }), 250);
                        break;
                    case 'SYNC_NEXT':
                        if (engine && awaitingStart) {
                            awaitingStart = false;
                            engine.startGame(data.nextPuyos);
                        }
                        break;
                    case 'OJAMA':
                        if (engine) {
                            engine.pendingOjama += Math.max(0, Math.floor(Number(data.amount) || 0));
                            engine.sendUpdate();
                        }
                        break;
                    case 'OPPONENT_LOST':
                        // プレイヤーが負けた（AI の勝ち）
                        if (engine) engine.stopGame();
                        aiWins++;
                        awaitingStart = aiWins < winTarget && playerWins < winTarget;
                        break;
                    case 'OPPONENT_SURRENDERED':
                        if (engine) engine.stopGame();
                        awaitingStart = false;
                        break;
                    case 'REMATCH_REQUEST':
                        winTarget = data.winTarget || winTarget;
                        aiWins = 0;
                        playerWins = 0;
                        awaitingStart = true;
                        setTimeout(() => deliver({ type: 'REMATCH_ACCEPT', winTarget: winTarget }), 250);
                        break;
                    default:
                        // BOARD_UPDATE などは AI 側では使わない（v1: 凝視なし）
                        break;
                }
            },
        };

        function deliver(data) {
            if (handlers.data) handlers.data(data);
        }

        engine = new AIEngine(deliver);
        engine.onGameOver = () => {
            // AI が負けた（プレイヤーの勝ち）
            playerWins++;
            awaitingStart = aiWins < winTarget && playerWins < winTarget;
        };
        return conn;
    };

    // ---------- オンラインメニューへのボタン追加 ----------
    function injectButton() {
        const box = document.querySelector('#online-overlay .online-box');
        if (!box || document.getElementById('ai-match-btn')) return true;
        const btn = document.createElement('button');
        btn.className = 'online-btn';
        btn.id = 'ai-match-btn';
        btn.textContent = 'AIと対戦（ローカル）';
        btn.style.backgroundColor = '#2e9e5b';
        btn.onclick = () => {
            if (typeof global.startAIMatch === 'function') global.startAIMatch();
        };
        const note = document.createElement('div');
        note.style.cssText = 'font-size: 0.8em; color: #aaa; margin-top: 4px;';
        note.textContent = 'ID不要：この端末上のAIと対戦します';
        const cancel = box.querySelector('button[onclick*="hideOnlineOverlay"]');
        if (cancel) {
            box.insertBefore(btn, cancel);
            box.insertBefore(note, cancel);
        } else {
            box.appendChild(btn);
            box.appendChild(note);
        }
        return true;
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', () => {
            let tries = 0;
            const timer = setInterval(() => {
                if (injectButton() || ++tries > 20) clearInterval(timer);
            }, 300);
        });
    } else {
        let tries = 0;
        const timer = setInterval(() => {
            if (injectButton() || ++tries > 20) clearInterval(timer);
        }, 300);
    }
})(window);
