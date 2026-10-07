#pragma once

// ============================================================================
// ModelInstanceBatcher
// ----------------------------------------------------------------------------
// 「同じメッシュ・同じ見た目」のモデル描画要求を、描画パスごとに
//   1) 視錐台カリング（ワールド境界箱 vs そのパスの ViewProjection）
//   2) まとめ（メッシュ・テクスチャ・マテリアル設定・ライトが一致するもの）
//   3) インスタンス描画（1 バッチ = DrawItem 数ぶんの DrawIndexedInstanced）
// に変換する。
//
// 以前の RC::DrawModel は 1 オブジェクトごとに std::function のコマンドを積み、
// 定数バッファを 256byte 確保して DrawIndexedInstanced(..., 1, ...) を 1 回呼んでいた。
// 影パスは最大 34 回あるので、1 万個なら最悪 34 万ドローになっていた。
// これを「バッチ数 × DrawItem 数」まで減らす。
//
// 流れ:
//   RC::DrawModelInstanced(handle, tex) ─┐  Submit（要求を溜めるだけ）
//   RC::DrawModelProxies()              ─┤  RequestProxies（VirtualEntity を描く印）
//                                         ▼
//   RenderContext::Execute3DCommands の先頭で Flush（そのパスの行列でカリング → まとめ → コマンド化）
//
// 制約（当てはまらない物は RC::DrawModelInstanced が自動で従来の DrawModel に回す）:
//   - 不透明のみ（マテリアル色 α < 1、ブレンドモード指定中は従来経路）
//   - スキニングモデルは対象外
//   - プロキシ（VirtualEntity）は不透明・非スキニングのテンプレートだけ描く
// ============================================================================

#include "Math/BoundingBox.h"
#include "Math/MathTypes.h"
#include "struct.h"
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

class ModelObject;
class ModelMesh;

namespace RC {

class RenderContext;
class ModelProxyPool;

/// @brief モデルのインスタンス描画のまとめ役（RenderContext が 1 つ持つ）
class ModelInstanceBatcher {
public:
  /// @brief フレーム単位の統計（デバッグ表示用）
  struct Stats {
    uint32_t submitted = 0;      ///< 受け付けた描画要求（エンティティ＋プロキシ）
    uint32_t culled = 0;         ///< 視錐台カリングで捨てた数
    uint32_t instancesDrawn = 0; ///< 実際に描いたインスタンス数
    uint32_t batches = 0;        ///< 作ったバッチ数（≒ PSO/マテリアル切り替え回数）
    uint32_t drawCalls = 0;      ///< 発行した DrawIndexedInstanced の数
    uint32_t overflowSkipped = 0; ///< フレームリソースが足りずに描けなかったインスタンス数
    uint32_t proxyTotal = 0;      ///< 生存している VirtualEntity の数
    uint32_t proxyMainVisible = 0; ///< メインパス（カメラ）で視錐台に入った VirtualEntity の数
    float flushMs = 0.0f;         ///< Flush（カリング＋まとめ＋インスタンスデータ書き込み）の CPU 時間の合計
  };

  /// @brief モデル 1 個の描画要求を受け付ける（このパスの Flush まで溜める）
  /// @param m 描くモデル（DrawModelInstanced が条件を確認済み）
  /// @param modelHandle デバッグ表示用のハンドル
  /// @param texHandle テクスチャ上書き（-1 = mtl のテクスチャ）
  void Submit(::ModelObject *m, int modelHandle, int texHandle);

  /// @brief このパスで ModelProxyPool（VirtualEntity）を描く
  void RequestProxies() { proxiesRequested_ = true; }

  /// @brief Flush すべき要求があるか
  bool HasPending() const { return !submissions_.empty() || proxiesRequested_; }

  /// @brief 溜まった要求を、現在のパスの視錐台でカリングしてバッチ化し、3D コマンドとして積む
  /// @details RenderContext::Execute3DCommands の先頭から呼ばれる
  void Flush(RenderContext &ctx, ModelProxyPool &proxies);

  /// @brief フレーム開始（統計を前フレーム分として確定させる）
  void BeginFrame();

  /// @brief 前フレームの統計
  const Stats &LastFrameStats() const { return lastStats_; }

  /// @brief 溜まっている要求を捨てる（終了処理用）
  void Clear();

private:
  /// @brief 描画要求
  /// @details ワールド行列と色は「呼んだ時点」の値を控える（DrawModel がラムダに world を
  ///          キャプチャしていたのと同じ）。同じハンドルを位置を変えて何度も描いても正しく描ける。
  struct Submission {
    ::ModelObject *model = nullptr;
    int modelHandle = -1;
    int texHandle = -1;
    Matrix4x4 world{};             ///< 呼んだ時点のワールド行列
    Matrix4x4 worldInvTranspose{}; ///< 逆転置（Flush で必要になったら計算）
    Vector4 color{1.0f, 1.0f, 1.0f, 1.0f}; ///< 呼んだ時点のマテリアル色
  };

  /// @brief まとめ判定のキー（全フィールド一致で同じバッチ）
  struct BatchKey {
    const ::ModelMesh *mesh = nullptr; ///< 共有メッシュ
    uint64_t texture = 0;              ///< テクスチャ上書き SRV（0 = mtl）
    uint64_t normalMap = 0;            ///< 法線マップ SRV
    uint64_t roughnessMap = 0;         ///< ラフネスマップ SRV
    uint64_t lightCB = 0;              ///< ライト CB
    int32_t lightingMode = 0;          ///< 解決済みライティングモード
    float shininess = 0.0f;            ///< 光沢
    float environmentCoefficient = 0.0f; ///< 映り込み
    float uvTransform[16]{};           ///< UV 変換

    bool operator==(const BatchKey &o) const;
  };
  struct BatchKeyHash {
    size_t operator()(const BatchKey &k) const;
  };

  /// @brief バッチ内の 1 インスタンス
  struct InstanceRef {
    const Matrix4x4 *world = nullptr;             ///< ワールド行列
    const Matrix4x4 *worldInvTranspose = nullptr; ///< 逆転置（影パスでは nullptr）
    Vector4 color{1.0f, 1.0f, 1.0f, 1.0f};        ///< インスタンス色（マテリアル色込み）
    float depthSq = 0.0f;                         ///< カメラからの距離の 2 乗（手前から描く並べ替え用）
  };

  /// @brief 1 バッチ
  struct Batch {
    ::ModelObject *leader = nullptr; ///< テクスチャ・メッシュ・ライトを借りる代表
    int leaderHandle = -1;           ///< デバッグ表示用
    uint64_t textureOverride = 0;    ///< 描画直前に leader へ設定し直すテクスチャ上書き（0 = mtl）
    Material material{};             ///< このバッチ用のマテリアル（color は白）
    std::vector<InstanceRef> instances;
  };

  /// @brief キーを作る（モデル側の状態を DrawModel と同じ規則で解決してから）
  BatchKey MakeKey_(RenderContext &ctx, ::ModelObject &m, Material &outMaterial) const;

  /// @brief キーに対応するバッチを取得（無ければ作る）
  Batch &FindOrAddBatch_(const BatchKey &key, ::ModelObject *leader, int leaderHandle,
                         const Material &material);

  /// @brief 1 バッチ分のインスタンスデータを書き、描画コマンドを積む
  void EmitBatch_(RenderContext &ctx, Batch &batch, bool shadowPass, const Matrix4x4 &viewProj);

  /// @brief 影パスで使ってよい SRV 領域の下限（メインパス用に残しておく量）
  /// @details 影パスはメインパスより先に走るので、影で使い切るとメイン画面の物が消えてしまう。
  ///          8MB ≒ 4 万インスタンス分をメインパス用に確保しておく。
  static constexpr uint32_t kMainPassReserveBytes = 8u * 1024u * 1024u;

  std::vector<Submission> submissions_;
  bool proxiesRequested_ = false;

  // 作業用（毎パス clear して使い回す。capacity は保持されるので再確保が起きない）
  std::unordered_map<BatchKey, uint32_t, BatchKeyHash> batchLookup_;
  std::vector<Batch> batches_;
  uint32_t batchCount_ = 0;
  std::unordered_map<int, int32_t> proxyTemplateBatch_; ///< テンプレートのモデルハンドル → バッチ番号（-1 = 描かない）

  Stats stats_{};
  Stats lastStats_{};
};

} // namespace RC
