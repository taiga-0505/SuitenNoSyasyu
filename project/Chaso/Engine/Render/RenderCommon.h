#pragma once
#include "GraphicsPipeline/GraphicsPipeline.h"
#include "Font/TextMeshGenerator.h"
#include "Math/Math.h"
#include "Common/SceneContext.h"
#include <d3d12.h>
#include <string>
#include <future>
#include "struct.h"
#include "Model/ModelProxyPool.h"       // ModelProxyHandle（VirtualEntity）
#include "Model/ModelInstanceBatcher.h" // ModelInstanceBatcher::Stats
#include "Effect/EffectParticleSystem.h" // EffectParticleSpawn

// D3D12 GPUハンドルを返すために必要
struct D3D12_GPU_DESCRIPTOR_HANDLE;

// PostEffectType 前方宣言（PostProcess.h で定義）
enum class PostEffectType;

namespace RC {

// ============================================================================
// ライト生成時の「アクティブ登録」モード
// ============================================================================
enum class LightActivateMode {
  None = 0, ///< 生成するだけ（描画には使われない）
  Add,      ///< 現在のアクティブ配列に追加（最大数を超えたら追加失敗）
  Set,      ///< アクティブ配列をクリアしてから追加（= それだけを使う）
};

/// @brief RenderCommon を初期化する（起動時に一度だけ呼ぶ）
/// @param ctx シーンコンテキスト
/// @note App::Init 内など、Device / PipelineManager / SRVManager が有効になった後に呼んでください。
void Init(SceneContext &ctx);

/// @brief RenderCommon を終了する（確保したリソースを解放）
void Term();

/// @brief 今フレームのカメラ情報（View/Proj とカメラ位置）を共有する
/// @param view ビュー行列
/// @param proj プロジェクション行列
/// @param camWorldPos カメラのワールド座標
/// @note View/Proj は各オブジェクト Update に渡され、camWorldPos はシェーダー側で利用できます。
void SetCamera(const Matrix4x4 &view, const Matrix4x4 &proj,
               const RC::Vector3 camWorldPos);

// ==============================
// ライト用
// ==============================

class DirectionalLightSource;

/// @brief ディレクショナルライトを生成してハンドルを返す
/// @param activateMode アクティブ化モード
/// @return ライトハンドル（失敗時は -1）
int CreateDirectionalLight(
    LightActivateMode activateMode = LightActivateMode::Set);

/// @brief ディレクショナルライトを破棄する
/// @param lightHandle ライトハンドル
void DestroyDirectionalLight(int lightHandle);

/// @brief 3D描画で使用する「アクティブなディレクショナルライト」を切り替える
/// @param lightHandle ライトハンドル。-1 の場合は「明示的なアクティブ無し」になり、描画時は共通のデフォルトライトを使用します。
void SetActiveDirectionalLight(int lightHandle);

/// @brief 現在のアクティブなディレクショナルライトのハンドルを返す
/// @return ライトハンドル（未設定なら 0）
int GetActiveDirectionalLightHandle();

/// @brief ディレクショナルライトの実体ポインタを取得する
/// @param lightHandle ライトハンドル
/// @return DirectionalLightSource*（無効ハンドルなら nullptr）
DirectionalLightSource *GetDirectionalLightPtr(int lightHandle);

/// @brief ディレクショナルライトの ON/OFF を切り替える
/// @param lightHandle ライトハンドル
/// @param enabled 有効にするなら true
void SetDirectionalLightEnabled(int lightHandle, bool enabled);

/// @brief ディレクショナルライトが ON かどうかを確認する
/// @param lightHandle ライトハンドル
/// @return 有効なら true
bool IsDirectionalLightEnabled(int lightHandle);

/// @brief 現在の実効アクティブなディレクショナルライトの ON/OFF を切り替える
/// @param enabled 有効にするなら true
void SetActiveDirectionalLightEnabled(bool enabled);

/// @brief 現在の実効アクティブなディレクショナルライトが ON かどうかを確認する
/// @return 有効なら true
bool IsActiveDirectionalLightEnabled();

// ==============================
// ポイントライト用
// ==============================

class PointLightSource;

/// @brief ポイントライトを生成してハンドルを返す
/// @param activateMode アクティブ化モード
/// @return ポイントライトハンドル（失敗時は -1）
int CreatePointLight(LightActivateMode activateMode = LightActivateMode::Add);

/// @brief ポイントライトを破棄する
/// @param pointLightHandle ポイントライトハンドル
void DestroyPointLight(int pointLightHandle);

/// @brief 3D描画で使用する「アクティブポイントライト」を切り替える
/// @param pointLightHandle ポイントライトハンドル。-1 の場合は「明示的なアクティブ無し」になり、描画時はデフォルト（OFF）を使用します。
void SetActivePointLight(int pointLightHandle);

/// @brief 現在のアクティブポイントライトのハンドルを返す
/// @return ポイントライトハンドル（未設定なら -1）
int GetActivePointLightHandle();

/// @brief アクティブポイントライト配列をクリアする
void ClearActivePointLights();

/// @brief アクティブポイントライト配列に追加する
/// @param pointLightHandle 追加するライトハンドル
/// @return 追加に成功したら true
bool AddActivePointLight(int pointLightHandle);

/// @brief アクティブポイントライト配列から削除する
/// @param pointLightHandle 削除するライトハンドル
void RemoveActivePointLight(int pointLightHandle);

/// @brief 現在のアクティブポイントライトの数を取得する
/// @return アクティブなポイントライトの数
int GetActivePointLightCount();

/// @brief アクティブポイントライトのハンドルをインデックスで取得する
/// @param index インデックス
/// @return ポイントライトハンドル
int GetActivePointLightHandleAt(int index);

/// @brief ポイントライトの実体ポインタを取得する
/// @param pointLightHandle ポイントライトハンドル
/// @return PointLightSource*（無効ハンドルなら nullptr）
PointLightSource *GetPointLightPtr(int pointLightHandle);

/// @brief ポイントライトの ON/OFF を切り替える
/// @param pointLightHandle ポイントライトハンドル
/// @param enabled 有効にするなら true
void SetPointLightEnabled(int pointLightHandle, bool enabled);

/// @brief ポイントライトが ON かどうかを確認する
/// @param pointLightHandle ポイントライトハンドル
/// @return 有効なら true
bool IsPointLightEnabled(int pointLightHandle);

/// @brief 現在の実効アクティブなポイントライト（先頭）の ON/OFF を切り替える
/// @param enabled 有効にするなら true
void SetActivePointLightEnabled(bool enabled);

/// @brief 現在の実効アクティブなポイントライトが ON かどうかを確認する
/// @return 有効なら true
bool IsActivePointLightEnabled();

// ==============================
// スポットライト用
// ==============================

class SpotLightSource;

/// @brief スポットライトを生成してハンドルを返す
/// @param activateMode アクティブ化モード
/// @return スポットライトハンドル（失敗時は -1）
int CreateSpotLight(LightActivateMode activateMode = LightActivateMode::Add);

/// @brief スポットライトを破棄する
/// @param spotLightHandle スポットライトハンドル
void DestroySpotLight(int spotLightHandle);

/// @brief 3D描画で使用する「アクティブスポットライト」を切り替える
/// @param spotLightHandle スポットライトハンドル。-1 の場合は「明示的なアクティブ無し」になり、描画時はデフォルト（OFF）を使用します。
void SetActiveSpotLight(int spotLightHandle);

/// @brief 現在のアクティブスポットライトのハンドルを返す
/// @return スポットライトハンドル（未設定なら -1）
int GetActiveSpotLightHandle();

/// @brief アクティブスポットライト配列をクリアする
void ClearActiveSpotLights();

/// @brief アクティブスポットライト配列に追加する
/// @param spotLightHandle 追加するライトハンドル
/// @return 追加に成功したら true
bool AddActiveSpotLight(int spotLightHandle);

/// @brief アクティブスポットライト配列から削除する
/// @param spotLightHandle 削除するライトハンドル
void RemoveActiveSpotLight(int spotLightHandle);

/// @brief 現在のアクティブスポットライトの数を取得する
/// @return アクティブなスポットライトの数
int GetActiveSpotLightCount();

/// @brief アクティブスポットライトのハンドルをインデックスで取得する
/// @param index インデックス
/// @return スポットライトハンドル
int GetActiveSpotLightHandleAt(int index);

/// @brief スポットライトの実体ポインタを取得する
/// @param spotLightHandle スポットライトハンドル
/// @return SpotLightSource*（無効ハンドルなら nullptr）
SpotLightSource *GetSpotLightPtr(int spotLightHandle);

/// @brief スポットライトの ON/OFF を切り替える
/// @param spotLightHandle スポットライトハンドル
/// @param enabled 有効にするなら true
void SetSpotLightEnabled(int spotLightHandle, bool enabled);

/// @brief スポットライトが ON かどうかを確認する
/// @param spotLightHandle スポットライトハンドル
/// @return 有効なら true
bool IsSpotLightEnabled(int spotLightHandle);

/// @brief 現在の実効アクティブなスポットライト（先頭）の ON/OFF を切り替える
/// @param enabled 有効にするなら true
void SetActiveSpotLightEnabled(bool enabled);

/// @brief 現在の実効アクティブなスポットライトが ON かどうかを確認する
/// @return 有効なら true
bool IsActiveSpotLightEnabled();



// ============================================================================
// Shadow Pass
// ============================================================================

/// @brief シャドウマップのパラメータを更新する
void UpdateShadowParams(const ShadowParams& params);

/// @brief シャドウパスを開始する（深度バッファ・ビューポートの設定等）
void BeginShadowPass();

/// @brief シャドウパスを終了する（SRVへの遷移等）
void EndShadowPass();

// ----------------------------------------------------------------------------
// Mask Pass（特定のオブジェクトだけ輪郭を強調するためのシルエット書き込み）
// ----------------------------------------------------------------------------
// 手順（PreDraw3D の後、メイン3Dの Draw を積む前。シャドウパスの直後が定位置）:
//   BeginMaskPass();
//   強調したい物だけ Draw（DrawModel 等をそのまま呼べる。PSO は自動で mask 系へ）
//   Execute3DCommands();
//   EndMaskPass();
//
// 書かれたマスクは PostProcessType::MaskOutline が t1 で受け取り、
// 膨張させてシルエットの外側だけを塗る。

/// @brief マスクパスを開始する（マスクRTを黒でクリアし、描画先に設定する）
/// @return 開始できたか。false なら描画も EndMaskPass も行わないこと
/// @note Execute3DCommands は「その時点までに積まれた全ての3Dコマンド」を流すため、
///       マスクパスの中に他の描画を混ぜないこと。
bool BeginMaskPass();

/// @brief マスクパスを終了する（SRVへ遷移し、メイン描画の描画先へ戻す）
void EndMaskPass();

/// @brief マスクRTのSRV（GPUハンドル）を取得する。今フレームまだ描いていなければ ptr == 0
D3D12_GPU_DESCRIPTOR_HANDLE GetMaskSRVGPU();

// ----------------------------------------------------------------------------
// Spot Light Shadow（スポットライトごとの影。壁を挟んだ向こう側へ光が漏れなくなる）
// ----------------------------------------------------------------------------
// 手順:
//   1. PreDraw3D の【前】に、影を落とすスポットライトを選んで SetSpotLightShadowIndex(handle, i) で
//      タイル番号を振る（影なしのライトは -1）。PreDraw3D がスポットライト定数バッファを
//      GPU へ転送するため、それより後に振ると 1 フレーム古い割り当てが使われてしまう。
//      同時に SpotShadowCB の entries[i] へライト視点の ViewProjection 等を用意しておく
//   2. PreDraw3D
//   3. UpdateSpotShadowParams(cb)（今フレーム用の CB は PreDraw3D で確保されるため、必ずその後）
//   4. BeginSpotShadowAtlas(); for (i) { BeginSpotShadowTile(i); 影キャスター描画; Execute3DCommands(); EndSpotShadowTile(); } EndSpotShadowAtlas();

/// @brief スポットライトに影アトラスのタイル番号を割り当てる
/// @param spotLightHandle スポットライトハンドル
/// @param shadowIndex タイル番号 (0 〜 kMaxSpotShadows-1)。-1 で影なし
void SetSpotLightShadowIndex(int spotLightHandle, int shadowIndex);

/// @brief スポットライト影の定数バッファ(b7)を更新する（PreDraw3D の後に呼ぶ）
void UpdateSpotShadowParams(const SpotShadowCB &params);

/// @brief スポット影アトラスへの描画を開始する（全面クリア）
/// @return 開始できたら true。false のときはタイル描画を行わずに抜けること
///         （false のまま描くと通常のバックバッファへ影キャスターを描いてしまう）
bool BeginSpotShadowAtlas();

/// @brief アトラス内の 1 タイル（1 灯分）への深度描画を開始する
/// @param tileIndex タイル番号 (0 〜 kMaxSpotShadows-1)
void BeginSpotShadowTile(int tileIndex);

/// @brief タイルへの描画を終了する
void EndSpotShadowTile();

/// @brief アトラスへの描画を終了し、通常描画へ戻す（SRVへの遷移等）
void EndSpotShadowAtlas();

/// @brief 蓄積された3D描画コマンドを即時実行する
void Execute3DCommands();

/// @brief 描画フレームの開始（1 フレームに 1 回。App がコマンドリストを開いた直後に呼ぶ）
/// @details フレームごとの一時バッファ（FrameResource）を次の 1 枚へ切り替える。
///          CPU と GPU を並行させるために必要で、呼び忘れると前フレームの描画中の領域を上書きする。
void BeginFrame();

// ============================================================================
// 3D Pass
// ============================================================================

class AreaLightSource;

/// @brief エリアライト（矩形）を生成してハンドルを返す
/// @param activateMode アクティブ化モード
/// @return エリアライトハンドル（失敗時は -1）
int CreateAreaLight(LightActivateMode activateMode = LightActivateMode::Add);

/// @brief エリアライト（矩形）を破棄する
/// @param areaLightHandle エリアライトハンドル
void DestroyAreaLight(int areaLightHandle);

/// @brief エリアライトを1個だけ有効化する（既存のアクティブをクリアして追加）
/// @param areaLightHandle エリアライトハンドル
void SetActiveAreaLight(int areaLightHandle);

/// @brief 現在のアクティブエリアライトのハンドルを返す
/// @return エリアライトハンドル
int GetActiveAreaLightHandle();

/// @brief アクティブエリアライト配列をクリアする
void ClearActiveAreaLights();

/// @brief アクティブエリアライト配列に追加する
/// @param areaLightHandle 追加するライトハンドル
/// @return 追加に成功したら true
bool AddActiveAreaLight(int areaLightHandle);

/// @brief アクティブエリアライト配列から削除する
/// @param areaLightHandle 削除するライトハンドル
void RemoveActiveAreaLight(int areaLightHandle);

/// @brief 現在のアクティブエリアライトの数を取得する
/// @return アクティブなエリアライトの数
int GetActiveAreaLightCount();

/// @brief アクティブエリアライトのハンドルをインデックスで取得する
/// @param index インデックス
/// @return エリアライトハンドル
int GetActiveAreaLightHandleAt(int index);

/// @brief エリアライトの実体ポインタを取得する
/// @param areaLightHandle エリアライトハンドル
/// @return AreaLightSource*
AreaLightSource *GetAreaLightPtr(int areaLightHandle);

/// @brief エリアライトの ON/OFF を切り替える
/// @param areaLightHandle エリアライトハンドル
/// @param enabled 有効にするなら true
void SetAreaLightEnabled(int areaLightHandle, bool enabled);

/// @brief エリアライトが ON かどうかを確認する
/// @param areaLightHandle エリアライトハンドル
/// @return 有効なら true
bool IsAreaLightEnabled(int areaLightHandle);

/// @brief 現在の実効アクティブなエリアライトの ON/OFF を切り替える
/// @param enabled 有効にするなら true
void SetActiveAreaLightEnabled(bool enabled);

/// @brief 現在の実効アクティブなエリアライトが ON かどうかを確認する
/// @return 有効なら true
bool IsActiveAreaLightEnabled();

// ── モデル用 ──────────────────────────────────────

/// @brief 3D描画の前処理（このフレームの SceneContext / CommandList を登録）
/// @param ctx SceneContext
/// @param cl このフレームで使うコマンドリスト
/// @note この関数を呼んだ後に DrawModel / DrawSphere などを呼んでください。
void PreDraw3D(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

/// @brief モデルをロードしてハンドルを返す（Mesh は内部キャッシュで共有）
/// @param path .obj へのパス
/// @return モデルハンドル（失敗時は -1）
int LoadModel(const std::string &path);

/// @brief モデルリソースの非同期ロードが完了し描画可能か確認する
/// @param modelHandle モデルハンドル
/// @return 準備完了なら true
bool IsModelReady(int modelHandle);

/// @brief モデルを描画する（テクスチャ指定版）
/// @param modelHandle モデルハンドル
/// @param texHandle テクスチャハンドル（RC::LoadTex で取得）
void DrawModel(int modelHandle, int texHandle);

/// @brief モデルを描画する（モデルが参照する mtl のテクスチャを使用）
/// @param modelHandle モデルハンドル
void DrawModel(int modelHandle);

/// @brief モデルをインスタンシング（複数Transform + 単一色）で描画する
/// @param modelHandle モデルハンドル
/// @param instances インスタンスTransform配列
/// @param color 全インスタンスに適用する乗算色
/// @param texHandle テクスチャハンドル（-1 なら mtl のテクスチャ）
void DrawModelBatchColored(int modelHandle,
                           const std::vector<Transform> &instances,
                           const Vector4 &color,
                           int texHandle = -1);

// ── インスタンス描画（自動バッチ＋視錐台カリング） ──────────────────
//
// DrawModel の代わりに使うと、同じメッシュ・同じ見た目（テクスチャ・ライティング設定等）の
// モデルを自動でまとめて 1 回のインスタンス描画にする。描画パス（メイン・影・スポット影タイル）
// ごとに、そのパスの視錐台の外にある物は描かない。
// 色（SetModelColor）はインスタンスごとに持てるので、色違いでも同じバッチにまとまる。
//
// 半透明（色の α < 1）・スキニング・ブレンドモード指定中は自動で従来の DrawModel に回る。

/// @brief モデルをインスタンス描画の候補として積む（DrawModel と同じ使い方）
/// @param modelHandle モデルハンドル
/// @param texHandle テクスチャハンドル（-1 なら mtl のテクスチャ）
void DrawModelInstanced(int modelHandle, int texHandle = -1);

/// @brief 前フレームのインスタンス描画の統計（受付数・カリング数・バッチ数など）
ModelInstanceBatcher::Stats GetModelInstancingStats();

// ── VirtualEntity（ModelProxyPool） ─────────────────────────────
//
// Entity を作らずに「見た目だけの物」を大量に置くための軽量ハンドル。
// 例: 1 万本の木や岩。テンプレート（RC::LoadModel のハンドル）を 1 つ用意し、
//     CreateModelProxy で好きなだけ置く。行列が変わった物だけ境界箱・BVH を更新するので、
//     動かない物は毎フレームの CPU コストがほぼゼロ。
// 描画は各パスで DrawModelProxies() を 1 回呼ぶだけ（BVH で視錐台カリング → インスタンス描画）。
// 制約: 不透明・非スキニングのテンプレートのみ。テンプレートを UnloadModel すると描かれなくなる。

/// @brief プロキシを作る
/// @param modelHandle 見た目のテンプレート（RC::LoadModel のハンドル）
ModelProxyHandle CreateModelProxy(int modelHandle);
/// @brief プロキシを破棄する
void DestroyModelProxy(ModelProxyHandle h);
/// @brief プロキシが生きているか
bool IsModelProxyAlive(ModelProxyHandle h);
/// @brief プロキシの Transform（SRT）を設定する
void SetModelProxyTransform(ModelProxyHandle h, const Transform &t);
/// @brief プロキシのワールド行列を直接設定する
void SetModelProxyWorld(ModelProxyHandle h, const Matrix4x4 &world);
/// @brief プロキシの乗算カラー（テンプレートのマテリアル色にさらに掛かる）
void SetModelProxyColor(ModelProxyHandle h, const Vector4 &color);
/// @brief プロキシの表示／非表示
void SetModelProxyVisible(ModelProxyHandle h, bool visible);
/// @brief プロキシが影を落とすか
void SetModelProxyCastShadow(ModelProxyHandle h, bool cast);
/// @brief 生存しているプロキシ数
uint32_t GetModelProxyCount();
/// @brief 全プロキシを破棄する（シーン切り替え時など）
void ClearModelProxies();
/// @brief 現在の描画パスに全プロキシを描く（カリング・バッチ化は Execute 時に自動）
/// @details メイン 3D・平行光源の影・スポット影タイルの各パスで 1 回ずつ呼ぶ。
///          影パスでは SetModelProxyCastShadow(false) の物は描かれない。
void DrawModelProxies();

// ── エフェクト粒（水しぶき・泡・水柱） ─────────────────────────────
//
// Entity を作らずに、短命の粒を大量に出すための API。
// 粒は種類ごとに共有メッシュでまとめて描かれるので、数が増えても描画コマンドは増えない。
// 使い方:
//   RC::SpawnEffectParticle({RC::EffectParticleKind::Splash, pos, 0.2f, color, impact});
//   毎フレーム RC::UpdateEffectParticles(dt) と、各描画パスで RC::DrawEffectParticles()
//   （DataDrivenScene が行う）

/// @brief エフェクト粒を 1 個出す
void SpawnEffectParticle(const EffectParticleSpawn &desc);
/// @brief 全エフェクト粒を進める（dt = 0 なら止まったまま）
void UpdateEffectParticles(float dt);
/// @brief 現在の描画パスにエフェクト粒を描く（カリング・まとめ描画は Execute 時に自動）
void DrawEffectParticles();
/// @brief 全エフェクト粒を消す（シーン切り替え時など）
void ClearEffectParticles();
/// @brief 生きているエフェクト粒の数
uint32_t GetEffectParticleCount();
/// @brief 前フレームのエフェクト粒の統計
EffectParticleSystem::Stats GetEffectParticleStats();

/// @brief モデルを解放する（ハンドルは無効化される）
/// @param modelHandle モデルハンドル
void UnloadModel(int modelHandle);

/// @brief モデルの Transform ポインタを取得する
/// @param modelHandle モデルハンドル
/// @return Transform*（無効ハンドルなら nullptr）
Transform *GetModelTransformPtr(int modelHandle);

/// @brief モデルの色（乗算カラー）を設定する
/// @param modelHandle モデルハンドル
/// @param color 色（RGBA）
void SetModelColor(int modelHandle, const Vector4 &color);

/// @brief モデルの光沢度（Shininess）を設定する
/// @param modelHandle モデルハンドル
/// @param shininess 光沢度 (0.0: 鏡面反射なし)
void SetModelShininess(int modelHandle, float shininess);

/// @brief アニメーションをアタッチする（モデルに紐づけられたファイルパスを使用）
/// @param modelHandle モデルハンドル
void AttachModelAnimation(int modelHandle);

/// @brief アニメーションを別ファイルからアタッチする
/// @param modelHandle モデルハンドル
/// @param filePath アニメーションファイルのパス
void AttachModelAnimation(int modelHandle, const std::string& filePath);

/// @brief アニメーションを別ファイルからインデックス指定でアタッチする
/// @param modelHandle モデルハンドル
/// @param filePath アニメーションファイルのパス
/// @param animIndex アニメーションインデックス（0始まり）
void AttachModelAnimation(int modelHandle, const std::string& filePath, int animIndex);

/// @brief 別のファイルからロードしたアニメーションへクロスフェードで滑らかに遷移させる
/// @param modelHandle モデルハンドル
/// @param filePath 新しく再生するアニメーションファイルのパス
/// @param blendDuration 切り替えに要するブレンド時間（秒）（デフォルト0.2秒）
void CrossfadeModelAnimation(int modelHandle, const std::string& filePath, float blendDuration = 0.2f);

/// @brief 別のファイルからインデックス指定でアニメーションをロードし、クロスフェードで滑らかに遷移させる
/// @param modelHandle モデルハンドル
/// @param filePath アニメーションファイルのパス
/// @param animIndex アニメーションインデックス（0始まり）
/// @param blendDuration 切り替えに要するブレンド時間（秒）（デフォルト0.2秒）
void CrossfadeModelAnimation(int modelHandle, const std::string& filePath, int animIndex, float blendDuration = 0.2f);

/// @brief モデルのアニメーション状態を更新する
/// @param modelHandle モデルハンドル
/// @param dt 経過時間 (負の値ならエンジンの deltaTime を自動使用)
void UpdateModelAnimation(int modelHandle, float dt = -1.0f);

/// @brief モデルにアタッチされているアニメーションの再生時間（秒）を取得する
/// @param modelHandle モデルハンドル
/// @return 再生時間（秒）
float GetModelAnimationDuration(int modelHandle);

/// @brief モデルのスケルトンをデバッグ描画する（Joint球 + Bone線）
/// @param modelHandle モデルハンドル
void DrawModelSkeleton(int modelHandle);

/// @brief モデルにスキンデータ（ボーン/スケルトン）が含まれているかを返す
/// @param modelHandle モデルハンドル
/// @return スキンデータが存在すれば true
bool HasModelSkinData(int modelHandle);

/// @brief モデルにスケルトン構造が含まれているかを返す（ボーンウェイト無しの階層アニメーションも含む）
/// @param modelHandle モデルハンドル
/// @return スケルトン構造が存在すれば true
bool HasModelSkeleton(int modelHandle);

// ============================================================
// ボーン追従（ソケット）
// ============================================================

/// @brief 指定Jointの現在の姿勢（スケルトン空間＝モデルローカル）を取得する
/// @param modelHandle モデルハンドル
/// @param jointName Joint名（例: "R_Hand"）
/// @param out 取得先の行列
/// @return 見つかれば true
/// @details ワールド姿勢は Multiply(out, モデルのワールド行列) で求まる。
///          武器などをボーンに追従させたい場合に使う。
bool GetModelJointMatrix(int modelHandle, const std::string &jointName,
                         Matrix4x4 &out);

/// @brief モデルの全Joint名を取得する（ボーン名を調べる用）
/// @param modelHandle モデルハンドル
/// @return Joint名の一覧。スケルトンが無ければ空
std::vector<std::string> GetModelJointNames(int modelHandle);

/// @brief 描画に使うワールド行列を直接指定する（Transform の TRS を無視する）
/// @param modelHandle モデルハンドル
/// @param world ワールド行列
/// @details ボーン追従のように行列でしか表せない姿勢を与えるための機能。
///          解除するには ClearModelWorldOverride を呼ぶ。
void SetModelWorldOverride(int modelHandle, const Matrix4x4 &world);

/// @brief ワールド行列の上書きを解除し、Transform 基準の描画に戻す
/// @param modelHandle モデルハンドル
void ClearModelWorldOverride(int modelHandle);

/// @brief モデルのライティングモードを設定する
/// @param modelHandle モデルハンドル
/// @param m ライティングモード
/// @note 設定以降、このモデルは DirectionalLight の LightingMode に追従しない。
///       追従に戻すには ClearModelLightingModeOverride() を呼ぶ。
void SetModelLightingMode(int modelHandle, LightingMode m);

/// @brief ライティングモードの個別設定を解除し、DirectionalLight に追従させる
/// @param modelHandle モデルハンドル
void ClearModelLightingModeOverride(int modelHandle);

/// @brief モデルが参照する Mesh を差し替える（内部キャッシュで共有）
/// @param modelHandle モデルハンドル
/// @param path .obj へのパス
void SetModelMesh(int modelHandle, const std::string &path);

/// @brief DrawModelBatchColored の内部カーソルをリセットする
/// @param modelHandle モデルハンドル
void ResetCursor(int modelHandle);

// ── ガラス（2パス：背面→表面） ──────────────────
// ※箱/ブロックみたいな「厚み」を出したい時に使う

/// @brief ガラス（2パス）を描画する
/// @param modelHandle モデルハンドル
/// @param texHandle テクスチャハンドル（-1 なら mtl のテクスチャ）
void DrawModelGlassTwoPass(int modelHandle, int texHandle = -1);

// ── 2D用 ──────────────────────────────────────────

/// @brief 2D描画の前処理（このフレームの SceneContext / CommandList を登録）
/// @param ctx SceneContext
/// @param cl このフレームで使うコマンドリスト
/// @note この関数を呼んだ後に DrawSprite / DrawLine などを呼んでください。
void PreDraw2D(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

/// @brief ポストプロセス後の 2D オーバーレイ描画を再開する
/// @param ctx SceneContext
/// @param cl このフレームで使うコマンドリスト
/// @details ポストプロセスの最終出力（画面）の上に、輪郭・水中・ビネットなどの効果を受けない
///          UI（ポーズメニュー等）を描くためのもの。App がポストプロセスの直後に
///          Scene::RenderOverlay を呼び、その中で使う。スクリプトは OnOverlayRender で描く。
/// @note PreDraw2D と違い BeginFrame を行わないので、同じフレームの PreDraw2D の後に呼ぶこと。
void ResumeDraw2D(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

/// @brief 2D 描画のあとに積んだ 3D コマンドをその場で実行する（UI より手前に出すオーバーレイ用）
/// @details 通常 3D コマンドは PreDraw2D で一括実行されるため、そのあとに DrawModel を
///          呼んでも今フレームには出ない。この関数を呼ぶとその場で実行され、
///          HUD などの 2D より手前に 3D モデルを重ねられる。
/// @note PreDraw2D（＝2D 描画）が終わったあとに呼ぶこと。実行後は 2D 用のステートへ戻る。
void ExecuteOverlay3D();

/// @brief 背景2D描画の前処理（モデルより後ろに描くスプライト用）
/// @param ctx SceneContext
/// @param cl このフレームで使うコマンドリスト
/// @note PreDraw3D を呼ぶ前に呼び、直後に DrawSprite を並べてください。
///       このパスで積んだスプライトは 3D モデルより奥に表示されます。
/// @warning Sprite 専用です。DrawString / DrawLine などは PreDraw2D 後に呼んでください。
void PreDraw2DBackground(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

/// @brief スプライトをワールド空間（3D）モードに切り替える
/// @param spriteHandle スプライトハンドル
/// @param enable true でワールド空間モード
/// @details ON にすると Transform の translation / rotation / scale が
///          ワールド座標として扱われます（scale がそのままクアッドの大きさ）。
///          描画には DrawSprite3D を使ってください。
void SetSpriteWorldSpace(int spriteHandle, bool enable);

/// @brief ワールド空間スプライトを 3D パスに積む（深度テストあり）
/// @param spriteHandle スプライトハンドル
/// @note PreDraw3D ～ PreDraw2D の間で呼んでください。
///       深度テスト・深度書き込みが有効なので、モデルとの前後関係が
///       自動で解決され、任意のモデルとモデルの間に挟み込めます。
void DrawSprite3D(int spriteHandle);

/// @brief スプライト用テクスチャをロードし、Sprite2D を生成してハンドルを返す
/// @param path 画像パス
/// @param ctx SceneContext（画面サイズ取得に使用）
/// @param srgb sRGBとして読み込むか
/// @return スプライトハンドル（失敗時は -1）
int LoadSprite(const std::string &path, SceneContext &ctx, bool srgb = true);

/// @brief スプライトを描画する
/// @param spriteHandle スプライトハンドル
/// @warning 1 つのハンドルは 1 フレームにつき 1 回しか描画できません。
///          位置・色・UV はハンドルごとに定数バッファ 1 つで保持しており、
///          Draw 系はコマンドリストへ記録するだけで GPU が読むのはフレーム終端です。
///          同じハンドルを位置を変えながら複数回描くと、実行時にはすべて最後に
///          書いた状態で描かれます。同時に複数箇所へ出したい場合は、
///          出したい個数だけ LoadSprite でハンドルを作ってください。
/// @note UV は identity（テクスチャ全体）に戻してから描画します。
void DrawSprite(int spriteHandle);

/// @brief スプライトを「テクスチャ内の矩形」を指定して描画する（スプライトシート用）
/// @param spriteHandle スプライトハンドル
/// @param srcX 切り出し矩形 左上X（ピクセル）
/// @param srcY 切り出し矩形 左上Y（ピクセル）
/// @param srcW 切り出し幅（ピクセル）
/// @param srcH 切り出し高さ（ピクセル）
/// @param texW テクスチャ全体の幅（ピクセル）
/// @param texH テクスチャ全体の高さ（ピクセル）
/// @param insetPx にじみ対策の内側オフセット（ピクセル）
/// @note 位置/回転/サイズは SetSpriteTransform で設定した値を使用します。
/// @note 切り出し矩形はハンドルに残ります（DrawSprite を呼ぶと全体表示へ戻ります）。
/// @warning DrawSprite と同じく、1 ハンドルにつき 1 フレーム 1 回までです。
///          スプライトシートから複数の絵柄を同時に出す場合は、
///          描く個数だけ同じ画像のハンドルを作ってください。
void DrawSpriteRect(int spriteHandle, float srcX, float srcY, float srcW,
                    float srcH, float texW, float texH, float insetPx = 0.0f);

/// @brief スプライトを UV(0..1) の矩形を指定して描画する
/// @param spriteHandle スプライトハンドル
/// @param u0 開始 U
/// @param v0 開始 V
/// @param u1 終了 U
/// @param v1 終了 V
/// @warning 制約は DrawSpriteRect と同じです（1 ハンドル 1 フレーム 1 回）。
void DrawSpriteRectUV(int spriteHandle, float u0, float v0, float u1, float v1);

/// @brief スプライトの Transform を設定する
/// @param spriteHandle スプライトハンドル
/// @param t Transform
void SetSpriteTransform(int spriteHandle, const Transform &t);

/// @brief スプライトの色（乗算カラー）を設定する
/// @param spriteHandle スプライトハンドル
/// @param color 色（RGBA）
void SetSpriteColor(int spriteHandle, const Vector4 &color);

/// @brief スプライトを解放する
/// @param spriteHandle スプライトハンドル
void UnloadSprite(int spriteHandle);

/// @brief スプライトのサイズを設定する
/// @param spriteHandle スプライトハンドル
/// @param w 幅（ピクセル）
/// @param h 高さ（ピクセル）
void SetSpriteScreenSize(int spriteHandle, float w, float h);

// ── 文字描画用 (Font) ─────────────────────────────

/// @brief フォントファイルをロードしてハンドルを返す
/// @param path フォントファイル（.ttf / .otf / .ttc）のパス
/// @param sizePx フォントサイズ（ピクセル。em 高さ）
/// @param atlasSize グリフアトラスの一辺（512 の倍数。日本語を多く出すなら 2048 推奨）
/// @return フォントハンドル（失敗時は -1）
/// @note DirectWrite でラスタライズするため ImGui には依存しません（Release でも使えます）。
/// @note 1 ハンドル = 1 ファイル × 1 サイズ。同じ組み合わせを再ロードすると同じハンドルが返ります。
///       別サイズが必要なら別ハンドルを作るか、DrawString の scale で拡縮してください。
int LoadFont(const std::string &path, float sizePx, uint32_t atlasSize = 1024);

/// @brief フォントを解放する
/// @param fontHandle フォントハンドル
/// @note シーン終了時など、GPU が参照を終えたタイミングで呼んでください。
void UnloadFont(int fontHandle);

/// @brief 文字列を描画する（UTF-8）
/// @param fontHandle フォントハンドル
/// @param utf8 描画する文字列（UTF-8。'\n' で改行）。ソースは /utf-8 でビルドされるので "日本語" と書けます
/// @param pos 基準位置（ピクセル、左上原点）。y は 1 行目の上端
/// @param color 色（RGBA）
/// @param scale 拡大率（1.0 でロード時サイズ。大きく拡大するとぼやけます）
/// @param align 水平揃え（Left: pos.x が左端 / Center: 中央 / Right: 右端）
/// @param lineSpacing 行送り倍率（1.0 でフォント既定の行高）
/// @note Sprite と異なり、同じハンドルを 1 フレームに何度でも位置・色を変えて描けます。
/// @note 初めて出す文字はその場でアトラスへ登録・転送されます（以降はキャッシュ）。
void DrawString(int fontHandle, const std::string &utf8, const Vector2 &pos,
                const Vector4 &color = {1.0f, 1.0f, 1.0f, 1.0f},
                float scale = 1.0f, TextAlign align = TextAlign::Left,
                float lineSpacing = 1.0f);

/// @brief 文字列を描画する（ワイド文字列版。L"..." リテラルをそのまま渡せます）
void DrawString(int fontHandle, const std::wstring &text, const Vector2 &pos,
                const Vector4 &color = {1.0f, 1.0f, 1.0f, 1.0f},
                float scale = 1.0f, TextAlign align = TextAlign::Left,
                float lineSpacing = 1.0f);

/// @brief 文字列の描画サイズを計測する（UTF-8）
/// @return x = 最長行の幅、y = 行数 × 行送り（ピクセル）
Vector2 MeasureString(int fontHandle, const std::string &utf8,
                      float scale = 1.0f, float lineSpacing = 1.0f);

/// @brief 文字列の描画サイズを計測する（ワイド文字列版）
Vector2 MeasureString(int fontHandle, const std::wstring &text,
                      float scale = 1.0f, float lineSpacing = 1.0f);

/// @brief フォントの行送り量（ピクセル）を取得する
float GetFontLineHeight(int fontHandle, float scale = 1.0f);

// ── 天球用 (Skydome) ──────────────────────────────

/// @brief 天球を生成する（最小形：半径100.0, 32x32）
/// @param textureHandle テクスチャハンドル（必須）
/// @return 天球ハンドル（失敗時は -1）
int GenerateSkydome(int textureHandle);

/// @brief 天球を生成する（パラメータ指定版）
/// @param textureHandle テクスチャハンドル（-1 なら未設定）
/// @param radius 半径
/// @param sliceCount スライス数
/// @param stackCount スタック数
/// @return 天球ハンドル（失敗時は -1）
int GenerateSkydomeEx(int textureHandle = -1, float radius = 100.0f,
                      unsigned int sliceCount = 32, unsigned int stackCount = 32);

/// @brief 天球を描画する
/// @details Skybox と同じく「無限遠の背景」として描く。深度は常に最遠（書き込み無し）なので
/// カメラの Far クリップや天球の半径に影響されず、カメラ位置に追従してカメラ中心に描かれる
/// （Transform.translation はカメラからのオフセット。rotation / scale はそのまま有効）。
/// @param skydomeHandle 天球ハンドル
/// @param texHandle 一時的に差し替えるテクスチャ（-1 なら生成時のテクスチャ。無ければ white1x1）
void DrawSkydome(int skydomeHandle, int texHandle = -1);

/// @brief 天球を解放する
/// @param skydomeHandle 天球ハンドル
void UnloadSkydome(int skydomeHandle);

/// @brief 天球の Transform ポインタを取得する
/// @param skydomeHandle 天球ハンドル
/// @return Transform*（無効ハンドルなら nullptr）
Transform *GetSkydomeTransformPtr(int skydomeHandle);

/// @brief 天球の色（乗算カラー）を設定する
/// @param skydomeHandle 天球ハンドル
/// @param color 色（RGBA）
void SetSkydomeColor(int skydomeHandle, const Vector4 &color);


// ── スカイボックス用 (Skybox) ────────────────────────

/// @brief DDSキューブマップからスカイボックスを生成する
/// @param ddsPath DDSファイルのパス（cubemap形式）
/// @return スカイボックスハンドル（失敗時は -1）
int CreateSkyBox(const std::string &ddsPath);

/// @brief スカイボックスを描画する（3Dパス内で使用）
/// @param skyboxHandle スカイボックスハンドル
void DrawSkyBox(int skyboxHandle);

/// @brief スカイボックスを解放する
/// @param skyboxHandle スカイボックスハンドル
void UnloadSkyBox(int skyboxHandle);

/// @brief スカイボックスの Transform ポインタを取得する
/// @param skyboxHandle スカイボックスハンドル
/// @return Transform*（無効ハンドルなら nullptr）
Transform *GetSkyBoxTransformPtr(int skyboxHandle);

/// @brief スカイボックスの色（乗算カラー）を設定する
/// @param skyboxHandle スカイボックスハンドル
/// @param color 色（RGBA）
void SetSkyBoxColor(int skyboxHandle, const Vector4 &color);


// ── 環境マップ（Environment Map） ────────────────────

/// @brief Skybox の Cubemap を環境マップとして登録する
/// @param skyboxHandle スカイボックスハンドル
/// @note DrawSkyBox() を呼んだ場合は自動的に登録されます。
void SetEnvironmentMap(int skyboxHandle);

/// @brief モデルの環境マップ映り込み係数を設定する
/// @param modelHandle モデルハンドル
/// @param coeff 映り込み係数（0=映り込みなし、1=完全鏡面）
void SetModelEnvironmentCoefficient(int modelHandle, float coeff);

/// @brief モデルの法線マップを設定する
/// @param modelHandle モデルハンドル
/// @param texHandle 法線マップのテクスチャハンドル
void SetModelNormalMap(int modelHandle, int texHandle);

/// @brief モデルのラフネスマップを設定する
/// @param modelHandle モデルハンドル
/// @param texHandle ラフネスマップのテクスチャハンドル
void SetModelRoughnessMap(int modelHandle, int texHandle);

/// @brief モデルのマテリアルポインタを取得する（直接編集用）
/// @param modelHandle モデルハンドル
/// @return Material*（無効ハンドルなら nullptr）
Material *GetModelMaterialPtr(int modelHandle);

// ── 汎用プロシージャルメッシュ（PrimitiveMesh） ──────────

/// @brief 平面メッシュを生成
/// @param width 幅
/// @param height 高さ
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GeneratePlane(float width = 1.0f, float height = 1.0f, int texHandle = -1);

/// @brief ボックスメッシュを生成
/// @param width 幅
/// @param height 高さ
/// @param depth 奥行き
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateBox(float width = 1.0f, float height = 1.0f, float depth = 1.0f, int texHandle = -1);

/// @brief 球体メッシュを生成（外向き）
/// @param radius 半径
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateSphere(float radius = 0.5f, int texHandle = -1);

/// @brief 円柱メッシュを生成
/// @param radius 半径
/// @param height 高さ
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateCylinder(float radius = 0.5f, float height = 1.0f, int texHandle = -1);

/// @brief 円錐メッシュを生成
/// @param radius 半径
/// @param height 高さ
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateCone(float radius = 0.5f, float height = 1.0f, int texHandle = -1);

/// @brief トーラスメッシュを生成
/// @param majorRadius 大半径
/// @param minorRadius 小半径
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateTorus(float majorRadius = 1.0f, float minorRadius = 0.2f, int texHandle = -1);

/// @brief カプセルメッシュを生成
/// @param radius 半径
/// @param height 高さ
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateCapsule(float radius = 0.5f, float height = 2.0f, int texHandle = -1);

/// @brief 円盤メッシュ（XZ平面）を生成
/// @param radius 半径
/// @param segments 円周の分割数
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateCircle(float radius = 1.0f, uint32_t segments = 32, int texHandle = -1);

/// @brief リング（ドーナツ状の平板）メッシュ（XZ平面）を生成
/// @param innerRadius 内半径
/// @param outerRadius 外半径
/// @param segments 円周の分割数
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
/// @note UV は u = 円周方向 0..1、v = 内周側 1.0 / 外周側 0.0。
///       床置きのエフェクトデカール（DrawPrimitiveMeshScanRing）向け。
int GenerateRing(float innerRadius = 0.5f, float outerRadius = 1.0f,
                 uint32_t segments = 32, int texHandle = -1);

/// @brief 角度範囲・UV方向を指定できる拡張リングメッシュを生成
/// @param innerRadius 内半径
/// @param outerRadius 外半径
/// @param segments 円周の分割数
/// @param startAngle 開始角度（度）
/// @param endAngle 終了角度（度）
/// @param isVerticalUV UVを縦方向（半径→U / 角度→V）に生成するか
/// @param isXY true なら XY 平面、false なら XZ 平面に生成
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateRingEx(float innerRadius = 0.5f, float outerRadius = 1.0f,
                   uint32_t segments = 32, float startAngle = 0.0f,
                   float endAngle = 360.0f, bool isVerticalUV = false,
                   bool isXY = false, int texHandle = -1);

/// @brief エフェクト用の蓋なし円柱（円錐台）メッシュを生成
/// @param topRadius 上面の半径
/// @param bottomRadius 底面の半径
/// @param height 高さ
/// @param segments 円周の分割数
/// @param startAngle 開始角度（度）
/// @param endAngle 終了角度（度）
/// @param isVerticalUV UVを縦方向に生成するか
/// @param flipV V座標を反転させるか
/// @param texHandle テクスチャハンドル
/// @return メッシュハンドル
int GenerateEffectCylinder(float topRadius = 1.0f, float bottomRadius = 1.0f,
                           float height = 3.0f, uint32_t segments = 32,
                           float startAngle = 0.0f, float endAngle = 360.0f,
                           bool isVerticalUV = true, bool flipV = false,
                           int texHandle = -1);

/// @brief 3D 文字メッシュ（押し出し文字）を生成
/// @param desc 文字列・フォント・サイズ・厚さなどの生成パラメータ（Font/TextMeshGenerator.h）
/// @param texHandle テクスチャハンドル（-1 でデフォルトの白）
/// @param outInfo 生成されたメッシュのローカル AABB（不要なら nullptr）
/// @return メッシュハンドル（フォントが開けない・描く文字が無い場合は -1）
/// @note 生成物は通常の PrimitiveMesh なので DrawPrimitiveMesh / UnloadPrimitiveMesh /
///       GetPrimitiveMeshMaterialPtr 等がそのまま使える。文字列や厚さを変えたい場合は
///       Unload して再生成する。
int GenerateTextMesh(const TextMeshDesc &desc, int texHandle = -1,
                     TextMeshInfo *outInfo = nullptr);

/// @brief 3D 文字メッシュの縁取り（アウトライン）シェルを生成
/// @param desc 本体と同じ生成パラメータ
/// @param outlineWidth 縁取りの太さ（em 比）
/// @param color 縁取り色（マテリアルに書き込む）
/// @param unlit true なら非ライティング（単色）で固定
/// @param outInfo 生成されたメッシュのローカル AABB（不要なら nullptr）
/// @return メッシュハンドル（生成できなければ -1）
/// @note 本体の GenerateTextMesh と同じ Transform を与えて DrawPrimitiveMesh で重ね描きする。
int GenerateTextMeshOutline(const TextMeshDesc &desc, float outlineWidth,
                            const Vector4 &color, bool unlit = true,
                            TextMeshInfo *outInfo = nullptr);

/// @brief 汎用プリミティブメッシュを描画
/// @param meshHandle メッシュハンドル
/// @param texHandle 一時的に差し替えるテクスチャ（-1 なら生成時のテクスチャ）
void DrawPrimitiveMesh(int meshHandle, int texHandle = -1);

/// @brief プリミティブメッシュを水専用シェーダーで描画する
/// @param meshHandle メッシュハンドル
/// @param texHandle 適用するテクスチャハンドル（-1でデフォルト）
void DrawPrimitiveMeshWater(int meshHandle, int texHandle = -1);
void DrawPrimitiveMeshWaterColumn(int meshHandle, int texHandle = -1);

// ── エフェクト用プリミティブ描画（加算合成・非ライティング） ────────

/// @brief リングメッシュを「場所指定ホログラム」シェーダーで描画する
/// @param meshHandle メッシュハンドル（GenerateRing / GenerateRingEx で生成）
/// @param texHandle 適用するテクスチャハンドル（-1でデフォルト。シェーダーは参照しない）
/// @note 加算合成・深度書き込みOFF・両面描画。発光色や進捗は
///       SetPrimitiveMeshEffectParams() で毎フレーム渡す。
void DrawPrimitiveMeshScanRing(int meshHandle, int texHandle = -1);

/// @brief 板メッシュを「接続ビーム」シェーダーで描画する
/// @param meshHandle メッシュハンドル（GeneratePlane で生成した 1x1 の板）
/// @param texHandle 適用するテクスチャハンドル（-1でデフォルト。シェーダーは参照しない）
/// @note ローカル +Z をビームの進行方向、scale.z を全長、scale.x を幅として扱う。
void DrawPrimitiveMeshScanBeam(int meshHandle, int texHandle = -1);

/// @brief エフェクト用シェーダーへ渡すパラメータをまとめて設定する
/// @param meshHandle メッシュハンドル
/// @param color 発光色（RGB）と明度スケール（A）
/// @param progress 進捗・伸長率 0.0〜1.0
/// @param time 経過秒数（アニメーションの位相）
/// @note DrawPrimitiveMeshScanRing / DrawPrimitiveMeshScanBeam 専用。
///       Material の未使用スロット（shininess / environmentCoefficient）を
///       progress / time として流用するため、これらのメッシュに対して
///       SetPrimitiveMeshEnvironmentCoefficient() は使用しないこと。
void SetPrimitiveMeshEffectParams(int meshHandle, const Vector4 &color,
                                  float progress, float time);

/// @brief 汎用プリミティブメッシュを解放
/// @param meshHandle メッシュハンドル
void UnloadPrimitiveMesh(int meshHandle);

/// @brief 汎用プリミティブメッシュの Transform ポインタを取得
/// @param meshHandle メッシュハンドル
/// @return Transform*（無効ハンドルなら nullptr）
Transform *GetPrimitiveMeshTransformPtr(int meshHandle);

/// @brief プリミティブメッシュの環境マップ映り込み係数を設定する
/// @param meshHandle メッシュハンドル
/// @param coeff 映り込み係数（0=映り込みなし、1=完全鏡面）
void SetPrimitiveMeshEnvironmentCoefficient(int meshHandle, float coeff);

/// @brief プリミティブメッシュの法線マップを設定する
/// @param meshHandle メッシュハンドル
/// @param texHandle 法線マップのテクスチャハンドル
void SetPrimitiveMeshNormalMap(int meshHandle, int texHandle);

/// @brief プリミティブメッシュのラフネスマップを設定する
/// @param meshHandle メッシュハンドル
/// @param texHandle ラフネスマップのテクスチャハンドル
void SetPrimitiveMeshRoughnessMap(int meshHandle, int texHandle);

/// @brief プリミティブメッシュのマテリアルポインタを取得する（直接編集用）
/// @param meshHandle メッシュハンドル
/// @return Material*（無効ハンドルなら nullptr）
Material *GetPrimitiveMeshMaterialPtr(int meshHandle);

/// @brief プリミティブメッシュのライティングモードを設定する
/// @param meshHandle メッシュハンドル
/// @param mode ライティングモード
/// @note 設定以降、このメッシュは DirectionalLight の LightingMode に追従しない。
///       追従に戻すには ClearPrimitiveMeshLightingModeOverride() を呼ぶ。
void SetPrimitiveMeshLightingMode(int meshHandle, LightingMode mode);

/// @brief ライティングモードの個別設定を解除し、DirectionalLight に追従させる
/// @param meshHandle メッシュハンドル
void ClearPrimitiveMeshLightingModeOverride(int meshHandle);

// ── 水面描画 (Water) ──────────────────────────────

/// @brief 水面用の高分割平面メッシュを生成
/// @param width 幅
/// @param height 奥行き
/// @param segments 分割数（各軸）
/// @param normalMapHandle 法線マップテクスチャハンドル（-1 ならテクスチャ無し）
/// @return メッシュハンドル（失敗時は -1）
int GenerateWaterPlane(float width, float height, uint32_t segments, int normalMapHandle = -1);

/// @brief 水面を描画する
/// @param meshHandle メッシュハンドル（GenerateWaterPlane で取得）
/// @param normalMapHandle 法線マップテクスチャハンドル（-1 なら生成時のテクスチャ）
void DrawWater(int meshHandle, int normalMapHandle = -1);

/// @brief 水面メッシュを解放する
/// @param meshHandle メッシュハンドル
void UnloadWater(int meshHandle);

/// @brief 水面メッシュの Transform ポインタを取得
/// @param meshHandle メッシュハンドル
/// @return Transform*（無効ハンドルなら nullptr）
Transform *GetWaterTransformPtr(int meshHandle);

/// @brief 水面パラメータを定数バッファに設定する
/// @param waveHeight 主波の高さ
/// @param waveSpeed 主波の速度
/// @param waveFreq 主波の周波数
/// @param waveHeight2 副波の高さ
/// @param waveSpeed2 副波の速度
/// @param waveFreq2 副波の周波数
/// @param waveSteepness Gerstner波の鋭さ (0..1)
/// @param shallowColor 浅瀬の色
/// @param deepColor 深海の色
/// @param fresnelPower フレネル指数
/// @param specularPower スペキュラ指数
/// @param normalScrollSpeed 法線マップスクロール速度
/// @param normalStrength 法線マップ強度
void SetWaterParams(float waveHeight, float waveSpeed, float waveFreq,
                    float waveHeight2, float waveSpeed2, float waveFreq2,
                    float waveSteepness,
                    const Vector4 &shallowColor, const Vector4 &deepColor,
                    float fresnelPower, float specularPower,
                    float normalScrollSpeed, float normalStrength);

/// @brief 水面の環境マップ映り込み係数を設定する
/// @param meshHandle メッシュハンドル
/// @param coeff 映り込み係数（0=映り込みなし、1=完全鏡面）
void SetWaterEnvironmentCoefficient(int meshHandle, float coeff);

/// @brief 波の高さによる色付け（山を明るく・谷を暗く）の強さを設定する
/// @param crestTint 0 で無効（従来どおり）。0.5〜1.0 で真上から見ても波のうねりが色で分かる
/// @details 水面の色は視線と法線の角度（フレネル）で浅瀬色⇔深海色を切り替えているため、
///          真上から見下ろすと角度がほぼ一定になり、うねりがまったく色に出ない。
///          これを補うために、頂点の高さ（静水面からの変位）で色を振る。
///          値は WaterParamsCB の未使用フィールド gFoamParams.z へ載せるため CB レイアウトは不変。
void SetWaterCrestTint(float crestTint);

/// @brief 海面のリアル化パラメータを設定する（詳細波・白波・SSS・透明度など）
/// @param detailStrength 詳細波の強さ（0 で従来の 3 波だけ）
/// @param choppiness 詳細波の尖り 0..1
/// @param minWavelength 頂点で変位させる最短波長 m（WaterComponent::MinDisplacedWavelength()）
/// @param whitecapStrength 白波の濃さ（0 で無効）
/// @param whitecapCoverage 白波の量 0..1
/// @param sssColor 波頭を透ける光の色 (rgb) と強さ (a)
/// @param clarity 透明度 m（0 で無効）
/// @param detailFadeDistance 遠景で細かい法線を弱める距離 m（0 で無効）
/// @param normalTileSize 法線マップ 1 枚のワールドサイズ m（0 で平面の UV に貼る従来方式）
/// @details CPU 側の水面（RC::WaterSurface）と波形を一致させるため、detailStrength / choppiness /
///          minWavelength は WaterComponent::ToWaveParams() と同じ値を渡すこと。
void SetWaterOceanParams(float detailStrength, float choppiness, float minWavelength,
                         float whitecapStrength, float whitecapCoverage,
                         const Vector4 &sssColor, float clarity,
                         float detailFadeDistance, float normalTileSize);

/// @brief 水面の屈折（スクリーンテクスチャ方式）を設定する
/// @param enable true で、水を描く直前の画面をコピーし、法線で歪めて水中を透かす
/// @param strength 歪みの強さ（画面 UV 単位。0.02〜0.06 くらい）
/// @param edgeFade 物体との交差部をぼかす幅 m（ここだけ αで透ける）
/// @details 有効時は浅瀬色を掛けた歪んだ画面と深海色を水深（透明度）で混ぜて不透明に描く。
///          コピーが取れない描画先（MSAA など）では自動で従来の αブレンドに戻る。
void SetWaterRefraction(bool enable, float strength, float edgeFade);

/// @brief 水面用のフレーム時間を設定する
/// @param timeSec 累積時間（秒）
void SetWaterTime(float timeSec);

/// @brief 水面用のフレーム時間を取得する
/// @return 累積時間（秒）。まだ設定されていなければ 0
/// @details C-01 の水面高さ計算（RC::WaterSurface）は GPU と同じ時刻で
///          評価しないと、見た目の水面と当たり判定の水面がズレる。
///          スクリプト側で時間を持ち直さず、必ずこの値を使うこと。
float GetWaterTime();

/// @brief 水面シェーダへ渡している障害物リストを取得する
/// @param out 書き込み先（xyz: 位置, w: 半径）。nullptr 可（数だけ知りたいとき）
/// @param maxCount out に書き込める最大数
/// @return 実際の障害物の数
/// @details 障害物は反射波を作るだけでなく、内側の波を平らに潰す（insideMask）。
///          CPU 側の水面高さ計算（RC::WaterSurface）でこれを渡さないと、
///          岩の近くで「描画は平らなのに当たり判定だけ波打つ」状態になる。
/// @note リストの中身を決めるのはアプリ側（水面に載せた WaterObstacleScript）。
///       この取得口は SetWaterObstacles で入れられた値をそのまま返す。
int GetWaterObstacles(Vector4* out, int maxCount);

/// @brief 水面シェーダへ渡す障害物リストを差し替える
/// @param obstacles xyz: ワールド座標, w: 半径。nullptr なら障害物なしにする
/// @param count 障害物の数（上限を超えたぶんは切り捨てる）
/// @details 以前はレンダラ内にハードコードしていた（技術的負債 D-01）。
///          「どれが障害物か」はシーンの中身を知っているアプリ側にしか決められないため、
///          エンジンは入れ物と受け口だけを持ち、中身は毎フレーム外から入れてもらう。
///
///          水面を一度も描いていない段階でも呼べる（定数バッファはまだ無いので
///          いったん内部に控え、次の描画でシェーダへ渡る）。
/// @note 半径は「その障害物の水際での大きさ」。シェーダはこの半径の円として扱い、
///       内側の波を平らに潰し、円周上で波を反射させる。実物より大きくすると
///       岩から離れた場所に平らな輪と反射の輪ができるので、見た目と合わせること。
void SetWaterObstacles(const Vector4* obstacles, int count);

/// @brief 反射波の強さ（シェーダの gObstacleCount.y）
float GetWaterReflectStrength();

/// @brief 反射波の到達範囲（障害物半径に対する倍率／gObstacleCount.z）
float GetWaterReflectRange();

/// @brief 反射波のチューニング値を差し替える
/// @param strength 反射の強さ（1.0 で入射波と同じ振幅）
/// @param range 反射の到達範囲（障害物半径に対する倍率）
/// @warning **どちらも 0 を渡してはいけない。** シェーダ側も CPU 側の WaterSurface も
///          `(値 > 0) ? 値 : 既定値` というフォールバックを持つため、0 は
///          「無効」ではなく既定値（1.0 / 3.0）として扱われる（技術的負債 D-14）。
///          反射を消したいときは障害物リストのほうを空にすること。
void SetWaterReflectParams(float strength, float range);

/// @brief 反射波のチューニング値を確認用に上書きする（C-03）
/// @param enable true のあいだ SetWaterReflectParams の値を無視して下の値を使う
/// @param strength 反射の強さ
/// @param range 反射の到達範囲（障害物半径に対する倍率）
/// @details 水面に載せた WaterObstacleScript が毎フレーム SetWaterReflectParams を
///          呼び直すため、外から一度書いた値はすぐ上書きされて効かない。
///          そこで「描画時に定数バッファへ写す直前」で横取りする口を用意し、
///          ゲームを動かしたまま反射の効きを見比べられるようにする。
///
///          enable == false（既定）なら何もしないので、通常動作は変わらない。
/// @warning strength / range に 0 を渡さないこと。シェーダ側も CPU 側も
///          `(値 > 0) ? 値 : 既定値` というフォールバックを持つため、
///          0 は「無効」ではなく既定値 1.0 / 3.0 として扱われる（D-14）。
///          反射をほぼ消したいときは 0.01 のような小さい正の値を使う。
void SetWaterReflectOverride(bool enable, float strength, float range);

/// @brief 反射波の確認用オーバーライドの現在値を取得する
/// @param outStrength 強さの書き込み先（nullptr 可）
/// @param outRange 到達範囲の書き込み先（nullptr 可）
/// @return オーバーライドが有効なら true
bool GetWaterReflectOverride(float* outStrength, float* outRange);

/// @brief 障害物リストが持てる最大数
/// @details HLSL の `gObstacles[4]` と `RC::WaterSurface::kMaxObstacles` と揃っている。
int GetMaxWaterObstacles();

/// @brief 水面の共有リソース（定数バッファなど）を解放する
void TermWaterResources();


// ── Primitive2D（即時描画） ──────────────────────────

/// @brief 線描画
/// @param pos1 座標1（画面ピクセル座標）
/// @param pos2 座標2（画面ピクセル座標）
/// @param color 色（RGBA）
/// @param thickness 線の太さ（ピクセル）
/// @param feather アンチエイリアス幅（ピクセル）
void DrawLine(const Vector2 &pos1, const Vector2 &pos2, const Vector4 &color,
              float thickness = 1.0f, float feather = 1.0f);

/// @brief 四角形描画
/// @param pos1 座標1（画面ピクセル座標）
/// @param pos2 座標2（画面ピクセル座標）
/// @param color 色（RGBA）
/// @param fillMode 塗りつぶし設定
/// @param feather アンチエイリアス幅（ピクセル）
void DrawBox(const Vector2 &pos1, const Vector2 &pos2, const Vector4 &color,
             kFillMode fillMode = kFill, float feather = 1.0f);

/// @brief 円描画
/// @param center 円の中心点（画面ピクセル座標）
/// @param radius 円の半径（ピクセル）
/// @param color 色（RGBA）
/// @param fillMode 塗りつぶし設定
/// @param feather アンチエイリアス幅（ピクセル）
void DrawCircle(const Vector2 &center, float radius, const Vector4 &color,
                kFillMode fillMode = kFill, float feather = 1.0f);

/// @brief 三角形描画
/// @param pos1 頂点1（画面ピクセル座標）
/// @param pos2 頂点2（画面ピクセル座標）
/// @param pos3 頂点3（画面ピクセル座標）
/// @param color 色（RGBA）
/// @param fillMode 塗りつぶし設定
/// @param feather アンチエイリアス幅（ピクセル）
void DrawTriangle(const Vector2 &pos1, const Vector2 &pos2, const Vector2 &pos3,
                  const Vector4 &color, kFillMode fillMode = kFill,
                  float feather = 1.0f);

// ── Primitive3D（デバッグ線） ──────────────────────────

/// @brief 3D線描画
/// @param a 始点（ワールド座標）
/// @param b 終点（ワールド座標）
/// @param color 色（RGBA）
/// @param depth 深度テストを行うか
void DrawLine3D(const Vector3 &a, const Vector3 &b, const Vector4 &color,
                bool depth = true);

/// @brief 3DAABB描画
/// @param mn 最小座標（ワールド座標）
/// @param mx 最大座標（ワールド座標）
/// @param color 色（RGBA）
/// @param depth 深度テストを行うか
void DrawAABB3D(const Vector3 &mn, const Vector3 &mx, const Vector4 &color,
                bool depth = true);

/// @brief カメラの視錐台（Frustum）をワイヤフレーム描画する
/// @param position カメラのワールド座標
/// @param rotation カメラの回転角 (ラジアン)
/// @param fovY 垂直画角 (ラジアン)
/// @param aspect アスペクト比
/// @param nearZ ニアクリップ距離
/// @param farZ ファークリップ距離
/// @param color 色（RGBA）
/// @param depth 深度テストを行うか
void DrawFrustum3D(const Vector3 &position, const Vector3 &rotation,
                   float fovY, float aspect, float nearZ, float farZ,
                   const Vector4 &color, bool depth = true);

/// @brief オーバーレイ3D描画を開始する
/// @details この呼び出しから EndOverlay3D() までの間に描画されたプリミティブは
/// オーバーレイレイヤー（モデルの後）に描画される。深度テストなし。
void BeginOverlay3D();

/// @brief オーバーレイ3D描画を終了する
void EndOverlay3D();

/// @brief 3Dグリッド - XZ描画
/// @param halfSize グリッドの半分のサイズ
/// @param step グリッドの間隔
/// @param color 色（RGBA）
/// @param depth 深度テストを行うか
void DrawGridXZ3D(int halfSize, float step, const Vector4 &color,
                  bool depth = true);

/// @brief 3Dグリッド - XY描画
/// @param halfSize グリッドの半分のサイズ
/// @param step グリッドの間隔
/// @param color 色（RGBA）
/// @param depth 深度テストを行うか
void DrawGridXY3D(int halfSize, float step, const Vector4 &color,
                  bool depth = true);

/// @brief 3Dグリッド - YZ描画
/// @param halfSize グリッドの半分のサイズ
/// @param step グリッドの間隔
/// @param color 色（RGBA）
/// @param depth 深度テストを行うか
void DrawGridYZ3D(int halfSize, float step, const Vector4 &color,
                  bool depth = true);

/// @brief 3Dワイヤー球（緯線/経線）
/// @param center 中心（ワールド座標）
/// @param radius 半径
/// @param color 色（RGBA）
/// @param slices 経度分割
/// @param stacks 緯度分割
/// @param depth 深度テストを行うか
void DrawWireSphere3D(const Vector3 &center, float radius, const Vector4 &color,
                      int slices = 24, int stacks = 12, bool depth = true);

/// @brief 3Dリング球（XY/XZ/YZ の 3本リング）
/// @param center 中心（ワールド座標）
/// @param radius 半径
/// @param color 色（RGBA）
/// @param segments 円の分割数
/// @param depth 深度テストを行うか
void DrawSphereRings3D(const Vector3 &center, float radius,
                       const Vector4 &color, int segments = 32,
                       bool depth = true);

/// @brief 3Dアーク（円弧 / 扇形）
/// @param center 中心（ワールド座標）
/// @param normal 面の法線（ワールド）
/// @param fromDir 開始方向
/// @param radius 半径
/// @param startRad 開始角（ラジアン）
/// @param endRad 終了角（ラジアン）
/// @param color 色（RGBA）
/// @param segments 分割数
/// @param depth 深度テストを行うか
/// @param drawToCenter true なら扇形の中心線も描く
void DrawArc3D(const Vector3 &center, const Vector3 &normal,
               const Vector3 &fromDir, float radius, float startRad,
               float endRad, const Vector4 &color, int segments = 32,
               bool depth = true, bool drawToCenter = false);

/// @brief ワイヤーカプセル
/// @param p0 端の球の中心（ワールド座標）
/// @param p1 端の球の中心（ワールド座標）
/// @param radius 半径
/// @param color 色（RGBA）
/// @param segments 円/半円の分割数
/// @param depth 深度テストを行うか
void DrawCapsule3D(const Vector3 &p0, const Vector3 &p1, float radius,
                   const Vector4 &color, int segments = 16,
                   bool depth = true);

/// @brief OBB（回転付き箱）
/// @param center 中心（ワールド座標）
/// @param axisX ローカルX軸（ワールド）
/// @param axisY ローカルY軸（ワールド）
/// @param axisZ ローカルZ軸（ワールド）
/// @param halfExtents 各軸方向の半サイズ
/// @param color 色（RGBA）
/// @param depth 深度テストを行うか
void DrawOBB3D(const Vector3 &center, const Vector3 &axisX,
               const Vector3 &axisY, const Vector3 &axisZ,
               const Vector3 &halfExtents, const Vector4 &color,
               bool depth = true);

/// @brief 視錐台（コーナー8点）
/// @param corners corners[0..3]=near, corners[4..7]=far
/// @param color 色
/// @param depth 深度テストを行うか
void DrawFrustumCorners3D(const Vector3 corners[8], const Vector4 &color,
                          bool depth = true);

/// @brief 視錐台（カメラパラメータから生成）
void DrawFrustum3D(const Vector3 &camPos, const Vector3 &forward,
                   const Vector3 &up, float fovYRad, float aspect,
                   float nearZ, float farZ, const Vector4 &color,
                   bool depth = true);

/// @brief 内部バッファの即時描画（通常は自動で呼ばれます）
void FlushPrimitive3D();


// ===============================
// ── 画面オーバーレイ（ポスト風） ───────────────────

/// @brief 画面全体に白い霧（寒い雰囲気）をオーバーレイする
/// @param timeSec 時間（秒）。ノイズを流すのに使用
/// @param intensity 濃さ（0..1）
/// @param scale 模様の大きさ（大きいほど細かい）
/// @param speed 流れる速度（秒あたり）
/// @param wind 流れる方向（UV空間）
/// @param feather もやの柔らかさ
/// @param bottomBias 下側を濃くする係数
/// @note 3D/2D を描いた後、最後に呼ぶと画面全体にかかります。
void DrawFogOverlay(float timeSec, float intensity, float scale, float speed,
                    const Vector2 &wind, float feather, float bottomBias);

/// @brief FogOverlay の色（霧の色味）を設定する
/// @param color RGBA(0..1)
void SetFogOverlayColor(const Vector4 &color);


// ── 共通関数 ────────────────────────────────────

/// @brief テクスチャをロードしてハンドルを返す
/// @param path 画像パス
/// @param srgb sRGBとして読み込むか
/// @return テクスチャハンドル（失敗時は -1）
int LoadTex(const std::string &path, bool srgb = true);

/// @brief テクスチャハンドルから SRV の GPU デスクリプタハンドルを取得する
/// @param texHandle テクスチャハンドル
/// @return D3D12_GPU_DESCRIPTOR_HANDLE（無効なら ptr=0）
D3D12_GPU_DESCRIPTOR_HANDLE GetSrv(int texHandle);

/// @brief RenderCommon が保持している D3D12 Device を取得する
/// @return ID3D12Device*
ID3D12Device *GetDevice();

/// @brief RenderCommon が初期化済みかどうか
/// @return 初期化済みなら true
bool IsInitialized();


// ── ブレンドモード切り替え ────────────────────────

/// @brief 以降の Draw* が使用する BlendMode を設定する
/// @param blendMode ブレンドモード
void SetBlendMode(BlendMode blendMode);

/// @brief 現在の BlendMode を取得する
/// @return ブレンドモード
BlendMode GetBlendMode();


// ── 表示モード切り替え ──────────────────────────

/// @brief 以降の 3D 描画のシェーディングモードを設定する
/// @param mode シェーディングモード（Solid / Wireframe / SolidWireframe）
void SetViewShadingMode(ViewShadingMode mode);

/// @brief 現在のシェーディングモードを取得する
/// @return シェーディングモード
ViewShadingMode GetViewShadingMode();

/// @brief シェーディングモードの ImGui 表示を行う
/// @param label ラベル名
void DrawViewShadingModeImGui(const char *label = "View Shading");


// ── ポストエフェクト切り替え ────────────────────────

/// @brief ポストエフェクトを1つだけ設定する（スタックをクリア）
/// @param type ポストエフェクトタイプ（None でエフェクトなし）
void SetPostEffect(::PostEffectType type);

/// @brief 先頭のエフェクトを返す（空なら None）
/// @return ポストエフェクトタイプ
::PostEffectType GetPostEffect();

/// @brief エフェクトをスタックに追加する（重ね掛け）
/// @param type ポストエフェクトタイプ
void AddPostEffect(::PostEffectType type);

/// @brief エフェクトをスタックから除去する
/// @param type ポストエフェクトタイプ
void RemovePostEffect(::PostEffectType type);

/// @brief エフェクトスタックを全クリアする
void ClearPostEffects();

/// @brief 指定したエフェクトが有効か確認する
/// @param type エフェクトタイプ
/// @return 有効なら true
bool HasPostEffect(::PostEffectType type);

/// @brief ポストプロセスのアウトライン色を設定する
/// @param color 色（RGBA）
void SetPostProcessOutlineColor(const float color[4]);

/// @brief ポストプロセスのアウトライン検出の重みを設定する
/// @param weight 重み
void SetPostProcessOutlineWeight(float weight);

/// @brief ポストプロセスのアウトラインのピクセル幅（太さ）を設定する
/// @param thickness 太さ
void SetPostProcessOutlineThickness(float thickness);

// ── Dissolve ポストエフェクト ────────────────────────

/// @brief Dissolve の閾値を設定する (0.0 ~ 1.0)
/// @param threshold 0.0で全表示、1.0で全消失
void SetDissolveThreshold(float threshold);

/// @brief Dissolve の Edge 発光色を設定する
/// @param r 赤 (0.0 ~ 1.0)
/// @param g 緑 (0.0 ~ 1.0)
/// @param b 青 (0.0 ~ 1.0)
void SetDissolveEdgeColor(float r, float g, float b);

/// @brief Dissolve の ベースカラー（抜けた部分の背景色）を設定する
/// @param r 赤 (0.0 ~ 1.0)
/// @param g 緑 (0.0 ~ 1.0)
/// @param b 青 (0.0 ~ 1.0)
/// @param a アルファ (0.0 ~ 1.0)
void SetDissolveBaseColor(float r, float g, float b, float a);

/// @brief Dissolve の Edge 検出幅を設定する
/// @param range Edge幅 (default: 0.03)
void SetDissolveEdgeRange(float range);

/// @brief Dissolve のノイズテクスチャを切り替える
/// @param index ノイズテクスチャのインデックス
void SetDissolveNoiseIndex(int index);


/// @brief Dissolve のノイズテクスチャリストを初期化する
/// @note RC::Init 完了後に呼び出してください
void InitDissolveNoiseTextures();

/// @brief RandomNoise の強度を設定する (0.0 ~ 1.0)
void SetRandomNoiseIntensity(float intensity);

/// @brief RandomNoise の色を設定する (RGB)
void SetRandomNoiseColor(float r, float g, float b);

/// @brief Bloom のパラメータを設定する
/// @param threshold 光として拾う輝度の閾値 (0.0 ~ 1.0)
/// @param intensity 加算強度 (0.0 で無効)
/// @param radius    にじみの広がり（ピクセル。1.0 ~ 16.0 目安）
/// @param knee      閾値付近の柔らかさ (0.0 でスパッと切る)
void SetBloomParams(float threshold, float intensity, float radius, float knee);

/// @brief SSAO のパラメータを設定する
/// @param radius    サンプリング半径（メートル）
/// @param intensity 効きの強さ (0.0 で無効)
/// @param bias      自己遮蔽対策の下駄（メートル）
/// @param power     コントラスト（pow の指数）
void SetSsaoParams(float radius, float intensity, float bias, float power);

/// @brief カラーグレーディングのパラメータを設定する
/// @param exposure    露出（EV）
/// @param contrast    コントラスト (1.0 で素通し)
/// @param saturation  彩度 (1.0 で素通し)
/// @param temperature 色温度 (-1.0 寒色 ~ +1.0 暖色)
/// @param tint        色偏り (-1.0 緑 ~ +1.0 マゼンタ)
void SetColorGradeParams(float exposure, float contrast, float saturation,
                         float temperature, float tint);

/// @brief カラーグレーディングのカラーフィルタ (RGB) を設定する
void SetColorGradeFilter(float r, float g, float b);

/// @brief カラーグレーディングの適用率 (0.0 ~ 1.0) を設定する
void SetColorGradeAmount(float amount);

/// @brief MaskOutline の色を設定する (RGBA)
void SetMaskOutlineColor(float r, float g, float b, float a = 1.0f);

/// @brief MaskOutline の太さを設定する (1.0 ~ 4.0 ピクセル)
void SetMaskOutlineThickness(float thickness);

/// @brief MaskOutline の強さを設定する (0.0 ~ 1.0)
void SetMaskOutlineStrength(float strength);

/// @brief ScreenDroplets (レンズ水滴) の強度を設定する (0.0 ~ 1.0)
void SetScreenDropletsIntensity(float intensity);

/// @brief ScreenDroplets (レンズ水滴) の流れる速度を設定する
void SetScreenDropletsSpeed(float speed);

/// @brief ScreenDroplets (レンズ水滴) によるUV屈折強度を設定する
void SetScreenDropletsDistortion(float distortion);

/// @brief ScreenDroplets (レンズ水滴) の密度（グリッドスケール）を設定する
void SetScreenDropletsScale(float scale);

/// @brief BloodOverlay (被弾の血) の強さを設定する
/// @param hitFlash  被弾フラッシュ (0.0 ~ 1.0)。被弾の瞬間だけ跳ね上げる
/// @param lowHealth 低HP持続 (0.0 ~ 1.0)。掛けている間ずっと心拍で脈動する
/// @details 2つは強いほうが採用される。両方 0 なら完全に素通しになる。
void SetBloodOverlayLevels(float hitFlash, float lowHealth);

/// @brief BloodOverlay (被弾の血) の見た目を設定する
/// @param r,g,b        血の色
/// @param coverage     最大時に画面の何割まで侵食するか (0.0 ~ 1.0)
/// @param splatterScale 飛沫の密度（大きいほど細かい粒）
/// @param desaturate   血の下の彩度をどれだけ落とすか (0.0 ~ 1.0)
void SetBloodOverlayLook(float r, float g, float b, float coverage,
                         float splatterScale, float desaturate);

/// @brief BloodOverlay (被弾の血) の脈動の速さを設定する（1.0 で毎秒1拍）
void SetBloodOverlayPulseSpeed(float speed);

/// @brief BloodOverlay (被弾の血) の飛沫パターンを次の種へ進める
/// @details パターンは時間で変化しないので、被弾のたびに呼ばないと毎回同じ形が出る。
void RerollBloodOverlaySplatter();

/// @brief ポストエフェクトの ImGui 表示を行う
/// @param label ラベル名
void DrawPostEffectImGui(const char *label = "PostEffect");

/// @brief バックグラウンドでのロードタスクを追加する
/// @param task ロードタスク
void AddLoadingTask(std::future<void> &&task);

/// @brief 現在進行中のすべてのバックグラウンドロードタスクが完了するまで待機する
void WaitAllLoads();

/// @brief テクスチャのログ出力履歴をクリアする
void ClearTextureLogHistory();

} // namespace RC
