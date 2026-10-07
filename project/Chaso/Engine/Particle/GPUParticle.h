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
  Count
};

/// @class GPUParticle
/// @brief GPU 上で初期化・更新・描画を行うパーティクルシステム（FreeList 方式）
/// @details Compute Shader でパーティクルデータを管理し、
/// FreeList で寿命切れパーティクルを再利用する。
/// EmitCS で射出、UpdateCS で更新、VS で billboard 描画。
/// パーティクルタイプを切り替えることで、Emit/Update の CS を動的に差し替え可能。
class GPUParticle {
public:
  GPUParticle() = default;
  ~GPUParticle();

  // コピー禁止
  GPUParticle(const GPUParticle &) = delete;
  GPUParticle &operator=(const GPUParticle &) = delete;

  /// @brief 初期化（リソース作成、CS による初期化実行）
  void Initialize(SceneContext &ctx);

  /// @brief 終了処理
  void Finalize();

  /// @brief フレーム更新（PerView / PerFrame 定数バッファの更新）
  void Update(const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj, float deltaTime);

  /// @brief 描画処理
  void Render(SceneContext &ctx, ID3D12GraphicsCommandList *cl);

  /// @brief ImGui デバッグ UI
  void DrawImGui();

  /// @brief 毎フレームの射出数を取得
  uint32_t GetEmitCount() const { return emitCount_; }

  /// @brief 毎フレームの射出数を設定
  void SetEmitCount(uint32_t count) { emitCount_ = count; }

  /// @brief パーティクルタイプを設定する
  /// @param type 新しいパーティクルタイプ
  void SetParticleType(ParticleType type);

  /// @brief 現在のパーティクルタイプを取得する
  ParticleType GetParticleType() const { return currentType_; }

  /// @brief 最大パーティクル数を設定する
  /// @param maxCount 新しい最大パーティクル数
  void SetMaxParticles(uint32_t maxCount);

  /// @brief 現在の最大パーティクル数を取得する
  uint32_t GetMaxParticles() const { return maxParticles_; }

  /// @brief ブレンドモードを取得する
  BlendMode GetBlendMode() const { return blendMode_; }

  /// @brief ブレンドモードを設定する
  void SetBlendMode(BlendMode mode) { blendMode_ = mode; }

  /// @brief テクスチャを動的に変更する
  /// @param path テクスチャファイルのパス
  void SetTexture(const std::string& path);

  /// @brief パーティクル設定をJSONファイルに保存する
  /// @param filepath 保存先のファイルパス
  void SaveToJson(const std::string& filepath) const;

  /// @brief JSONファイルからパーティクル設定を読み込む
  /// @param filepath 読み込み元のファイルパス
  void LoadFromJson(const std::string& filepath);

  /// @brief プレビューモードを設定する（Render 時に即時描画を行うか）
  void SetPreviewMode(bool isPreview) { isPreview_ = isPreview; }

  /// @brief 使用するグラフィックスパイプラインのプレフィックスを設定する
  void SetPipelinePrefix(const std::string& prefix) { pipelinePrefix_ = prefix; }
  const std::string& GetPipelinePrefix() const { return pipelinePrefix_; }

  /// @brief 「モデルにまとわりつく電撃」のプリセットを適用する
  /// @details タイプ (Electric) / パイプライン (gpu_particle_electric) / 加算合成 /
  ///          寿命・スケール・速度・色をまとめて電撃向けの値にする。
  ///          殻の大きさは emitterShape_ == Box のとき shapeBoxSize_ を「直径」とした楕円体、
  ///          それ以外は shapeRadius_ の球。emitterOffset_ と合わせて対象モデルに合わせること。
  ///          既定値は足元ピボット・高さ約 1.7m の Player モデル向け。
  void ApplyElectricPreset();

  // --- Particle Editor 用パラメータ ---
  float minLifeTime_ = 3.0f;     ///< 最小寿命
  float maxLifeTime_ = 8.0f;     ///< 最大寿命
  float minScale_ = 0.3f;        ///< 最小スケール
  float maxScale_ = 0.6f;        ///< 最大スケール
  float gravity_ = 0.0f;         ///< 重力
  RC::Vector3 baseVelocity_ = {0.0f, 0.02f, 0.0f}; ///< 基本速度
  float velocityVariance_ = 0.02f; ///< 速度の分散
  EmitterShape emitterShape_ = EmitterShape::Point; ///< エミッタ形状
  float shapeRadius_ = 2.0f;     ///< 形状の半径
  float coneAngle_ = 0.5f;       ///< コーンの半角 (rad)
  RC::Vector3 shapeBoxSize_ = {4.0f, 4.0f, 4.0f}; ///< Box形状のサイズ
  RC::Vector4 startColor_ = {1.0f, 1.0f, 1.0f, 1.0f}; ///< 開始色
  RC::Vector4 endColor_ = {1.0f, 1.0f, 1.0f, 0.0f};   ///< 終了色
  RC::Vector3 emitterPosition_ = {0.0f, 0.0f, 0.0f};  ///< エミッタ位置
  /// @brief エミッタ位置に足すオフセット（ワールド座標）
  /// @details シーン側は毎フレーム emitterPosition_ をエンティティの Transform で上書きするため、
  ///          「足元ピボットのモデルの胸の高さから出したい」といった調整はここで行う。
  ///          GPU に渡す位置は emitterPosition_ + emitterOffset_。
  RC::Vector3 emitterOffset_ = {0.0f, 0.0f, 0.0f};

  // --- 殻（ParticleType::Electric 専用。CB では shapePad.xy / shapeBoxPad の枠を使う）---
  /// @brief 殻の角の丸み (0=角のある箱, 1=最短辺いっぱいまで丸める → カプセル/球)。Box 形状のときだけ有効
  float shellRoundness_ = 1.0f;
  /// @brief 殻をモデル表面から外側へ浮かせる距離 (m)。0 だと表面と重なって半分が埋まって見える
  float shellMargin_ = 0.04f;
  /// @brief 殻の Y 軸回転 (rad)。Transform.rotation.y を入れると回転する箱にも殻が合う
  float shellYaw_ = 0.0f;

  std::string texturePath_ = "Resources/Particle/circle.png"; ///< テクスチャパス

private:
  static constexpr uint32_t Align256(uint32_t s) { return (s + 255u) & ~255u; }
  static constexpr uint32_t kParticleTypeCount = static_cast<uint32_t>(ParticleType::Count);

  /// @brief パーティクルタイプごとの Compute Shader セット
  struct ParticleCSSet {
    ComputeShader emit;    ///< 射出用 CS
    ComputeShader update;  ///< 更新用 CS
    bool ready = false;    ///< 両方の CS が初期化済みか
  };

  // パーティクルバッファ（DEFAULT ヒープ、UAV 対応）
  Microsoft::WRL::ComPtr<ID3D12Resource> particleBuffer_;
  SRVManager::Handle uavHandle_{};  ///< CS 書き込み用 UAV (u0)
  SRVManager::Handle srvHandle_{};  ///< VS 読み取り用 SRV

  // FreeList バッファ（DEFAULT ヒープ、UAV 対応）
  Microsoft::WRL::ComPtr<ID3D12Resource> freeListBuffer_;      ///< uint32_t × kMaxParticles
  SRVManager::Handle freeListUavHandle_{};                     ///< UAV (u2)

  // FreeListIndex バッファ（DEFAULT ヒープ、UAV 対応）
  Microsoft::WRL::ComPtr<ID3D12Resource> freeListIndexBuffer_; ///< int32_t × 1
  SRVManager::Handle freeListIndexUavHandle_{};                ///< UAV (u1)

  // PerView 定数バッファ（UPLOAD ヒープ）
  /// @details 値は CPU 側。バインド時に今フレームの領域へ送る（CPU と GPU を並行させるため）
  RC::DynamicCB<GPUParticlePerView> perViewCB_;
  GPUParticlePerView *perViewMapped_ = nullptr; ///< 書き込み先（= perViewCB_.Ptr()）

  // 板ポリ頂点バッファ
  Microsoft::WRL::ComPtr<ID3D12Resource> vbResource_;
  D3D12_VERTEX_BUFFER_VIEW vbView_{};
  uint32_t vertexCount_ = 0;

  // テクスチャ
  int texHandle_ = -1;

  // Compute Shader: 初期化用（タイプ共通）
  ComputeShader initCS_;

  // Compute Shader: タイプ別 Emit/Update セット
  std::array<ParticleCSSet, kParticleTypeCount> csSets_{};
  ParticleType currentType_ = ParticleType::Default;

  // PerFrame 定数バッファ（deltaTime 用、UPLOAD ヒープ）
  /// @details 値は CPU 側。バインド時に今フレームの領域へ送る（CPU と GPU を並行させるため）
  RC::DynamicCB<GPUParticlePerFrame> perFrameCB_;
  GPUParticlePerFrame *perFrameMapped_ = nullptr; ///< 書き込み先（= perFrameCB_.Ptr()）

  // ブレンドモード
  BlendMode blendMode_ = kBlendModeAdd;

  // 射出設定
  uint32_t emitCount_ = 10;  ///< 毎フレーム射出するパーティクル数

  // 参照保持
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  SRVManager *srvMgr_ = nullptr;
  DeferredReleaseQueue *deferredRelease_ = nullptr; ///< 遅延解放キュー（非所有）

  bool initialized_ = false;
  bool visible_ = true;
  bool isPreview_ = false;    ///< プレビューモードか（trueなら即時描画）
  bool needsCSInit_ = false;  ///< CS 初期化を初回フレームに遅延実行するフラグ
  std::string pipelinePrefix_ = "gpu_particle"; ///< パイプラインのプレフィックス

  uint32_t maxParticles_ = 1024; ///< 現在の最大パーティクル数

  /// @brief パーティクルバッファ類の再構築（最大数変更時）
  void rebuildBuffers_();
};
