# AI vNext: root-action preservation and contextual cashout control

基準: `gata272/puyoAI5` の `main` 時点の `ai/search/beam_search.cpp`。

## 変更の目的

- 探索途中の board-hash dedupe が異なる root action を消してしまう問題を防ぐ。
- beam pruning の各層で root action の多様性を維持する。
- 7～9連鎖を一律に抑制せず、「発火前に盤面が安全で、かつ潜在的な長連鎖構築資産が十分ある」場合だけ cashout を弱く抑制する。
- 未計測の `rootTrueTriggerPath` / `rootTriggerRoute` を値 0 と誤認しない。
- V6 で観測された broad Rescue / Emergency による 12→9、12→8、13→11 のような高連鎖候補の切り捨てを避ける。

## 主な変更

### 1. Root-aware transposition

従来は同じ `boardHash` に到達したノードを root action に関係なく1つへ統合していた。

現在は `boardHash + root(x, rotation)` をキーにしている。同一盤面でも異なる初手の代表ノードを保持できるため、深い層で「初手Aの強い枝が初手Bを消す」現象を抑える。

### 2. 探索中の root diversity reserve

各 pruning 層の最初に最大6 root action を1ノードずつ確保する。その後に danger-zone 用の小さな survival reserve を追加し、最後に通常 utility 順で埋める。

安全性は通常 utility に大きく足さず、root の選択肢を消さないことを主目的にしている。

### 3. Contextual anti-cashout

7～9連鎖そのものを悪とせず、発火前盤面が比較的安全で、`chainUnit4/5`、handoff、futureChainSpace、tailSpace、buildSpace などから潜在構築力が高い場合のみ、7～9連鎖の reward に弱い penalty を加える。

危険盤面では penalty を入れないため、回復のための7～9連鎖を抑制しない。

### 4. Root probe validity flags

`hasRootTrueProbe` / `hasRootRouteProbe` を追加した。未プローブ時の初期値0と、実測値0を区別する。

### 5. Final rescue を critical-only に縮小

従来の stale-route Rescue / Emergency を削り、`rootFutureSafeMoves <= 1` の genuinely critical な状態だけを対象にした。escape 側にも最低3 safe moves と chain value の条件を課している。

## 検証

- `g++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only` 相当の構文チェックを実施。
- 実リポジトリ全体をこの実行環境へ clone するネットワーク経路が利用できなかったため、完全な native / Emscripten 実ビルドまではこの ZIP 作成段階では実行していない。
