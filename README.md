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
| 作ったゲームを GitHub に公開（初回 push） | `.\project\Chaso\scripts\publish-github.ps1`（gh があれば自動作成、無ければ URL を聞く） |
| リソースを手動で同期する（通常はビルド前に自動） | `.\project\Chaso\scripts\sync-resources.ps1` |
| ゲーム中にエンジンを直した → ChasoEngine へ反映 | `.\project\Chaso\scripts\engine-push.ps1` |
| ChasoEngine の更新を取り込む | `.\project\Chaso\scripts\engine-pull.ps1` |

- `engine-push` は `project/Chaso/` 配下に触れたコミットだけを切り出して送る。ゲームのコードは送られない。
- 別のゲームで先にエンジンが更新されていて push が拒否されたら、`engine-pull` → 競合解決 → `engine-push`。
- エンジンだけ直したいときは、どれかのゲームリポジトリの `project/Chaso/` で直して push すればよい。
  ChasoEngine を単体で clone しても Visual Studio では開けない（sln はゲーム側にある）。

VS Code の Project Actions（`.project-actions.json`）にも同じボタンがある:
「エンジン反映」「エンジン取込」「リソース同期」「新規ゲーム」「GitHub公開」。新規ゲームにもこのボタン設定と `scripts/build/` が付いてくる。

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
