#pragma once
#include "Render/FrameResource.h" // RC::DynamicCB
#include "ComputeShader/ComputeShader.h"
#include "GraphicsPipeline/GraphicsPipeline.h"
#include "SRVManager/SRVManager.h"
#include "struct.h"
#include <array>
#include <cstdint>
#include <string>
#include <d3d12.h>
#include <wrl/client.h>

struct SceneContext;
class PipelineManager;
class DeferredReleaseQueue;

/// @brief パーティクルの挙動タイプ
enum class ParticleType : uint8_t {
  Default,    ///< 上方向噴出（既存）
  Explosion,  ///< 全方向放射（爆発）
  Rain,       ///< 下方向落下（雨）
  Fire,       ///< 炎（浮力・ゆらぎ・中心軸への収束）
  Electric,   ///< 電撃（殻の表面を這う火花。モデルにまとわりつく帯電表現）
  Count       ///< 種類数（配列サイズ用。タイプとしては使わない）
};

/// @class GPUParticle
/// @brief GPU 上で初期化・更新・描画を行うパーティクルシステム（FreeList 方式）
/// @details Compute Shader でパーティクルデータを管理し、
///          FreeList で寿命切れパーティクルを再利用する。
///          EmitCS で射出、UpdateCS で更新、VS でビルボード描画する。
///          パーティクルタイプを切り替えると、Emit / Update の CS を動的に差し替えられる。
class GPUParticle {
public:
  /// @brief コンストラクタ（リソースは Initialize で作る）
  GPUParticle() = default;

  /// @brief デストラクタ（Finalize を呼ぶ）
  ~GPUParticle();

  // コピー禁止（GPU リソースとディスクリプタを所有しているため）
  GPUParticle(const GPUParticle &) = delete;
  GPUParticle &operator=(const GPUParticle &) = delete;

  /// @brief バッファ・定数バッファ・頂点バッファ・テクスチャ・CS を準備する
  /// @param ctx シーンコンテキスト（デバイス・SRVManager・PipelineManager を取り出す）
  /// @note CS による初期化はコマンドリストが無いので、初回の Render まで遅らせる。
  void Initialize(SceneContext &ctx);

  /// @brief リソースとディスクリプタを解放する（GPU が使い終わってから遅延解放する）
  void Finalize();

  /// @brief PerView / PerFrame 定数バッファを更新する（毎フレーム呼ぶ）
  /// @param view      ビュー行列
  /// @param proj      プロジェクション行列
  /// @param deltaTime 経過時間（秒）
  void Update(const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj, float deltaTime);

  /// @brief 射出・更新の CS を実行し、描画コマンドを積む
  /// @param ctx シーンコンテキスト（再生中でなければ CS は実行しない）
  /// @param cl  コマンドリスト
  /// @note プレビューモードでは cl へ直接描画し、それ以外は 3D 描画キューへ積む。
  void Render(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief ImGui のデバッグ UI を描画する
  void DrawImGui();

  /// @brief 毎フレームの射出数を取得する
  /// @return 1 フレームに射出するパーティクル数
  uint32_t GetEmitCount() const { return emitCount_; }

  /// @brief 毎フレームの射出数を設定する
  /// @param count 1 フレームに射出するパーティクル数
  void SetEmitCount(uint32_t count) { emitCount_ = count; }

  /// @brief パーティクルタイプを設定する
  /// @param type 新しいパーティクルタイプ
  /// @note タイプが変わったときは FreeList を初期化し直す。
  void SetParticleType(ParticleType type);

  /// @brief 現在のパーティクルタイプを取得する
  /// @return 現在のパーティクルタイプ
  ParticleType GetParticleType() const { return currentType_; }

  /// @brief 最大パーティクル数を設定する
  /// @param maxCount 新しい最大パーティクル数
  /// @note 初期化済みならバッファを作り直す。
  void SetMaxParticles(uint32_t maxCount);

  /// @brief 現在の最大パーティクル数を取得する
  /// @return 最大パーティクル数
  uint32_t GetMaxParticles() const { return maxParticles_; }

  /// @brief ブレンドモードを取得する
  /// @return 描画時のブレンドモード
  BlendMode GetBlendMode() const { return blendMode_; }

  /// @brief ブレンドモードを設定する
  /// @param mode 描画時のブレンドモード
  void SetBlendMode(BlendMode mode) { blendMode_ = mode; }

  /// @brief テクスチャを動的に変更する
  /// @param path テクスチャファイルのパス
  void SetTexture(const std::string& path);

  /// @brief パーティクル設定を JSON ファイルに保存する
  /// @param filepath 保存先のファイルパス
  void SaveToJson(const std::string& filepath) const;

  /// @brief JSON ファイルからパーティクル設定を読み込む
  /// @param filepath 読み込み元のファイルパス
  void LoadFromJson(const std::string& filepath);

  /// @brief プレビューモードを設定する（Render 時に即時描画を行うか）
  /// @param isPreview true なら Render で直接描画する（深度なしパイプラインを使う）
  void SetPreviewMode(bool isPreview) { isPreview_ = isPreview; }

  /// @brief 使用するグラフィックスパイプラインのプレフィックスを設定する
  /// @param prefix パイプラインキーのプレフィックス（例: "gpu_particle"）
  void SetPipelinePrefix(const std::string& prefix) { pipelinePrefix_ = prefix; }

  /// @brief 使用するグラフィックスパイプラインのプレフィックスを取得する
  /// @return パイプラインキーのプレフィックス
  const std::string& GetPipelinePrefix() const { return pipelinePrefix_; }

  /// @brief 「モデルにまとわりつく電撃」のプリセットを適用する
  /// @details タイプ (Electric) / パイプライン (gpu_particle_electric) / 加算合成 /
  ///          寿命・スケール・速度・色をまとめて電撃向けの値にする。
  ///          殻の大きさは emitterShape_ == Box のとき shapeBoxSize_ を「直径」とした楕円体、
  ///          それ以外は shapeRadius_ の球。emitterOffset_ と合わせて対象モデルに合わせること。
  ///          既定値は足元ピボット・高さ約 1.7m の Player モデル向け。
  void ApplyElectricPreset();

  // --- パーティクルエディタ用パラメータ ---
  float minLifeTime_ = 3.0f;     ///< 最小寿命（秒）
  float maxLifeTime_ = 8.0f;     ///< 最大寿命（秒）
  float minScale_ = 0.3f;        ///< 最小スケール
  float maxScale_ = 0.6f;        ///< 最大スケール
  float gravity_ = 0.0f;         ///< 重力
  RC::Vector3 baseVelocity_ = {0.0f, 0.02f, 0.0f}; ///< 基本速度
  float velocityVariance_ = 0.02f; ///< 速度の分散
  EmitterShape emitterShape_ = EmitterShape::Point; ///< エミッタ形状
  float shapeRadius_ = 2.0f;     ///< 形状の半径
  float coneAngle_ = 0.5f;       ///< コーンの半角（rad）
  RC::Vector3 shapeBoxSize_ = {4.0f, 4.0f, 4.0f}; ///< Box 形状のサイズ
  RC::Vector4 startColor_ = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 開始色
  RC::Vector4 endColor_ = {1.0f, 1.0f, 1.0f, 0.0f};   ///< 終了色
  RC::Vector3 emitterPosition_ = {0.0f, 0.0f, 0.0f};  ///< エミッタ位置
  /// @brief エミッタ位置に足すオフセット（ワールド座標）
  /// @details シーン側は毎フレーム emitterPosition_ をエンティティの Transform で上書きするため、
  ///          「足元ピボットのモデルの胸の高さから出したい」といった調整はここで行う。
  ///          GPU に渡す位置は emitterPosition_ + emitterOffset_。
  RC::Vector3 emitterOffset_ = {0.0f, 0.0f, 0.0f};

  // --- 殻（ParticleType::Electric 専用。CB では shapePad.xy / shapeBoxPad の枠を使う）---
  /// @brief 殻の角の丸み（0 = 角のある箱、1 = 最短辺いっぱいまで丸める → カプセル／球）。Box 形状のときだけ有効
  float shellRoundness_ = 1.0f;
  /// @brief 殻をモデル表面から外側へ浮かせる距離（m）。0 だと表面と重なって半分が埋まって見える
  float shellMargin_ = 0.04f;
  /// @brief 殻の Y 軸回転（rad）。Transform.rotation.y を入れると回転する箱にも殻が合う
  float shellYaw_ = 0.0f;

  std::string texturePath_ = "Resources/Particle/circle.png"; ///< テクスチャパス

private:
  /// @brief サイズを 256 バイト境界に切り上げる（定数バッファの配置要件）
  /// @param s 元のサイズ（バイト）
  /// @return 256 の倍数に切り上げたサイズ
  static constexpr uint32_t Align256(uint32_t s) { return (s + 255u) & ~255u; }
  static constexpr uint32_t kParticleTypeCount = static_cast<uint32_t>(ParticleType::Count); ///< パーティクルタイプの数

  /// @brief パーティクルタイプごとの Compute Shader セット
  struct ParticleCSSet {
    ComputeShader emit;    ///< 射出用 CS
    ComputeShader update;  ///< 更新用 CS
    bool ready = false;    ///< 両方の CS が初期化済みか
  };

  // パーティクルバッファ（DEFAULT ヒープ、UAV 対応）
  Microsoft::WRL::ComPtr<ID3D12Resource> particleBuffer_; ///< ParticleCS × maxParticles_
  SRVManager::Handle uavHandle_{};  ///< CS 書き込み用 UAV（u0）
  SRVManager::Handle srvHandle_{};  ///< VS 読み取り用 SRV

  // FreeList バッファ（DEFAULT ヒープ、UAV 対応）
  Microsoft::WRL::ComPtr<ID3D12Resource> freeListBuffer_;      ///< uint32_t × maxParticles_
  SRVManager::Handle freeListUavHandle_{};                     ///< FreeList の UAV（u2）

  // FreeListIndex バッファ（DEFAULT ヒープ、UAV 対応）
  Microsoft::WRL::ComPtr<ID3D12Resource> freeListIndexBuffer_; ///< int32_t × 1
  SRVManager::Handle freeListIndexUavHandle_{};                ///< FreeList の先頭インデックスの UAV（u1）

  // PerView 定数バッファ（UPLOAD ヒープ）
  /// @details 値は CPU 側。バインド時に今フレームの領域へ送る（CPU と GPU を並行させるため）
  RC::DynamicCB<GPUParticlePerView> perViewCB_;
  GPUParticlePerView *perViewMapped_ = nullptr; ///< 書き込み先（= perViewCB_.Ptr()）

  // 板ポリ頂点バッファ
  Microsoft::WRL::ComPtr<ID3D12Resource> vbResource_; ///< クワッドの頂点バッファ（UPLOAD ヒープ）
  D3D12_VERTEX_BUFFER_VIEW vbView_{};                 ///< 頂点バッファビュー
  uint32_t vertexCount_ = 0;                          ///< 頂点数（クワッドなので 6）

  // テクスチャ
  int texHandle_ = -1; ///< テクスチャハンドル（未読み込みなら -1）

  // Compute Shader: 初期化用（タイプ共通）
  ComputeShader initCS_; ///< パーティクルと FreeList を初期化する CS

  // Compute Shader: タイプ別 Emit / Update セット
  std::array<ParticleCSSet, kParticleTypeCount> csSets_{}; ///< ParticleType をインデックスにした CS セット
  ParticleType currentType_ = ParticleType::Default;      ///< 現在のパーティクルタイプ

  // PerFrame 定数バッファ（deltaTime 用、UPLOAD ヒープ）
  /// @details 値は CPU 側。バインド時に今フレームの領域へ送る（CPU と GPU を並行させるため）
  RC::DynamicCB<GPUParticlePerFrame> perFrameCB_;
  GPUParticlePerFrame *perFrameMapped_ = nullptr; ///< 書き込み先（= perFrameCB_.Ptr()）

  // ブレンドモード
  BlendMode blendMode_ = kBlendModeAdd; ///< 描画時のブレンドモード

  // 射出設定
  uint32_t emitCount_ = 10;  ///< 毎フレーム射出するパーティクル数

  // 参照保持
  Microsoft::WRL::ComPtr<ID3D12Device> device_;    ///< D3D12 デバイス
  SRVManager *srvMgr_ = nullptr;                    ///< ディスクリプタの確保先（非所有）
  DeferredReleaseQueue *deferredRelease_ = nullptr; ///< 遅延解放キュー（非所有）

  bool initialized_ = false;  ///< Initialize 済みか
  bool visible_ = true;       ///< 描画・更新するか（ImGui から切り替える）
  bool isPreview_ = false;    ///< プレビューモードか（true なら即時描画）
  bool needsCSInit_ = false;  ///< CS 初期化を初回フレームに遅延実行するフラグ
  std::string pipelinePrefix_ = "gpu_particle"; ///< パイプラインのプレフィックス

  uint32_t maxParticles_ = 1024; ///< 現在の最大パーティクル数

  /// @brief パーティクルバッファ・FreeList と UAV / SRV を作り直す（最大数の変更時）
  /// @note 古いリソースとディスクリプタは GPU が使い終わってから解放する。
  void rebuildBuffers_();
};
