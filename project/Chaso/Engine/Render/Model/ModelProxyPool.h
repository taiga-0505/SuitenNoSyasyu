#pragma once

// ============================================================================
// ModelProxyPool（VirtualEntity）
// ----------------------------------------------------------------------------
// 「見た目だけを持つ軽いオブジェクト」を大量に置くためのプール。
//
//  - Entity（コンポーネントを unordered_map で持つ重いオブジェクト）を作らずに、
//    モデルハンドル + ワールド行列 + 色 だけを SoA（配列ごと）で持つ。
//  - ハンドルは (添え字, 世代番号)。破棄済みスロットを再利用しても、古いハンドルは
//    世代番号が合わないので無効と判定できる（ダングリング参照を防ぐ）。
//  - 行列の逆転置・ワールド境界箱・BVH の更新は「変更されたスロット（dirty）」だけ行う。
//    動かない物は登録後、毎フレームの CPU コストがほぼゼロ。
//  - カリングは DynamicBVH を視錐台で辿る。完全に内側の部分木は丸ごと採用する。
//
// 使い方（ゲーム側は RenderCommon.h の RC::CreateModelProxy などを使う）:
//   int rock = RC::LoadModel("rock.obj");          // 見た目のテンプレート（1 個だけ）
//   auto h = RC::CreateModelProxy(rock);           // 1 万個作っても Entity は増えない
//   RC::SetModelProxyTransform(h, transform);
//   ...描画パスごとに RC::DrawModelProxies();       // カリング＋インスタンス描画
// ============================================================================

#include "Math/BoundingBox.h"
#include "Math/DynamicBVH.h"
#include "Math/MathTypes.h"
#include "struct.h"
#include <cstdint>
#include <vector>

namespace RC {

class ModelManager;

/// @brief ModelProxyPool のハンドル（VirtualEntity）
struct ModelProxyHandle {
  uint32_t index = UINT32_MAX; ///< スロット番号
  uint32_t generation = 0;     ///< 世代番号（スロット再利用の検出用）

  /// @brief 一度でも作られたハンドルか（生存しているかは ModelProxyPool::IsAlive で確認）
  bool IsValid() const { return index != UINT32_MAX; }
  bool operator==(const ModelProxyHandle &o) const {
    return index == o.index && generation == o.generation;
  }
};

/// @brief 大量の静的／準静的モデルを SoA で保持するプール
class ModelProxyPool {
public:
  /// @brief プロキシを作る
  /// @param modelHandle 見た目のテンプレート（RC::LoadModel のハンドル）。メッシュ・テクスチャ・マテリアルを借りる
  /// @return ハンドル
  ModelProxyHandle Create(int modelHandle);

  /// @brief プロキシを破棄する（無効なハンドルなら何もしない）
  void Destroy(ModelProxyHandle h);

  /// @brief 生存しているハンドルか
  bool IsAlive(ModelProxyHandle h) const {
    return h.index < generation_.size() && generation_[h.index] == h.generation &&
           (flags_[h.index] & kAlive) != 0;
  }

  /// @brief ワールド行列を設定する（行ベクトル規約）
  void SetWorld(ModelProxyHandle h, const Matrix4x4 &world);
  /// @brief Transform（SRT）からワールド行列を設定する
  void SetTransform(ModelProxyHandle h, const Transform &t);
  /// @brief 乗算カラーを設定する（テンプレートのマテリアル色にさらに掛かる）
  void SetColor(ModelProxyHandle h, const Vector4 &color);
  /// @brief 表示／非表示
  void SetVisible(ModelProxyHandle h, bool visible);
  /// @brief 影を落とすか
  void SetCastShadow(ModelProxyHandle h, bool cast);

  /// @brief 生存数
  uint32_t Count() const { return aliveCount_; }

  /// @brief 全削除
  void Clear();

  /// @brief 変更されたスロットの逆転置行列・境界箱・BVH を更新する
  /// @details テンプレートのメッシュが未ロードのスロットは境界箱が決まらないので
  ///          次回へ持ち越す（ロードが終わるまで描かれない）。
  void UpdateDirty(ModelManager &models);

  /// @brief 視錐台にかかるスロットを列挙する（保守的）
  /// @param shadowPass true なら castShadow の物だけ
  /// @param cb void(uint32_t slot)
  template <class Callback>
  void QueryVisible(const Frustum &frustum, bool shadowPass, Callback &&cb) const {
    const uint8_t need = static_cast<uint8_t>(kAlive | kVisible | (shadowPass ? kCastShadow : 0));
    bvh_.QueryFrustum(frustum, [&](int32_t proxy) {
      const uint32_t slot = bvh_.GetUserData(proxy);
      if ((flags_[slot] & need) == need) cb(slot);
    });
  }

  // ── SoA の読み出し（slot は QueryVisible が返した番号） ──
  int ModelHandle(uint32_t slot) const { return modelHandle_[slot]; }
  const Matrix4x4 &World(uint32_t slot) const { return world_[slot]; }
  /// @brief 法線用の逆転置行列。行列が変わった後、最初に要求されたときだけ計算する
  /// @details 毎フレーム全プロキシを動かしても、逆行列を計算するのはカメラに映った物だけで済む
  ///          （影パスは World しか使わないので要求しない）。
  const Matrix4x4 &WorldInverseTranspose(uint32_t slot) {
    if ((flags_[slot] & kWitValid) == 0) {
      worldInvTranspose_[slot] = ComputeInverseTranspose_(world_[slot]);
      flags_[slot] |= kWitValid;
    }
    return worldInvTranspose_[slot];
  }
  const Vector4 &Color(uint32_t slot) const { return color_[slot]; }

  /// @brief BVH（デバッグ表示・検証用）
  const DynamicBVH &Bvh() const { return bvh_; }

private:
  enum Flag : uint8_t {
    kAlive = 1 << 0,      ///< 使用中
    kVisible = 1 << 1,    ///< 表示する
    kCastShadow = 1 << 2, ///< 影を落とす
    kDirty = 1 << 3,      ///< 行列が変わった（UpdateDirty 待ち）
    kWitValid = 1 << 4,   ///< worldInvTranspose_ が world_ に対して計算済み
  };

  void MarkDirty_(uint32_t slot);
  static Matrix4x4 ComputeInverseTranspose_(const Matrix4x4 &w);

  // SoA（スロット番号で揃える）
  std::vector<Matrix4x4> world_;             ///< ワールド行列
  std::vector<Matrix4x4> worldInvTranspose_; ///< 法線用の逆転置行列（必要になったときに計算）
  std::vector<Vector4> color_;               ///< 乗算カラー
  std::vector<int32_t> modelHandle_;         ///< テンプレートのモデルハンドル
  std::vector<int32_t> bvhProxy_;            ///< BVH の葉（未登録は kNullNode）
  std::vector<uint32_t> generation_;         ///< 世代番号
  std::vector<uint8_t> flags_;               ///< Flag の組み合わせ

  std::vector<uint32_t> freeSlots_;          ///< 空きスロット
  std::vector<uint32_t> dirtySlots_;         ///< UpdateDirty 待ち
  std::vector<uint32_t> pendingSlots_;       ///< メッシュ未ロードで持ち越し中（UpdateDirty の作業用）

  /// @brief 葉を太らせる量 0.25m、移動量の先読み 4 倍
  /// @details 小さく揺れ続ける物（風に揺れる草木、浮き沈みする物など）が毎フレーム
  ///          fat AABB をはみ出して木を組み替えないよう、やや大きめに取る。
  ///          太らせた分だけカリングが甘くなる（余分に描く）が、取りこぼしは起きない。
  DynamicBVH bvh_{0.25f, 4.0f};
  uint32_t aliveCount_ = 0;
};

} // namespace RC
