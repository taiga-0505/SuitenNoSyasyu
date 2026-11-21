# ChasoEngine

DirectX 12 製の自作ゲームエンジン。ゲームリポジトリからは `project/Chaso/` に **git subtree** として取り込んで使う。

```
ゲームリポジトリ/
  project/
    chaso.sln, main.cpp                      ← ゲーム側（Template からコピー）
    Application/ChasoApp.vcxproj             ← ゲーム側
    Application/Game/Scripts/                ← ゲーム側（スクリプト）
    Application/Game/Framework/              ← ゲーム側（GameMode の派生・シーンをまたぐ状態・GameSetup.cpp）
    Resources/                               ← 実行時に使う唯一の Resources（ゲーム + エンジン由来のコピー）
      Setting/                               ← AppConfig.json / GameSettings.json / EditorConfig.json
    Chaso/                                   ← ★ このリポジトリ（subtree）
      Engine/       エンジン本体（ChasoEngine.vcxproj）
      Externals/    imgui / assimp / DirectXTex / nlohmann / curl / httplib
      Framework/    App（アプリのライフサイクル）, AppConfig
      Editor/       EditorManager, CaptureMode, EditorExtension（ImGui エディタ）
      Game/         Game, Scene, SceneManager, DataDrivenScene, Fade, Framework/（GameModeBase, GameStateBase）
      Resources/    エンジン用リソースの原本（ビルド前に project/Resources へ同期される）
      Template/     新しいゲームを作るときに project/ へコピーするひな形（_root/ はリポジトリ直下へ）
      scripts/      同期用スクリプト
```

## リソースの扱い

実行ファイルが見るのは従来どおり **`project/Resources/` ひとつだけ**。配布時もこのフォルダを exe と一緒に置けばよい。

エンジンが必要とするリソース（Shader / icons / fonts / noise / Particle / Template / uvChecker / white1x1）の
原本は ChasoEngine 側の `Chaso/Resources/` にあり、**ビルド前に `scripts/sync-resources.ps1` が自動で同期**する
（`ChasoApp.vcxproj` の PreBuildEvent）:

1. `Chaso/Resources` → `Resources`: 新しい・無いファイルをコピー
2. `Resources` → `Chaso/Resources`: エンジン側に既にあるファイルだけ、更新されていれば書き戻す（新しい方が勝つ）
3. `Resources/.gitignore` を自動生成し、エンジン由来のコピーはゲームリポジトリで追跡しない

つまり **エンジンのシェーダーやフォントは `project/Resources/` で直接編集してよい**。次のビルドで
`Chaso/Resources` に書き戻され、`engine-push.ps1` で ChasoEngine に反映される。
新しいファイルをエンジンに追加するときだけ `Chaso/Resources/` に置く（`Resources/` に置くとゲーム固有扱い）。
削除は同期されないので、両方から消すこと。

保険として `Chaso::ResolvePath()`（`Engine/Common/ResourcePath.h`）が「`Resources/` に無ければ `Chaso/Resources/`」も
探すので、同期前でも起動はできる。

## ゲームごとに変える設定（`project/Resources/Setting/AppConfig.json`）

```json
{
  "width": 1280, "height": 720, "fullscreen": false,
  "title":     { "Debug": "ChasoEngine", "Development": "〇〇_CG4", "Release": "〇〇_ゲーム名" },
  "bootScene": { "Debug": "Title",       "Development": "Stage1",  "Release": "Title" }
}
```

`title` がウィンドウタイトル、`bootScene` が起動時に読むシーン名（`Resources/Scenes/<name>.json`）。
どちらも文字列 1 つ（全構成共通）でも、`Debug` / `Development` / `Release` の構成別オブジェクトでも書ける
（無い構成は `Release` → 既定値の順で補う）。エディタからの保存（`SaveAppConfig`）はウィンドウ設定だけ上書きし、
これらのキーは壊さない。

## 日常の使い方（ゲームリポジトリ側で実行）

| やりたいこと | コマンド |
|---|---|
| 新しいゲームを作る | `.\project\Chaso\scripts\new-game.ps1 -Name MyGame`（作成先はターミナルで聞かれる。Enter で初期値、パス入力、`?` でフォルダ選択ダイアログ。選んだ場所は PC ごとに記憶して次回の初期値になる。引数なしなら名前も聞かれる。`-Dest D:\production` のように指定すればダイアログは出ない） |
| 学校以外のメンバーと作るゲームを作る | `.\project\Chaso\scripts\new-game.ps1 -External`（公開先の GitHub アカウントとコミット用メールも聞かれる。ボタンが外部用になり、`.chaso.json` が作られる） |
| 作ったゲームを GitHub に公開（初回 push）・学校 | `.\project\Chaso\scripts\publish-github.ps1`（`-Mode school`。gh があれば自動作成、無ければ URL を聞く） |
| 作ったゲームを GitHub に公開（初回 push）・外部 | `.\project\Chaso\scripts\publish-github.ps1 -Mode external`（`.chaso.json` の `githubOwner` に作成。origin を `https://<アカウント>@github.com/...` にしてこのリポジトリだけ別アカウントで認証） |
| リソースを手動で同期する（通常はビルド前に自動） | `.\project\Chaso\scripts\sync-resources.ps1` |
| ゲーム中にエンジンを直した → ChasoEngine へ反映 | `.\project\Chaso\scripts\engine-push.ps1` |
| ChasoEngine の更新を取り込む | `.\project\Chaso\scripts\engine-pull.ps1` |

- `engine-push` は `project/Chaso/` 配下に触れたコミットだけを切り出して送る。ゲームのコードは送られない。
- 別のゲームで先にエンジンが更新されていて push が拒否されたら、`engine-pull` → 競合解決 → `engine-push`。
- エンジンだけ直したいときは、どれかのゲームリポジトリの `project/Chaso/` で直して push すればよい。
  ChasoEngine を単体で clone しても Visual Studio では開けない（sln はゲーム側にある）。

VS Code の Project Actions（`.project-actions.json`）にも同じボタンがある。新規ゲームにもこのボタン設定と `scripts/build/` が付いてくる。

| 種類 | ボタン |
|---|---|
| 通常（学校） | エンジン反映 / エンジン取込 / リソース同期 / 新規ゲーム / 新規ゲーム(外部) / GitHub公開(学校) |
| 外部（`new-game -External` で作ったゲーム。`Template/_root_external/` で上書き） | エンジン反映 / エンジン取込 / リソース同期 / GitHub公開(別アカウント) |

- 外部ゲームもエンジンは同じ ChasoEngine（taiga-0505/ChasoEngine）を subtree で使う。外部メンバーが「エンジン反映」するには ChasoEngine への書き込み権限が必要。
- 外部ゲームの「GitHub公開(学校)」は誤公開防止のため止まる（`.chaso.json` の `mode` で判定）。

## 手動で同じことをする場合

```powershell
# 反映
git subtree push --prefix=project/Chaso https://github.com/taiga-0505/ChasoEngine.git main
# 取り込み
git subtree pull --prefix=project/Chaso https://github.com/taiga-0505/ChasoEngine.git main --squash
# 新規ゲームへ追加
git subtree add  --prefix=project/Chaso https://github.com/taiga-0505/ChasoEngine.git main --squash
```

## シーン遷移（`project/Resources/SceneFlow.json`）

「どのシーンで・何が起きたら・どこへ・どの演出で」遷移するかを 1 つの表で持つ。
エディタの **Window > Scene Flow (シーン遷移)** で編集する（編集は即反映、「保存」でファイルへ）。

```cpp
// スクリプト（ScriptableEntity）からはシーン名を書かず、きっかけ名だけで遷移する
RequestTransition("start");            // 表の行き先・演出で遷移
RequestTransition("stage", "Stage3");  // 遷移先が $arg の行なら、渡した名前へ
LookupTransition("next", target, transition); // 引くだけ（独自演出を挟む・ボタンを隠す判定など）
```

| 列 | 書き方 |
|---|---|
| 遷移元 | シーン名 / `Stage*`（前方一致）/ `*`（全シーン）。完全一致 → 長い前方一致 → `*` の順で優先 |
| きっかけ | スクリプトや `GameMode::EvaluateOutcome` が渡す名前 |
| 遷移先 | シーン名 / `$current`（今のシーン）/ `$arg`（スクリプトが渡す）/ Application が登録した変数 |
| 演出 | `dissolve` / `dive` |

表に無いきっかけで遷移しようとすると、パネルの「未登録のきっかけ」に出る（「追加」で行を作れる）。
未登録のシーン名・重複行などは「チェック」に出る。新規ゲームには空の `SceneFlow.json` が付く。

## 当たり判定クエリ（レイキャストなど）

`ColliderComponent` を持つエンティティに対して、BVH を使ったクエリが撃てる（AABB / 球。Capsule は他の判定と同じく AABB 扱い）。

```cpp
// スクリプト（ScriptableEntity）から。自分自身は自動で除外される
RaycastHit hit;
if (Raycast(muzzle, forward, 100.0f, hit)) {
  hit.entity->Destroy();          // hit.point / hit.normal / hit.distance も取れる
}
if (SphereCast(pos, 0.3f, {0, -1, 0}, 0.2f, hit)) { /* 接地している */ }

// シーンから（レイヤーで絞る・トリガーも含めるなど）
QueryFilter f;
f.layerMask = kLayerEnemy;        // ColliderComponent::layer と AND して 0 なら無視
f.includeTriggers = true;
std::vector<Entity*> found;
GetScene()->OverlapSphere(pos, 3.0f, found, f);
GetScene()->RaycastAll(origin, dir, 50.0f, hits, f);  // 近い順
GetScene()->Raycast(RC::ScreenPointToRay(mouse, w, h, view, proj), 1000.0f, hit); // マウスで選ぶ
```

- 起点がコライダーの中にあるときは `distance = 0` で当たりになる。
- 同じフレーム中に物を大きく（0.25m 以上）動かした直後に撃つなら、先に `GetScene()->SyncColliderBroadphase()`。
- そのフレームに生成したばかりの物は、次のフレームから当たる。

## プレハブ

エンティティ（子孫込み）を `Resources/Prefabs/<名前>.prefab.json` に保存し、実行時に何個でも生成できる。

| やりたいこと | 方法 |
|---|---|
| 保存する | Hierarchy でエンティティを右クリック →「Save as Prefab」（同名は上書き） |
| エディタで置く | Content Browser の `.prefab.json` を Viewport へドラッグ＆ドロップ |
| スクリプトから生成 | `InstantiatePrefab("Prefabs/Bullet", muzzlePos);`（`"Bullet"` だけでも可） |
| シーンから生成 | `scene->InstantiatePrefab("Prefabs/Bullet", pos);` / 保存時の位置のままなら `scene->InstantiatePrefab("Prefabs/Bullet")` |

- 生成のたびに GUID を振り直す。ルートを指定位置に置き、子孫も同じだけずらす。
- 更新ループ中に呼んでよい（生成した物はそのフレームの更新の後でシーンに加わる）。
- ファイルは初回だけ読んでキャッシュする。手でファイルを書き換えたら `Prefab::ClearCache()`（エディタからの保存は自動で反映）。

## コンポーネントの更新順

`Entity::UpdateAll` は `IComponent::UpdateOrder()` の小さい順（同じなら型名順）に `Update` を呼ぶ。
既定はスクリプト（`kScript = 0`）→ 剛体（`kPhysics = 100`）なので、スクリプトが書いた `velocity` はそのフレームで位置に反映される。
自作コンポーネントで順番を決めたいときは `UpdateOrder()` を override して `ComponentUpdateOrder::kEarly / kLate` などを返す。

## 入力アクション（キーコンフィグ）

キーコードを直接書かず、`"Jump"` などの名前で入力を読む。1 つの名前にキーボード・マウス・パッドを複数割り当てられる。

```cpp
// 既定の割り当て（ゲーム側の初期化で 1 回。GameSetup.cpp など）
auto& in = Chaso::InputActions::Get();
in.Define("Jump",  {"Key:SPACE", "Pad:A"});
in.Define("Fire",  {"Mouse:Left", "Pad:RT"});
in.Define("MoveX", {"Key:D", "-Key:A", "Pad:LX"});   // "-" で逆向き
in.Define("MoveY", {"Key:W", "-Key:S", "Pad:LY"});

// スクリプトから
if (in.IsTriggered("Jump")) { ... }                   // IsPressed / IsReleased / GetValue も
RC::Vector2 move = in.GetVector2("MoveX", "MoveY");   // WASD とスティックが同じ値になる（長さ最大 1）
```

- 書き方は `[-]デバイス:名前[*倍率]`。`Key:A` `Key:LSHIFT` `Mouse:Right` `Pad:B` `Pad:LB` `Pad:LT` `Pad:LY+`（スティックの片側）など。一覧は `Engine/Input/InputActions.h`。
- スティックはデッドゾーン（既定 0.2）の内側を 0 にする。
- キーコンフィグ画面を作るなら `CaptureNextBinding()`（このフレームに押された入力を拾う）→ `SetBindings()` → `Save(Chaso::kInputActionsPath)`。
  保存した `Resources/Setting/InputActions.json` は次回起動時に読まれ、`Define` より優先される。`ResetToDefault()` で戻せる。

## 時間（タイムスケール・ヒットストップ・スロー）

スクリプトの `OnUpdate(deltaTime)`・Rigidbody・アニメーション・パーティクルには、倍率込みの「ゲーム時間」が渡る。

```cpp
auto& time = Chaso::Time::Get();
time.HitStop(0.08f);               // 0.08 秒（実時間）止める。第 2 引数で「ほぼ止める」倍率も指定できる
time.SlowMotion(0.3f, 1.5f, 0.5f); // 0.3 倍速を 1.5 秒。最後の 0.5 秒で等速へ戻す
time.SetTimeScale(0.5f);           // ずっと掛けておく倍率
float uiDt = time.UnscaledDeltaTime(); // 倍率を無視した実時間（ヒットストップ中も動かしたい UI 用）
```

- 再生開始・停止のたびに倍率と演出は初期状態へ戻る。
- ポーズは今まで通り `SceneContext::gamePaused`（ポーズ中は演出の残り時間も止まる）。
- `SceneContext::deltaTime` は実時間のまま（ポーズメニューのスクリプトはこれを読めばよい）。

## Tween（値をなめらかに動かす）

```cpp
// スクリプトから。Tweens() は自分のエンティティに TweenComponent を自動で付けて返す
auto* tr = GetComponent<TransformComponent>();
Tweens().To(tr->scale, {1.2f, 1.2f, 1.2f}, 0.15f, Chaso::EaseType::OutBack)
        .Loop(2, Chaso::LoopMode::Yoyo);                   // ぽよんと膨らんで戻る
Tweens().To(tr->position, target, 0.5f).Delay(1.0f)
        .OnComplete([this] { GetEntity()->Destroy(); });   // 1 秒待って移動し、着いたら消える
Tweens().After(2.0f, [this] { RequestTransition("next"); }); // タイマー
Tweens().Value(0.3f, [this](float k) { power_ = k * 100.0f; }); // 任意の処理に 0〜1 を渡す
Tweens().To(...).Unscaled();                               // ヒットストップ・スローを無視（UI 用）
```

- 動かせるのは `float` / `Vector2` / `Vector3` / `Vector4`（色）。イージングは `Chaso::EaseType`（Sine / Quad / Cubic / Back / Elastic / Bounce など 31 種）。
- 動かす値は自分のエンティティのコンポーネントにすること（エンティティと一緒に Tween も消えるので安全）。
- 後で止めたいときは `.Id()` を保存して `Tweens().Kill(id)`（`Kill(id, true)` で最後まで進めて完了扱い）。
- `TweenComponent` は実行時だけのものなので、シーンファイル・プレハブ・Undo 履歴には保存されない。

## 物理（Rigidbody）

```cpp
auto* rb = GetComponent<RigidbodyComponent>();
rb->AddForce({0, 0, 10});                            // 押し続ける力（毎フレーム呼ぶ）
rb->AddForce({0, 5, 0}, ForceMode::Impulse);         // 一瞬の力（ジャンプ・ノックバック）
rb->AddForce({0, 3, 0}, ForceMode::VelocityChange);  // 質量を無視して速度を足す
rb->restitution = 0.8f;  // 跳ね返り（0〜1）
rb->friction = 0.5f;     // 摩擦（0〜1）
```

- Inspector でも `Gravity Scale`（重力の倍率）・`Damping`（空気抵抗）・`Restitution`・`Friction`・`Max Speed` を設定できる。
- 1 フレームの時間を 1/60 秒以下に分けて計算するので、30fps でも 144fps でもジャンプの高さがほぼ同じになる。
  毎回ぴったり 1/60 秒で進める方式にしていないのは、144Hz などで「動くフレーム／動かないフレーム」が交互に出てカクつくのを避けるため。
- 反発と摩擦は、ぶつかった 2 体のうち大きい方の値を使う（壁に Rigidbody が無くても、ボール側に設定すれば跳ねる）。
  両方 0（既定）のときは以前と同じ動き。
- 重力・刻み幅などの全体設定は `PhysicsSettings::Get()`（`gravity` / `maxStep` / `bounceThreshold`）。

## セーブデータ

```cpp
auto& save = Chaso::SaveData::Get();
save.Load("slot1");                              // 無ければ空から（新規ゲーム）
save.Set("stage.cleared", 3);                    // "." で階層を作れる
int best = save.Get("best.score", 0);            // 無ければ第 2 引数
save.Save();                                     // Saves/slot1.json に書く
for (auto& s : save.ListSlots()) { s.name; s.savedAt; }  // セーブ画面用の一覧
```

- 一時ファイルに書いてから置き換え、前のファイルを `.bak` として残す。本体が壊れていたら `.bak` から読む。
- データの形を変えたら `SetVersion(2)` と `SetMigration([](int from, nlohmann::json& d) { ... })` で古いセーブを直せる。
- 保存先は既定で作業フォルダの `Saves/`（`SetDirectory` で変更可）。配布時は exe と同じ場所に作られる。

## ホットリロード（Debug / Development ビルド）

ゲームを動かしたまま、ファイルを保存するだけで反映される（再起動不要）。

| 対象 | 監視する場所 | 反映のしかた |
|---|---|---|
| シェーダー（`.hlsl`） | `project/Resources/Shader/` | そのシェーダーを使うパイプラインだけ作り直す |
| 共通ヘッダー（`.hlsli`） | 同上 | 全パイプラインを作り直す（数秒かかる） |
| テクスチャ | 今読み込んでいる画像ファイル | 同じ SRV のまま中身を差し替える |

- コンパイルエラーのときはログに出して前のシェーダーのまま動き続ける（直して保存し直せばよい）。
- エンジンのシェーダーは `project/Resources/Shader/` 側を編集すること（次のビルドで `Chaso/Resources` へ書き戻される）。
- 反映するときに GPU の完了を一度待つので、保存した瞬間だけ一瞬止まる。

## HDR とトーンマップ

`project/Resources/Setting/AppConfig.json` に `"hdr": true` を書いて起動すると、シーンを浮動小数点（R16G16B16A16_FLOAT）で描き、
ポストプロセスの最後にトーンマップで画面の明るさへ収める。光源やブルームの明るい部分が白飛びしにくくなる。

- 既定は `false`（従来どおり 8bit で描く）。切り替えには再起動が必要。
- 露出と曲線（ACES / Reinhard / None）はエディタの Post Effect Settings、またはコードから
  `GetSceneContext()->postProcess->SetTonemapExposure(ev)` / `SetTonemapOperator(TonemapOperator::ACES)`。
- トーンマップは積んだエフェクトの最後（FXAA があればその直前）に自動で入る。オーバーレイ（ポーズメニューなど）はトーンマップの後に描かれるので UI の色は変わらない。
- ACES は明るい所をなめらかに白へ寄せる分、全体が少し暗く・締まって見える。暗いと感じたら露出を +0.5〜+1 程度上げる。

## デバッグキー

エディタの **Help > キー操作一覧** に全部載っている（一覧の元は `Editor/KeyBindingsHelp.cpp`。キーを変えたらここも直す）。
デバッグキーは Debug / Development ビルドだけで効き、配布用 Release では効かない。

| キー | 動作 |
|---|---|
| F1 | ゲームカメラ / デバッグカメラの切り替え（再生中） |
| F2 | スクリーンショット |
| F3 / F4 | コライダー / デバッグ描画を全部表示 |
| F10 | 録画（MP4）の開始 / 停止（撮影モード中ならゲーム画面だけが写る） |
| F11 | 撮影モード（ゲーム画面だけを全画面）の開始 / 終了 |
| Ctrl+S | シーンを保存（未保存の Scene Flow も。編集モードのみ） |

## 注意

- `project/Chaso/` の中にゲーム固有のファイルを置かない（push でエンジン側に混ざる）。
- エンジン（`Engine/` `Editor/` `Game/` `Framework/`）から `Application/` のヘッダを include しない。
  ゲームごとに違う部分は、エンジンが用意した登録口へ Application 側から差し込む:

  | 差し込むもの | 登録口 | 例（水天の射手） |
  |---|---|---|
  | ゲームルール（1 プレイの開始・決着判定・レベルの敵に載せるスクリプト） | `GameModeBase` を継承し `GameModeBase::SetFactory` で登録 | `Application/Game/Framework/RailShooterGameMode` |
  | シーン遷移の行き先で使う変数（`$nextStage` など） | `SceneFlow::Get().RegisterVariable` | `GameSetup.cpp` |
  | Dive 遷移で抜ける画面色 | `Scene::SceneManager::SetDiveScreenColor` | `GameSetup.cpp` |
  | エディタのパネル（ゲーム専用のデバッグ UI） | `EditorExtension::AddPanel`（`CHASO_EDITOR_EXTENSION` マクロ） | （未使用） |
  | プリミティブの描画方法（水・水柱） | `PrimitiveMeshComponent::drawStyle` | 弾やしぶきを作るスクリプト |

  登録は `REGISTER_SCRIPT` と同じく静的初期化で行う（エンジン側の保存先は関数内 static なので順序は問わない）。
  何も登録しなければ `GameModeBase` の既定動作（決着判定なし）で動く。
- `Externals/assimp/lib/*.lib` は追跡対象。それ以外の `.lib` と `DirectXTex/Shaders/Compiled/` は無視される。
