# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

DirectX 12 製の自作エンジン（ChasoEngine）と、その上に作られたゲーム（『水天の射手』、レールシューター）のリポジトリ。C++20 / Windows x64 / Visual Studio 2026 (v145)。コメント・ドキュメント・コミットメッセージは日本語。

## ビルド・実行

リポジトリのルートから（PowerShell）:

```powershell
# ビルドして実行（Configuration: Debug / Development / Release）
powershell -ExecutionPolicy Bypass -File scripts/build/build_and_run.ps1 -Configuration Debug
# ビルドのみ / 実行のみ / リビルド
powershell -ExecutionPolicy Bypass -File scripts/build/build_and_run.ps1 -Configuration Debug -BuildOnly
powershell -ExecutionPolicy Bypass -File scripts/build/build_and_run.ps1 -Configuration Debug -RunOnly
powershell -ExecutionPolicy Bypass -File scripts/build/build_and_run.ps1 -Configuration Debug -Rebuild
# 配布パッケージ（generated/package/ に zip）
powershell -ExecutionPolicy Bypass -File scripts/build/package_release.ps1 -Configuration Release
```

MSBuild を直接使う場合（スクリプトと同じ引数。Debug x64 でビルド成功を確認済み）:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" project\chaso.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
```

- MSBuild のパスは環境依存。`vswhere.exe -latest -property installationPath` で VS のインストール先を調べ、`MSBuild\Current\Bin\amd64\MSBuild.exe` を使う（`build_and_run.ps1` の `Find-MSBuild` と同じ方法）。
- 出力先: `generated/outputs/<Configuration>/ChasoApp.exe`（例: `generated\outputs\Debug\ChasoApp.exe`）。`ChasoEngine.lib` などの .lib も同じフォルダ、中間ファイルは `generated/obj/<ProjectName>/<Configuration>/`。
- ソリューションは `project/chaso.sln`。ビルドログは `logs/build/`、実行ログは `logs/app/`。
- ユニットテストは無い。動作確認は `--autotest`（下記）か、ビルド成功＋実行（ログ確認）で行う。CI（`.github/workflows/`）は master への push で Debug / Release ビルドと不要ファイル検査（`*.pdb`, `*.log`, `imgui.ini`, `x64/`, `logs/` など）を行う。
- **シーン巡回の自動テスト**: 作業ディレクトリ `project/` で `..\generated\outputs\Debug\ChasoApp.exe --autotest` を起動すると、Title → Stage1 → Result → Title を自動で遷移し、各シーンのゲーム画面（エディタ UI なし）を `media/autotest/<日時>/NN_<シーン>.png` に保存して終了する。終了コード 0 = 成功 / 1 = 失敗、結果は `logs/app/` の `[AutoTest]` 行。オプション `--autotest-route=A,B,C` / `--autotest-wait=<秒>`（既定 2.0）/ `--autotest-out=<フォルダ>`。既定の順路は `GameSetup.cpp` の `AutoTest::SetDefaultRoute`、実装は `Chaso/Framework/AutoTest.*`。
- 構成: Debug / Development（最適化あり＋エディタ有効、`RC_DEVELOPMENT`）/ Release（エディタ・デバッグキー無効）。エディタ・ImGui 関連コードは `#if RC_ENABLE_IMGUI` で囲む（`Engine/Common/EngineConfig.h`）。
- シェーダ（HLSL, SM6.0+）は実行時に DXC でコンパイルされる。`dxcompiler.dll` / `dxil.dll` は PostBuild で Windows SDK からコピーされる。
- 整形は `.editorconfig`（スペース 2、CRLF、UTF-8）に従う。

## リポジトリ構成の要点

- `project/Chaso/` はエンジン本体で、別リポジトリ **ChasoEngine** の **git subtree**。ゲーム固有のコードを入れてはいけない（エンジンはゲームの型を知らない設計）。ChasoEngine への反映/取込は `project/Chaso/scripts/engine-push.ps1` / `engine-pull.ps1`（ユーザーが明示的に頼んだときだけ実行）。
- `project/Application/` がゲーム側（`ChasoApp.vcxproj`）。ロジックは `Game/Scripts/`（NativeScript）、`Game/Framework/`（GameMode 派生・シーンをまたぐ状態・演出ヘルパ）。
- `project/Chaso/Engine/` は静的ライブラリ `ChasoEngine.vcxproj`。ただし `Framework/App.cpp`、`Editor/*.cpp`、`Game/`（Game / Scene / SceneManager / SceneFlow / Fade / GameModeBase など）の .cpp は **`ChasoApp.vcxproj` 側でコンパイル**されている。新しい .cpp を追加したら、置き場所に応じて正しい vcxproj（と `.filters`）に `ClCompile` を手で追加すること。ヘッダのみなら不要。
- `project/Resources/` が実行時に参照される唯一のリソースフォルダ。エンジン用リソースの原本は `project/Chaso/Resources/` にあり、ChasoApp の PreBuildEvent（`Chaso/scripts/sync-resources.ps1`）で双方向同期される:
  - エンジン由来ファイル（Shader / fonts / icons 等）は `project/Resources/` 側で編集してよい（次のビルドで原本に書き戻される）。
  - エンジンに**新規**ファイルを追加するときは `project/Chaso/Resources/` に置く。`project/Resources/` に置くとゲーム固有扱いになる。削除は同期されないので両方から消す。
  - `project/Resources/.gitignore` は自動生成され、エンジン由来のコピーは追跡されない。
- `Docs/`, `Portfolio/`, `Reference/`, `週間報告/` はコード外の資料。

## アーキテクチャ

`App`（Window / Dx12Core / Input / PipelineManager / PostProcess を所有）→ `Game` → `SceneManager`（State パターンでフェード/ロード管理）→ `DataDrivenScene`（`Resources/Scenes/<name>.json` からエンティティを構築）。起動シーンとウィンドウ設定は `Resources/Setting/AppConfig.json`（`bootScene` は構成別に指定可）。

- **ECS**: `Entity` + `IComponent`。ゲームロジックは `ScriptableEntity` を継承した NativeScript（`OnCreate` / `OnUpdate` / `OnCollision` / 各種 `On*Render` フック）。`REGISTER_SCRIPT(ClassName)` で `ScriptRegistry` に登録し、シーン JSON から名前で生成される。パラメータは `Serialize` / `Deserialize` でシーン JSON に保存し、`OnImGui` で Inspector に出す。README の Quick Start が雛形。
- **シーン遷移**: スクリプトはシーン名を書かず `RequestTransition("trigger"[, arg])` を呼ぶ。行き先と演出（`dissolve` / `dive`）は `Resources/SceneFlow.json` のルール表で決まる（`from` は完全一致 → 長い前方一致 `Stage*` → `*` の順で優先、`to` に `$current` / `$arg` / 登録変数）。
- **ゲーム固有の差し込み口**: `Application/Game/Framework/GameSetup.cpp` が静的初期化で GameMode ファクトリ（`RailShooterGameMode`）、Dive 遷移色、SceneFlow 変数（`$lastPlay`, `$nextStage`）をエンジンへ登録する。エディタ用のゲーム専用パネルは `CHASO_EDITOR_EXTENSION`、エディタ→アプリへの依頼は `DebugBridge`。
- **描画**: `RC::`（RenderCommon）API に描画要求を積み、64bit `SortKey` でソートして発行。フレームごとの定数バッファは `FrameResource`（トリプルバッファのリニアアロケータ）、GPU 使用中リソースの解放は `DeferredReleaseQueue`（フェンス待ち）経由で行う。モデル/テクスチャは非同期ロードで、未ロードの描画はガードされる。
- **フレームの描画順**: 2D 背景 → スポットライト影 → 平行光源影 → マスクパス → メイン 3D → 3D オーバーレイ → 2D → ポストプロセス（ピンポンバッファでマルチパス）→ `OnOverlayRender`（ポストエフェクトを受けない UI）→ ImGui エディタ。
- **オーディオ**: `AudioEngine`（XAudio2 シングルトン）。BGM は keep-alive 方式（次のシーンにも同じ曲があれば継続）、SE は重ね再生。ECS からは `AudioSourceComponent` の名前付きスロット（`Play("shoot")`）を使う。
- リソースパスは `Chaso::ResolvePath()`（`Engine/Common/ResourcePath.h`）で解決する。ログは `Log`（`Log::Fatal` は Release でも停止）。

## コード規約

- 関数・クラスには Doxygen 形式の日本語コメント（`/// @brief`, `/// @param`, `/// @return`, `///<`）を付けるのが既存コードの慣習。
- インクルードはエンジン基準の相対パス（例: `"ECS/ScriptableEntity.h"`, `"Common/SceneContext.h"`）。
