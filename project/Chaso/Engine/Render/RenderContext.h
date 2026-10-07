#pragma once

// ============================================================================
// RenderContext
// ----------------------------------------------------------------------------
// エンジン内部専用。描画サブシステム全体の状態を保持するクラス。
// RenderCommon.cpp が唯一のインスタンスを所有し、各サブモジュール
// (RenderModel.cpp 等) は GetRenderContext() 経由で参照する。
//
// このヘッダーをシーン側 (#include "RenderCommon.h" のみ) が直接
// include することはない。
// ============================================================================

#include <d3d12.h>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>
#include <wrl.h>
#include <future>
#include <mutex>

#include "SortKey.h"
#include "FrameResource.h"
#include "Dx12/ShadowMap/ShadowMap.h"


#include "Light/Area/AreaLightManager.h"
#include "Light/Directional/DirectionalLightManager.h"
#include "Light/Point/PointLightManager.h"
#include "Light/Spot/SpotLightManager.h"
#include "Model/ModelManager.h"
#include "Model/ModelInstanceBatcher.h"
#include "Model/ModelProxyPool.h"
#include "Effect/EffectParticleSystem.h"
#include "Skydome/SkydomeManager.h"
#include "Skybox/SkyboxManager.h"
#include "Mesh/PrimitiveMeshManager.h"
#include "Sprite/SpriteManager.h"
#include "Font/FontManager.h"
#include "Texture/TextureManager/TextureManager.h"

#include "GraphicsPipeline/GraphicsPipeline.h" // BlendMode
#include "Math/Math.h"
#include "Model/ModelObject.h" // ModelManager の unique_ptr<ModelObject> に必要
#include "Graphics/Skydome/Skydome.h"    // SkydomeManager の unique_ptr<Skydome> に必要
#include "Graphics/Skybox/Skybox.h"      // SkyboxManager の unique_ptr<Skybox> に必要
#include "Graphics/Mesh/PrimitiveMesh.h"
#include "Graphics/Mesh/MeshGenerator.h"
#include "function/function.h"
#include "struct.h"

class Primitive2D;
class Primitive3D;
class GraphicsPipeline;
class DescriptorHeap;
class PostProcess;
class RenderTexture;
struct SceneContext;

namespace RC {

/// @brief 描画サブシステム全体の状態と各リソースマネージャを統括するコンテキストクラス
/// エンジン内部専用であり、シングルトンとして描画パスの実行、コマンドのキューイング、マネージャへのアクセスを提供します。
class RenderContext {
public:
  /// @brief シングルトンインスタンスを取得する
  /// @return RenderContextの参照
  static RenderContext &GetInstance();

  RenderContext();
  /// @note 前方宣言のみの型（RenderTexture）を unique_ptr で持つため、
  ///       デストラクタは .cpp 側でだけ定義する（ヘッダで = default にしない）
  ~RenderContext();

  /// @brief 初期化処理
  /// @param ctx シーンコンテキスト
  void Init(SceneContext &ctx);

  /// @brief 終了処理
  void Term();

  /// @brief 初期化済みか確認する
  /// @return 初期化済みなら true
  bool IsInitialized() const { return initialized_; }

  /// @brief フレームごとのカメラ情報を設定する
  /// @param view ビュー行列
  /// @param proj 射影行列
  /// @param camWorldPos カメラのワールド座標
  void SetCamera(const Matrix4x4 &view, const Matrix4x4 &proj,
                 const Vector3 &camWorldPos);

  /// @brief 現在のフレームで使用するグラフィックスコマンドリストを設定する
  /// @param cl コマンドリストへのポインタ
  void SetCommandList(ID3D12GraphicsCommandList *cl) { cl_ = cl; }

  /// @brief シーンコンテキストへの参照を設定する
  /// @param ctx シーンコンテキストへのポインタ
  void SetSceneContext(SceneContext *ctx) { ctxRef_ = ctx; }

  /// @brief 現在のブレンドモードを設定する
  /// @param mode 設定するブレンドモード
  void SetBlendMode(BlendMode mode) { currentBlendMode_ = mode; }

  /// @brief 3D描画パスの開始前処理
  /// 各種定数バッファの更新やバインドを行います。
  /// @param ctx シーンコンテキスト
  /// @param cl コマンドリスト
  void PreDraw3D(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief 2D描画パスの開始前処理
  /// @param ctx シーンコンテキスト
  /// @param cl コマンドリスト
  void PreDraw2D(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief ポストプロセス後の 2D オーバーレイ描画を再開する
  /// @param ctx シーンコンテキスト
  /// @param cl コマンドリスト
  /// @details ポストプロセス（輪郭・水中・ビネット・ディゾルブ等）が掛かった最終画像の上に、
  ///          ポーズメニューのような「効果を受けたくない UI」を描くために使う。
  ///          呼び出し側が最終出力先（バックバッファ or ビューポート用テクスチャ）を
  ///          レンダーターゲットに設定してから呼ぶこと。
  ///          PreDraw2D と違い 3D コマンドの実行や Primitive2D / FontManager の BeginFrame は
  ///          行わない（同じフレームで二度 BeginFrame すると、先に描いた HUD の頂点・定数を
  ///          上書きしてしまう）。ビューポートと 2D 用のパイプラインだけを張り直す。
  /// @note 同じフレームで PreDraw2D が呼ばれたあとに使うこと。
  void ResumeDraw2D(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief 背景2D（モデルより後ろ）描画パスの開始前処理
  /// @param ctx シーンコンテキスト
  /// @param cl コマンドリスト
  /// @note PreDraw3D より前に呼ぶこと。3Dコマンドキューの実行を行わないため、
  ///       ここで積んだスプライトは 3D より先にコマンドリストへ記録され、
  ///       後から深度テスト付きで描かれるモデルに上書きされる（＝奥に見える）。
  /// @warning このパスは Sprite 専用。DrawString / Primitive2D は
  ///          頂点リングの BeginFrame が済んでいないため使えない。
  void PreDraw2DBackground(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief ビューのシェーディングモード（Solid, Wireframe 等）を設定する
  /// @param mode シェーディングモード
  void SetViewShadingMode(ViewShadingMode mode) { viewShadingMode_ = mode; }

  /// @brief 現在のビューシェーディングモードを取得する
  /// @return シェーディングモード
  ViewShadingMode GetViewShadingMode() const { return viewShadingMode_; }

  /// @brief テクスチャをロードする（マネージャへのラップ）
  /// @param path 画像パス
  /// @param srgb sRGBとして読み込むか
  /// @return テクスチャハンドル
  int LoadTex(const std::string &path, bool srgb);

  /// @brief テクスチャのSRVハンドルを取得する
  /// @param texHandle テクスチャハンドル
  /// @return GPUディスクリプタハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE GetSrv(int texHandle);

  // ── アクセッサ ──
  /// @brief 現在のコマンドリストを取得
  ID3D12GraphicsCommandList *CL() const { return cl_; }
  /// @brief シーンコンテキストを取得
  SceneContext *Ctx() const { return ctxRef_; }
  /// @brief D3D12デバイスを取得
  ID3D12Device *Device() const { return device_.Get(); }
  /// @brief 現在のブレンドモードを取得
  BlendMode CurrentBlendMode() const { return currentBlendMode_; }
  /// @brief ビュー行列を取得
  const Matrix4x4 &View() const { return view_; }
  /// @brief 射影行列を取得
  const Matrix4x4 &Proj() const { return proj_; }

  // ── マネージャへの参照 ──
  /// @brief モデルマネージャを取得
  ModelManager &Models() { return modelMan_; }
  /// @brief スプライトマネージャを取得
  SpriteManager &Sprites() { return spriteMan_; }
  /// @brief フォントマネージャを取得
  FontManager &Fonts() { return fontMan_; }
  /// @brief スカイドームマネージャを取得
  SkydomeManager &Skydomes() { return skydomeMan_; }
  /// @brief スカイボックスマネージャを取得
  SkyboxManager &SkyBoxes() { return skyboxMan_; }
  /// @brief プリミティブメッシュマネージャを取得
  PrimitiveMeshManager &PrimitiveMeshes() { return primitiveMeshMan_; }
  /// @brief 平行光源マネージャを取得
  DirectionalLightManager &DirLights() { return dirLightMan_; }
  /// @brief 点光源マネージャを取得
  PointLightManager &PtLights() { return ptLightMan_; }
  /// @brief スポットライトマネージャを取得
  SpotLightManager &SpLights() { return spLightMan_; }
  /// @brief エリアライトマネージャを取得
  AreaLightManager &ArLights() { return arLightMan_; }
  /// @brief テクスチャマネージャを取得
  TextureManager &Textures() { return texMan_; }
  /// @brief モデルのインスタンス描画のまとめ役を取得
  ModelInstanceBatcher &InstanceBatcher() { return instanceBatcher_; }
  /// @brief VirtualEntity（ModelProxyPool）を取得
  ModelProxyPool &ModelProxies() { return modelProxies_; }
  /// @brief Entity を使わないエフェクト粒（水しぶき・泡など）を取得
  EffectParticleSystem &EffectParticles() { return effectParticles_; }

  /// @brief 今描いているパスの ViewProjection（カリング用）
  /// @details 平行光源の影パス → ライト行列、スポット影タイル → そのタイルの行列、それ以外 → カメラ。
  Matrix4x4 CurrentPassViewProjection() const;

  /// @brief ポストプロセスオブジェクトを取得
  /// @return PostProcessへのポインタ
  PostProcess *GetPostProcess() const { return postProcess_; }

  /// @brief プレフィックスと現在のブレンドモードに基づいてPSOをバインドする
  /// @param prefix パイプラインのプレフィックス（"Object3D"等）
  /// @return バインドされたGraphicsPipelineへのポインタ
  GraphicsPipeline *BindPipeline(std::string_view prefix);

  /// @brief 条件に合うGraphicsPipelineを取得する（バインドはしない）
  /// @param prefix プレフィックス
  /// @param mode ブレンドモード
  /// @return GraphicsPipelineへのポインタ
  GraphicsPipeline *GetPipeline(std::string_view prefix, BlendMode mode);

  /// @brief カメラ定数バッファをバインドする
  void BindCameraCB();

  /// @brief 全種類のライト定数バッファを一括でバインドする（バインドのみ。転送は SyncLightCBs）
  void BindAllLightCBs();

  /// @brief Point / Spot / Area ライトの CPU 側状態を GPU 定数バッファへ転送する
  /// @details 以前は BindAllLightCBs（＝ドローごと）に約 49KB を毎回書き直していた。
  ///          ライト CB は 1 本しかなく GPU が読むのはコマンドリスト実行時なので、
  ///          「各描画パス（Execute3DCommands）の先頭で 1 回」書けば結果は同じになる。
  void SyncLightCBs();

  /// @brief 環境マップ用のSRVを設定する（PBR等で使用）
  /// @param srv GPUディスクリプタハンドル
  void SetEnvironmentMap(D3D12_GPU_DESCRIPTOR_HANDLE srv) { environmentMapSrv_ = srv; }

  /// @brief 現在設定されている環境マップのSRVを取得する
  /// @return GPUディスクリプタハンドル
  D3D12_GPU_DESCRIPTOR_HANDLE GetEnvironmentMapSrv() const { return environmentMapSrv_; }

  /// @brief 環境マップを現在のスロット（b6等）にバインドする
  void BindEnvironmentMap();

  /// @brief シャドウパラメータを更新する
  void UpdateShadowParams(const ShadowParams& params);

  /// @struct ShadowDebugOverride
  /// @brief シャドウパラメータを実行時に上書きするための確認用の口（C-04）
  /// @details DataDrivenScene::Render() は毎フレーム ShadowParams をローカルで
  ///          作り直して UpdateShadowParams() へ渡す（pcfRadius も bias も
  ///          そこにハードコードされている）ため、外から書いた値は必ず消える。
  ///          そこで「呼び出し側の値を無視して上書きする」処理を
  ///          shadowMapTexelSize と同じ場所に置き、PCF の効きを
  ///          ゲームを動かしたまま見比べられるようにする。
  ///
  ///          enabled == false（既定）のときは何もしないので、
  ///          この構造体を触らない限り従来と完全に同じ挙動になる。
  struct ShadowDebugOverride {
    bool enabled = false;      ///< true のとき以下の値で上書きする
    float pcfRadius = 1.0f;    ///< PCF のタップ間隔（0 以下で 1 タップ＝PCF 無効）
    float bias = 0.01f;        ///< シャドウバイアス
    float darkness = 0.5f;     ///< 影の濃さ（ShadowParams::color.w）
    bool forceDisable = false; ///< true で影そのものを切る（shadowMapEnabled = 0）
  };

  /// @brief シャドウの確認用オーバーライド設定を取得する
  ShadowDebugOverride &ShadowDebug() { return shadowDebug_; }
  /// @brief シャドウの確認用オーバーライド設定を取得する (const)
  const ShadowDebugOverride &ShadowDebug() const { return shadowDebug_; }

  /// @brief シャドウマップを取得する（確認用のプレビュー表示などに使う）
  const ShadowMap &GetShadowMap() const { return shadowMap_; }

  /// @brief シャドウ定数バッファとシャドウマップを現在のスロットにバインドする
  void BindShadow();

  /// @brief シャドウマップ用の描画パスを開始する
  void BeginShadowPass();

  /// @brief シャドウマップ用の描画パスを終了し、メイン描画用にSRVへ遷移する
  void EndShadowPass();

  /// @brief シャドウ（平行光源・スポット影タイル）描画中かどうか
  /// @details 影パスの VS は World 行列しか読まないため、Draw 側はこれを見て
  ///          WVP / WorldInverseTranspose の計算を省略できる（結果は変わらない）。
  bool IsShadowPass() const { return isShadowPass_; }

  // --------------------------------------------------------------------------
  // マスクパス（特定のオブジェクトだけを白く別RTへ書き、輪郭強調に使う）
  // --------------------------------------------------------------------------
  // 使い方（PreDraw3D の後、メイン3D描画の Draw を積む前）:
  //   BeginMaskPass();
  //   強調したい物だけ Draw（DrawModel 等をそのまま呼べる）
  //   Execute3DCommands();
  //   EndMaskPass();
  //
  // マスクパス中は PSO が "mask" 系へ振り替わるので、呼び出し側は
  // 通常の描画と同じコードでよい（シャドウパスと同じ仕組み）。

  /// @brief マスク描画パスを開始する（マスクRTを黒でクリアして描画先に設定）
  /// @return 開始できたか。false のときは描画も EndMaskPass も行わないこと
  ///         （そのまま描くと、マスク用のジオメトリがメイン画面に二重描画される）
  bool BeginMaskPass();

  /// @brief マスク描画パスを終了し、SRVへ遷移してメイン描画の描画先に戻す
  void EndMaskPass();

  /// @brief マスクRTのSRV（GPUハンドル）を取得する。まだ描かれていなければ ptr == 0
  D3D12_GPU_DESCRIPTOR_HANDLE GetMaskSRVGPU() const;

  // --------------------------------------------------------------------------
  // スポットライト影（灯ごとのシャドウマップをアトラス 1 枚に敷き詰める）
  // --------------------------------------------------------------------------
  // 使い方（PreDraw3D の後）:
  //   UpdateSpotShadowParams(cb);      // 灯ごとの ViewProjection などを転送
  //   BeginSpotShadowAtlas();          // アトラス全体をクリア
  //   for (i) { BeginSpotShadowTile(i); 影を落とす物を Draw; Execute3DCommands(); EndSpotShadowTile(); }
  //   EndSpotShadowAtlas();            // SRV へ遷移して通常描画へ戻す
  // SpotLight::shadowIndex にタイル番号を入れた灯だけが PS 側で遮蔽される。

  /// @brief スポットライト影の定数バッファ(b7)を更新する
  /// @details count / 各エントリの lightViewProjection・nearZ・farZ・tanHalfFov を呼び出し側が埋める。
  ///          tilesX / tilesY / tileSizePx / atlasTexelSize はアトラスの実寸で上書きされる。
  ///          PreDraw3D() より後に呼ぶこと（フレーム毎の一時 CB に書き込むため）。
  void UpdateSpotShadowParams(const SpotShadowCB &params);

  /// @brief スポット影アトラスへの描画を開始する（深度書き込み状態へ遷移し全面クリア）
  /// @return 開始できたら true（false のときはタイル描画を行わないこと）
  bool BeginSpotShadowAtlas();

  /// @brief アトラス内の 1 タイル（1 灯分）への深度描画を開始する
  /// @param tileIndex タイル番号 (0 〜 kMaxSpotShadows-1)。UpdateSpotShadowParams の entries と対応
  void BeginSpotShadowTile(int tileIndex);

  /// @brief タイルへの描画を終了する（シャドウパスフラグを下ろす）
  void EndSpotShadowTile();

  /// @brief アトラスへの描画を終了し、SRV へ遷移してメイン描画用のRTV/ビューポートへ戻す
  void EndSpotShadowAtlas();

  /// @brief スポット影アトラスを取得する（確認用のプレビュー表示などに使う）
  const ShadowMap &GetSpotShadowAtlas() const { return spotShadowAtlas_; }

  /// @brief 2Dプリミティブ描画オブジェクトを遅延生成・取得する
  Primitive2D *EnsurePrimitive2D();

  /// @brief 3Dプリミティブ描画オブジェクトを遅延生成・取得する
  Primitive3D *EnsurePrimitive3D();

  /// @brief 実行中のすべての非同期ロードタスクの完了を待機する
  void WaitAllLoads();

  /// @brief 非同期ロードタスクを追加する
  /// @param task futureオブジェクト
  void AddLoadingTask(std::future<void> &&task);

  /// @brief 描画コマンドの履歴保存用構造体
  /// @note debugName は文字列リテラル（静的寿命）を指す string_view。
  ///       std::string にすると 1 コマンドごとにヒープ確保が発生し、
  ///       影パス × 全オブジェクトぶん毎フレーム数千回の malloc になる。
  struct RenderCommandHistory {
    std::string_view debugName;
    int debugIndex;
    uint64_t sortKey;
  };

  /// @brief 描画履歴にコマンドを追加する
  /// @param name 文字列リテラル（静的寿命）を渡すこと
  void AddCommandHistory(const char* name, int index, uint64_t sortKey = 0) {
    currentCommandHistory_.push_back({name ? std::string_view(name) : std::string_view("Unknown"), index, sortKey});
  }

  /// @brief 3D描画用コマンドの構造体
  struct RenderCommand3D {
    enum Type {
      Other,      ///< 汎用ラムダ実行
      Primitive,  ///< プリミティブ描画のマージ用
    } type = Other;

    uint64_t sortKey = 0; ///< ソート順（0は追加順維持）
    std::function<void(ID3D12GraphicsCommandList *)> func; ///< 実行する処理

    // Primitive マージ用の情報
    bool primDepth = false;
    uint32_t primStart = 0;
    uint32_t primCount = 0;

    const char* debugName = "Unknown"; ///< デバッグ用コマンド名
    int debugIndex = -1; ///< デバッグ用ハンドル番号(Index)
  };

  /// @brief 3D描画コマンドをキューに追加する
  /// @param func 実行する関数
  /// @param debugName デバッグ用コマンド名
  /// @param debugIndex デバッグ用ハンドル番号
  void PushCommand3D(std::function<void(ID3D12GraphicsCommandList *)> func, const char* debugName = "Unknown", int debugIndex = -1) {
    RenderCommand3D cmd;
    cmd.type = RenderCommand3D::Other;
    cmd.func = std::move(func);
    cmd.debugName = debugName;
    cmd.debugIndex = debugIndex;
    commandQueue3D_.push_back(std::move(cmd));
  }

  /// @brief ソートキーを指定して3D描画コマンドをキューに追加する
  /// @param sortKey ソート順序
  /// @param func 実行する関数
  /// @param debugName デバッグ用コマンド名
  /// @param debugIndex デバッグ用ハンドル番号
  void PushCommand3D(uint64_t sortKey,
                     std::function<void(ID3D12GraphicsCommandList *)> func, const char* debugName = "Unknown", int debugIndex = -1) {
    RenderCommand3D cmd;
    cmd.type = RenderCommand3D::Other;
    cmd.sortKey = sortKey;
    cmd.func = std::move(func);
    cmd.debugName = debugName;
    cmd.debugIndex = debugIndex;
    commandQueue3D_.push_back(std::move(cmd));
  }

  /// @brief 3Dプリミティブ描画コマンドを追加する
  void PushPrimitive3DCommand(bool depth, uint32_t start, uint32_t count,
                              uint64_t sortKey = 0);

  /// @brief 次のフレームの Execute3DCommands 時にコマンドの実行順序をログ出力するよう要求する
  void RequestDumpCommandOrder() { dumpCommandOrder_ = true; }

  /// @brief キューに積まれた全3D描画コマンドを実行する
  void Execute3DCommands();

  /// @brief 「2D 描画のあと」に積んだ 3D コマンドをその場で実行する（UI より前面に出すオーバーレイ用）
  /// @details 通常の 3D コマンドは PreDraw2D の中で一括実行されるため、
  ///          そのあとに DrawModel 等を呼んでもキューに残るだけで今フレームには出ない。
  ///          この関数はキューをその場で実行し、実行後は 2D 用の PSO / ルートシグネチャへ戻すので、
  ///          2D の HUD より手前に 3D モデルを重ねられる。
  /// @note PreDraw2D のあと（＝2D 描画がすべて終わったあと）に呼ぶこと。
  ///       深度バッファはそのまま使われるので、カメラに近い位置に置いたモデルは全てを覆える。
  void ExecuteOverlay3DCommands();

  /// @brief 直前のフレームで実行された描画コマンド(3D/2D)のリストを取得する（デバッグ用）
  const std::vector<RenderCommandHistory>& GetLastCommandHistory() const { return lastCommandHistory_; }

  /// @brief 3D描画コマンドキューをクリアする
  void Clear3DCommands() { commandQueue3D_.clear(); }

  /// @brief オーバーレイモードを設定する
  void SetOverlayMode(bool enabled) { overlayMode_ = enabled; }
  /// @brief オーバーレイモードかどうか
  bool IsOverlayMode() const { return overlayMode_; }

  /// @brief 現在のフレーム用リソースアロケータを取得する
  FrameResource &CurrentFrame() { return frameResources_[frameIndex_]; }

  /// @brief フレーム開始。1 フレームに 1 回、コマンドリストを開いた直後に呼ぶ（App が呼ぶ）
  /// @details FrameResource を次の 1 枚へ進めてリセットし、フレームの通し番号を進める。
  ///          CPU と GPU を並行させているので、ここより前（Update 中）に確保した領域は
  ///          直前の 1 枚の続きに入る。その 1 枚が再利用されるのは 2 フレーム後で、
  ///          そのときには GPU の完了を待っているので安全。
  void BeginFrame();

  /// @brief フレームの通し番号（BeginFrame のたびに +1）
  uint64_t FrameSerial() const { return frameSerial_; }

  /// @brief 現在のフレームインデックスを取得
  uint32_t FrameIndex() const { return frameIndex_; }

  /// @brief フレームインデックスを進める
  void AdvanceFrame() { frameIndex_ = (frameIndex_ + 1) % FrameResource::kFrameCount; }

  /// @brief フォグ(Fog)用定数バッファを更新する
  void UpdateFogCB(float timeSec, float intensity, float scale, float speed,
                   const Vector2 &wind, float feather, float bottomBias);

  /// @brief フォグの色を設定する
  void SetFogColor(const Vector4 &color);

  /// @brief フォグ用定数バッファのリソースを取得する
  /// @brief フォグ CB を今フレームの領域へ送り、その GPU アドレスを返す（コマンド記録中に呼ぶ）
  D3D12_GPU_VIRTUAL_ADDRESS FogCBAddress() { return fogCBMapped_ ? fogCB_.Address() : 0; }

private:
  /// @brief スポット影 CB のタイル数・テクセルサイズをアトラスの実寸で埋める
  void ApplySpotShadowAtlasInfo_(SpotShadowCB &cb) const;

  bool initialized_ = false; ///< 初期化フラグ
  bool overlayMode_ = false; ///< オーバーレイモード（ギズモ用）
  bool dumpCommandOrder_ = false; ///< 1フレームだけコマンド実行順をダンプするフラグ
  bool isShadowPass_ = false; ///< シャドウパス中かどうかのフラグ
  bool isMaskPass_ = false;   ///< マスクパス中かどうかのフラグ（PSO を "mask" 系へ振り替える）
  bool maskReady_ = false;    ///< 今フレームのマスクRTが描き終わっているか

  Microsoft::WRL::ComPtr<ID3D12Device> device_;    ///< デバイス
  DescriptorHeap *srvHeap_ = nullptr;              ///< SRVヒープ
  ID3D12GraphicsCommandList *cl_ = nullptr;        ///< コマンドリスト
  SceneContext *ctxRef_ = nullptr;                 ///< シーンコンテキストへのポインタ
  PostProcess *postProcess_ = nullptr;             ///< ポストプロセス

  Matrix4x4 view_;                                 ///< ビュー行列
  Matrix4x4 proj_;                                 ///< 射影行列
  BlendMode currentBlendMode_ = kBlendModeNone;    ///< 現在のブレンドモード
  ViewShadingMode viewShadingMode_ = ViewShadingMode::Solid; ///< シェーディング設定

  // マネージャー群
  ModelManager modelMan_;                          ///< モデル管理
  SpriteManager spriteMan_;                        ///< スプライト管理
  FontManager fontMan_;                            ///< フォント/文字描画管理
  SkydomeManager skydomeMan_;                      ///< スカイドーム管理
  SkyboxManager skyboxMan_;                        ///< スカイボックス管理
  PrimitiveMeshManager primitiveMeshMan_;          ///< プリミティブメッシュ管理
  DirectionalLightManager dirLightMan_;            ///< 平行光源管理
  PointLightManager ptLightMan_;                   ///< 点光源管理
  SpotLightManager spLightMan_;                    ///< スポットライト管理
  AreaLightManager arLightMan_;                    ///< エリアライト管理
  TextureManager texMan_;                          ///< テクスチャ管理
  ModelInstanceBatcher instanceBatcher_;           ///< モデルのインスタンス描画のまとめ役
  ModelProxyPool modelProxies_;                    ///< VirtualEntity（見た目だけの軽量オブジェクト）
  EffectParticleSystem effectParticles_;           ///< エフェクト粒（水しぶき・泡・水柱）
  Matrix4x4 dirShadowViewProj_{};                  ///< 平行光源の影パスの ViewProjection（カリング用の控え）
  Matrix4x4 spotShadowTileViewProj_{};             ///< 描画中のスポット影タイルの ViewProjection（同上）

  D3D12_GPU_DESCRIPTOR_HANDLE environmentMapSrv_{}; ///< 環境マップSRV

  // Camera CB
  struct CameraCB {
    Vector3 worldPos;
    float _pad = 0.0f;
  };
  /// @brief カメラCB（値は CPU 側。バインド時に今フレームの領域へ送る。CPU/GPU 並行のため）
  DynamicCB<CameraCB> cameraCB_;
  CameraCB *cameraCBMapped_ = nullptr;              ///< 書き込み先（= cameraCB_.Ptr()）

  // Fog CB
  struct FogOverlayCB {
    float timeSec = 0.0f;
    float intensity = 0.25f;
    float scale = 4.0f;
    float speed = 0.05f;
    Vector2 wind = {0.08f, 0.03f};
    float feather = 0.18f;
    float bottomBias = 0.35f;
    Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f};
  };
  /// @brief フォグCB（値は CPU 側。バインド時に今フレームの領域へ送る。CPU/GPU 並行のため）
  DynamicCB<FogOverlayCB> fogCB_;
  FogOverlayCB *fogCBMapped_ = nullptr;            ///< 書き込み先（= fogCB_.Ptr()）

  // Shadow CB
  /// @brief シャドウCB（値は CPU 側。バインド時に今フレームの領域へ送る。CPU/GPU 並行のため）
  DynamicCB<ShadowParams> shadowCB_;
  ShadowParams *shadowCBMapped_ = nullptr;            ///< 書き込み先（= shadowCB_.Ptr()）
  ShadowMap shadowMap_;                               ///< シャドウマップリソース
  ShadowDebugOverride shadowDebug_;                   ///< 確認用のシャドウ上書き設定（既定は無効）

  // Mask（輪郭強調したいオブジェクトのシルエット）
  // RenderTexture は前方宣言できないので unique_ptr で持つ（RenderContext.cpp で生成）
  std::unique_ptr<RenderTexture> maskTexture_;        ///< マスクRT（画面と同解像度・同フォーマット）
  uint32_t maskWidth_ = 0;                            ///< 生成時の幅（リサイズ検出用）
  uint32_t maskHeight_ = 0;                           ///< 生成時の高さ

  // Spot Light Shadow（灯ごとの影アトラス）
  ShadowMap spotShadowAtlas_;                         ///< スポット影アトラス（kSpotShadowTilesX×Y タイル）
  bool spotShadowAtlasOpen_ = false;                  ///< BeginSpotShadowAtlas 〜 EndSpotShadowAtlas の間 true
  int currentSpotShadowTile_ = -1;                    ///< 描画中のタイル番号（-1 でタイル描画中ではない）
  /// @brief 今フレームのスポット影 CB（b7）。PreDraw3D でフレーム毎の一時 CB から確保する
  SpotShadowCB *spotShadowCBMapped_ = nullptr;        ///< 書き込み先（今フレーム分）
  D3D12_GPU_VIRTUAL_ADDRESS spotShadowCBAddr_ = 0;    ///< GPU アドレス（今フレーム分）
  /// @brief PreDraw3D を通らずに object3d 系を描いた場合（2D のみのシーン等）に b7 へ載せる
  ///        count = 0 の固定 CB。b7 が未バインドのまま Draw されないようにする
  Microsoft::WRL::ComPtr<ID3D12Resource> spotShadowFallbackCB_;
  /// @brief タイル描画用の ShadowParams（b6）スライス。Shadow.VS が lightViewProjection を読むため、
  ///        タイルごとに別スライスを用意して切り替える（同じく PreDraw3D で確保）
  ShadowParams *spotShadowPassParamsMapped_ = nullptr; ///< 先頭スライス（256byte 間隔で kMaxSpotShadows 個）
  D3D12_GPU_VIRTUAL_ADDRESS spotShadowPassParamsAddr_ = 0; ///< 先頭スライスの GPU アドレス
  /// @brief BindPipeline が b6 に載せるアドレスの上書き。0 なら shadowCB_（平行光源の影）、
  ///        スポットタイル描画中はそのタイルのスライス
  D3D12_GPU_VIRTUAL_ADDRESS shadowCBBoundAddr_ = 0;
  static constexpr uint32_t kShadowParamsSliceStride = 256; ///< ShadowParams スライスの間隔（CB の 256byte 制約）

  std::unique_ptr<Primitive2D> prim2D_;             ///< 2Dプリミティブ描画器
  std::unique_ptr<Primitive3D> prim3D_;             ///< 3Dプリミティブ描画器

  std::vector<RenderCommand3D> commandQueue3D_;    ///< 3D描画コマンドキュー
  /// @brief Execute3DCommands がシャドウ／マスクパス中に持ち越すプリミティブコマンドの退避先
  ///        （毎回ローカルで確保せず使い回して、パスごとの再確保を無くす）
  std::vector<RenderCommand3D> deferredCommands3D_;
  std::vector<RenderCommandHistory> currentCommandHistory_; ///< 現在フレームの描画キュー履歴
  std::vector<RenderCommandHistory> lastCommandHistory_;    ///< デバッグ表示用の前フレーム描画キュー履歴

  /// @brief PSO 検索結果のキャッシュ（prefix + BlendMode → GraphicsPipeline*）
  /// @details PipelineManager::Get は毎回 std::string のキーを組んでハッシュマップを引く。
  ///          ドローごとに呼ばれるため、ここで小さな線形キャッシュに置き換える。
  ///          PipelineManager はエントリを削除しない（Rebuild も同じオブジェクトを再利用）ので
  ///          ポインタは Term まで有効。見つからなかった結果（nullptr）はキャッシュしない。
  struct PsoCacheEntry {
    std::string prefix;
    BlendMode mode;
    GraphicsPipeline *pso;
  };
  std::vector<PsoCacheEntry> psoCache_;

  /// @brief シャドウ／アトラス描画中に t4/t5 へ載せるダミー SRV（white1x1）のキャッシュ
  /// @details 以前は BindPipeline がドローごとに TextureManager::LoadID（mutex＋パス正規化＋map 検索）
  ///          を呼んでいた。ロード完了後の SRV は変わらないので一度取れたら保持する。
  D3D12_GPU_DESCRIPTOR_HANDLE dummyWhiteSrv_{};

  std::array<FrameResource, FrameResource::kFrameCount> frameResources_; ///< フレーム別リソース
  uint32_t frameIndex_ = 0;                         ///< 現在のフレームリソースインデックス
  uint64_t frameSerial_ = 1;                        ///< フレームの通し番号（DynamicCB の使い回し判定用）

  std::vector<std::future<void>> ongoingTasks_;    ///< 実行中の非同期タスク
  std::mutex mtxTasks_;                            ///< タスク管理用ミューテックス

  std::mutex mtxResource_;                         ///< リソース生成同期用ミューテックス

public:
  /// @brief リソース生成時などの排他制御用ミューテックスを取得する
  std::mutex &ResourceMutex() { return mtxResource_; }
};

/// @brief グローバルなRenderContextインスタンスを取得する（ヘルパー）
RenderContext &GetRenderContext();

} // namespace RC
