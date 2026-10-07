#pragma once

// ============================================================================
// EffectParticleSystem
// ----------------------------------------------------------------------------
// 水しぶき・泡・水柱などの「短命で数が多い見た目だけの粒」を、Entity を使わずに扱う。
//
// 以前は 1 粒ごとに
//   Entity + TransformComponent + PrimitiveMeshComponent（専用の球メッシュ）+ NativeScript
// を作っていたため、弾を撃つと数百個の Entity が増え、
//   - シーン更新の全エンティティ走査（コンポーネント検索）
//   - 1 粒 2〜3 回の描画コマンド（水の 2 パス描画＋影）
//   - エディタの Hierarchy の行
// がすべて粒の数に比例して重くなっていた。
//
// ここでは粒を配列で持ち、
//   - 更新: 1 回のループ（UpdateEffectParticles）
//   - 描画: 種類ごとに共有メッシュ 1 つでインスタンス描画（水は奥→手前に並べる）
// にする。粒が増えても描画コマンド数は「種類の数 × パス数」で一定。
//
// 動き（寿命・初速・重力・縮み方・透明度）は旧スクリプト
// （Application/Game/Scripts/SplashParticle.cpp の SplashParticle / WakeParticle /
//   HeavySplashParticle / BubbleParticle）と同じ式にしてある。
// ============================================================================

#include "Math/BoundingBox.h"
#include "Math/MathTypes.h"
#include "struct.h"
#include <cstdint>
#include <vector>

namespace RC {

class RenderContext;

/// @brief エフェクト粒の種類
enum class EffectParticleKind : uint8_t {
  Splash,      ///< 水しぶき（球・水の質感）。外側上方へ飛び、重力で落ちながら縮んで消える
  Wake,        ///< 航跡の飛沫（球・水の質感）。上へ跳ねて水面で小さくバウンドする
  HeavySplash, ///< 水柱（円柱・水柱の質感）。一気に伸びて、終盤に薄くなって消える
  Bubble,      ///< 泡（球・不透明）。揺れながらゆっくり上がり、最後にはじける
};

/// @brief 粒を 1 個出すときの指定
struct EffectParticleSpawn {
  EffectParticleKind kind = EffectParticleKind::Splash;
  Vector3 position{0.0f, 0.0f, 0.0f}; ///< 出現位置（HeavySplash は水柱の根元）
  float scale = 0.2f;                 ///< 初期サイズ（球の半径 / 水柱の太さ）
  Vector4 color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 色（α は種類ごとの式で上書きされる）
  float impactFactor = 1.0f;          ///< 勢い（Splash の初速・HeavySplash の高さに掛かる）
};

/// @brief Entity を使わないエフェクト粒の管理と描画（RenderContext が 1 つ持つ）
class EffectParticleSystem {
public:
  /// @brief フレーム単位の統計（デバッグ表示用）
  struct Stats {
    uint32_t alive = 0;     ///< 生きている粒の数
    uint32_t drawn = 0;     ///< 描いた粒の数（全パス合計）
    uint32_t drawCalls = 0; ///< 発行したドローコール数（全パス合計）
  };

  /// @brief 同時に存在できる粒の上限（超えた分は出さない）
  static constexpr uint32_t kMaxParticles = 8192;

  /// @brief 粒を 1 個出す
  void Spawn(const EffectParticleSpawn &desc);

  /// @brief 全粒を進める（dt = 0 なら止まったまま）
  void Update(float dt);

  /// @brief このパスでエフェクト粒を描く（Execute3DCommands の先頭で Flush される）
  void RequestDraw() { drawRequested_ = true; }

  /// @brief Flush すべき要求があるか
  bool HasPending() const { return drawRequested_; }

  /// @brief 現在のパスの視錐台でカリングし、種類ごとにインスタンス描画のコマンドを積む
  void Flush(RenderContext &ctx);

  /// @brief 全粒を消す（シーン切り替え時など）
  void Clear();

  /// @brief 共有メッシュも含めて解放する（終了処理用）
  void Term(RenderContext &ctx);

  /// @brief フレーム開始（統計を前フレーム分として確定）
  void BeginFrame();

  /// @brief 生きている粒の数
  uint32_t Count() const { return static_cast<uint32_t>(particles_.size()); }

  /// @brief 前フレームの統計
  const Stats &LastFrameStats() const { return lastStats_; }

private:
  /// @brief 1 粒
  struct Particle {
    Vector3 pos{0.0f, 0.0f, 0.0f};
    Vector3 vel{0.0f, 0.0f, 0.0f};
    Vector3 scale{1.0f, 1.0f, 1.0f}; ///< 現在の拡大率（描画用）
    Vector4 color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 現在の色（α 込み）
    float elapsed = 0.0f;
    float lifetime = 1.0f;
    float startScale = 1.0f;
    float startY = 0.0f;     ///< HeavySplash: 根元の高さ
    float maxHeight = 15.0f; ///< HeavySplash: 伸びきったときの高さ
    EffectParticleKind kind = EffectParticleKind::Splash;
  };

  /// @brief 描画の種類（共有メッシュ × 描き方）
  enum class DrawGroup : uint8_t {
    Water,       ///< Splash / Wake（球・水の 2 パス描画）
    WaterColumn, ///< HeavySplash（円柱・水柱の 2 パス描画）
    Bubble,      ///< Bubble（球・不透明）
    Count,
  };

  /// @brief 種類ごとの更新。死んだら false
  static bool Step_(Particle &p, float dt);
  static DrawGroup GroupOf_(EffectParticleKind k);

  /// @brief 共有メッシュ（球・円柱）を用意する。作れなければ false
  bool EnsureMeshes_(RenderContext &ctx);

  /// @brief 1 グループ分のインスタンスデータを書いて描画コマンドを積む
  void EmitGroup_(RenderContext &ctx, DrawGroup group, const std::vector<uint32_t> &indices,
                  bool shadowPass);

  std::vector<Particle> particles_;
  bool drawRequested_ = false;
  float time_ = 0.0f;          ///< 経過時間（水柱の UV スクロール用）
  int sphereMesh_ = -1;        ///< 共有の球メッシュ（PrimitiveMeshManager のハンドル）
  int cylinderMesh_ = -1;      ///< 共有の円柱メッシュ

  // 作業用（毎パス clear して使い回す）
  std::vector<uint32_t> groupIndices_[static_cast<size_t>(DrawGroup::Count)];
  std::vector<float> sortKeys_;

  Stats stats_{};
  Stats lastStats_{};
};

} // namespace RC
