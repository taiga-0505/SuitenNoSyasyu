# クラス設計（確認課題 01_クラス設計）

## UML クラス図（01_05）

| ファイル | 内容 |
|---|---|
| `UML_Enemy.png` | 敵まわり。DamageableScript 階層（ポリモーフィズム / Template Method）、サメの State Pattern、Object Pool、Observer |
| `UML_Player.png` | 自機（RailShooterController）。武器の Strategy、水上/水中・生死の State Pattern、Object Pool |

再生成: `python make_uml.py` → `dot -Tpng -Gdpi=110 UML_Enemy.dot -o UML_Enemy.png`（Player も同様）

## 共通基盤

| ファイル | 役割 |
|---|---|
| `Application/Game/Framework/StateMachine.h` | State Pattern の基盤。`IState<Owner>` / `StateMachine<Owner>`。遷移は状態クラス自身が `ChangeState<次>()` で予約する |
| `Application/Game/Framework/GameEvents.h` | Observer。`GameEvent<Args...>`（Subject）と `EventConnection`（RAII で購読解除） |
| `Application/Game/Framework/EntityPool.h` | Object Pool。弾など頻繁に出し入れするエンティティの再利用 |
| `Application/Game/Scripts/DamageableScript.h` | 弾で壊せるものの基底。毎フレームの流れを固定（Template Method）し、差分を仮想関数で差し替える |

## デザインパターンの採用箇所（01_04）

| パターン | 主な場所 | 採用理由 |
|---|---|---|
| State | 敵 AI（Shark / Ship / Octopus / Bird）、WaveManager、自機、PauseMenu、Result、タイトル演出、カメラ演出、カットシーン | enum + switch に状態ごとの変数・遷移条件が混在していたため、状態 1 つ = クラス 1 つに分離 |
| Template Method | DamageableScript、SplashParticle（PooledParticleScript）、CameraZoneScript、CameraSequence | 流れが同じで差分だけ違う処理の重複を除去 |
| Strategy | 武器（IWeapon）、弾種・陣営（WaterBullet）、天候、宝箱の部位の動き、レール分岐条件 | 種類が増えても分岐を増やさずに済む |
| Observer | 敵撃破 → RailShooterGameMode が撃破数を集計 | 敵が集計先（GameSession）を知らずに済む |
| Object Pool | 自機弾・敵弾・砲弾 | 3 か所に重複していた再利用処理を集約 |
| Factory Method | 背景アクタ生成、サンゴ生成、敵の見た目生成、ステージデモ生成、Result の選択肢 | 種類 → クラスの対応を 1 か所に集約 |
| Command | タイトル / ポーズ / リザルトのメニュー項目 | 項目ごとの処理を if/switch から各コマンドの `Execute` へ |

Singleton（GameSession / StageProgress / GameSettings / AudioEngine）は、シーンをまたいで値を持つ用途に限定している。
