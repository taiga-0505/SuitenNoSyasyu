#pragma once
#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

#include "GraphicsPipeline/GraphicsPipeline.h" // BlendMode
#include "Math/Math.h"
#include "function/function.h" // CreateBufferResource

/// @class Primitive3D
/// @brief 3D 空間でのデバッグ用プリミティブ（ライン、グリッド、球、箱など）の描画を管理するクラス
/// @details フレームごとに頂点データを蓄積し、深度テストの有無を分けて一括描画する。
///          各種デバッグ形状をワールド座標で簡単に追加できるインターフェースを提供する。
class Primitive3D {
public:
  /// @brief コンストラクタ（GPU リソースは Initialize で作る）
  Primitive3D() = default;
  /// @brief デストラクタ（Term を呼んで GPU リソースを解放する）
  ~Primitive3D();

  /// @brief 定数バッファと初期頂点バッファを作成する
  /// @param device D3D12 デバイス
  /// @note 既にリソースがある場合は Term してから作り直す。
  void Initialize(ID3D12Device *device);

  /// @brief GPU リソースと蓄積済みの頂点を解放する
  void Term();

  /// @brief フレームの開始を宣言し、ビュー・プロジェクション行列をセットする
  /// @param view      ビュー行列
  /// @param proj      プロジェクション行列
  /// @param blendAt3D 描画時のブレンドモード
  /// @note 蓄積済みの頂点はここではクリアしない（前フレームの ImGui 描画中に追加された線を残すため）。
  void BeginFrame(const RC::Matrix4x4 &view, const RC::Matrix4x4 &proj,
                  BlendMode blendAt3D = kBlendModeNone);

  /// @brief ラインを追加する
  /// @param a     開始点
  /// @param b     終了点
  /// @param color 描画色
  /// @param depth 深度テストを行うか
  void AddLine(const RC::Vector3 &a, const RC::Vector3 &b,
               const RC::Vector4 &color, bool depth = true);

  /// @brief AABB（軸平行境界ボックス）を追加する
  /// @param mn    最小座標
  /// @param mx    最大座標
  /// @param color 描画色
  /// @param depth 深度テストを行うか
  void AddAABB(const RC::Vector3 &mn, const RC::Vector3 &mx,
               const RC::Vector4 &color, bool depth = true);

  /// @brief 原点を中心とした XZ 平面のグリッドを追加する
  /// @param halfSize 中心から端までの線の本数
  /// @param step     グリッドの間隔
  /// @param color    描画色
  /// @param depth    深度テストを行うか
  void AddGridXZ(int halfSize, float step, const RC::Vector4 &color,
                 bool depth = true);

  /// @brief 原点を中心とした XY 平面のグリッドを追加する
  /// @param halfSize 中心から端までの線の本数
  /// @param step     グリッドの間隔
  /// @param color    描画色
  /// @param depth    深度テストを行うか
  void AddGridXY(int halfSize, float step, const RC::Vector4 &color,
                 bool depth = true);

  /// @brief 原点を中心とした YZ 平面のグリッドを追加する
  /// @param halfSize 中心から端までの線の本数
  /// @param step     グリッドの間隔
  /// @param color    描画色
  /// @param depth    深度テストを行うか
  void AddGridYZ(int halfSize, float step, const RC::Vector4 &color,
                 bool depth = true);

  // --------------------------------------------------------------------------
  // 便利形状（デバッグ用）
  // --------------------------------------------------------------------------

  /// @brief ワイヤーフレーム球を追加する
  /// @param center 中心座標
  /// @param radius 半径（0 以下なら何もしない）
  /// @param color  描画色
  /// @param slices 経度分割数（3 未満は 3 に丸める）
  /// @param stacks 緯度分割数（2 未満は 2 に丸める）
  /// @param depth  深度テストを行うか
  void AddSphere(const RC::Vector3 &center, float radius,
                 const RC::Vector4 &color, int slices = 24, int stacks = 12,
                 bool depth = true);

  /// @brief 3 本の軸リングのみで構成される軽量な球を追加する
  /// @param center   中心座標
  /// @param radius   半径（0 以下なら何もしない）
  /// @param color    描画色
  /// @param segments 各リングの分割数（3 未満は 3 に丸める）
  /// @param depth    深度テストを行うか
  void AddSphereRings(const RC::Vector3 &center, float radius,
                      const RC::Vector4 &color, int segments = 32,
                      bool depth = true);

  /// @brief 円弧を追加する
  /// @param center       中心座標
  /// @param normal       円弧が乗る平面の法線
  /// @param fromDir      開始方向（法線成分は除去して使う）
  /// @param radius       半径（0 以下なら何もしない）
  /// @param startRad     開始角（ラジアン）
  /// @param endRad       終了角（ラジアン）
  /// @param color        描画色
  /// @param segments     分割数
  /// @param depth        深度テストを行うか
  /// @param drawToCenter 中心から端点への線を引いて扇形にするか
  void AddArc(const RC::Vector3 &center, const RC::Vector3 &normal,
              const RC::Vector3 &fromDir, float radius, float startRad,
              float endRad, const RC::Vector4 &color, int segments = 32,
              bool depth = true, bool drawToCenter = false);

  /// @brief ワイヤーカプセルを追加する
  /// @param p0       線分の一方の端点（球の中心）
  /// @param p1       線分のもう一方の端点（球の中心）
  /// @param radius   半径（0 以下なら何もしない）
  /// @param color    描画色
  /// @param segments 分割数（3 未満は 3 に丸める）
  /// @param depth    深度テストを行うか
  /// @note p0 と p1 がほぼ同じ位置なら AddSphereRings で球として描く。
  void AddCapsule(const RC::Vector3 &p0, const RC::Vector3 &p1, float radius,
                  const RC::Vector4 &color, int segments = 16,
                  bool depth = true);

  /// @brief OBB（有向境界ボックス）を追加する
  /// @param center      中心座標
  /// @param axisX       X 軸方向（内部で正規化する）
  /// @param axisY       Y 軸方向（内部で正規化する）
  /// @param axisZ       Z 軸方向（内部で正規化する）
  /// @param halfExtents 各軸方向の半サイズ
  /// @param color       描画色
  /// @param depth       深度テストを行うか
  void AddOBB(const RC::Vector3 &center, const RC::Vector3 &axisX,
              const RC::Vector3 &axisY, const RC::Vector3 &axisZ,
              const RC::Vector3 &halfExtents, const RC::Vector4 &color,
              bool depth = true);

  /// @brief 視錐台（コーナー 8 点指定）を追加する
  /// @param corners 頂点配列（0〜3: Near 面 LB/LT/RT/RB、4〜7: Far 面 LB/LT/RT/RB）
  /// @param color   描画色
  /// @param depth   深度テストを行うか
  void AddFrustum(const RC::Vector3 corners[8], const RC::Vector4 &color,
                  bool depth = true);

  /// @brief カメラパラメータから視錐台を計算して追加する
  /// @param camPos  カメラ位置
  /// @param forward 前方ベクトル
  /// @param up      上方ベクトル
  /// @param fovYRad 垂直視野角（ラジアン）
  /// @param aspect  アスペクト比（W/H）
  /// @param nearZ   近クリップ面距離
  /// @param farZ    遠クリップ面距離
  /// @param color   描画色
  /// @param depth   深度テストを行うか
  void AddFrustumCamera(const RC::Vector3 &camPos, const RC::Vector3 &forward,
                        const RC::Vector3 &up, float fovYRad, float aspect,
                        float nearZ, float farZ, const RC::Vector4 &color,
                        bool depth = true);

  /// @brief 現在蓄積されている形状を転送して描画し、描いた分をクリアする
  /// @param cl    コマンドリスト
  /// @param depth 深度テストあり（true）となし（false）のどちらを描画するか
  /// @note 旧方式との互換用。通常は TransferVertices + DrawRange を使う。
  void Draw(ID3D12GraphicsCommandList *cl, bool depth);

  /// @brief 現在蓄積されている頂点データを今フレームの一時領域（FrameResource）へ転送する
  /// @note 深度ありの頂点を先に、深度なしの頂点を後ろに並べる。容量不足なら今回は描かない。
  void TransferVertices();

  /// @brief 転送済みの頂点バッファ内の特定範囲を直接描画する
  /// @param cl    コマンドリスト
  /// @param depth 描画状態（深度テストあり／なし）
  /// @param start 開始頂点インデックス（各リスト内でのインデックス）
  /// @param count 描画頂点数
  void DrawRange(ID3D12GraphicsCommandList *cl, bool depth, uint32_t start,
                 uint32_t count);

  /// @brief 現在蓄積されている頂点数を取得する
  /// @param depth true で深度テストあり、false で深度テストなしの頂点数
  /// @return 頂点数
  uint32_t GetVertexCount(bool depth) const;

  /// @brief 描画すべきデータが一つでも存在するか確認する
  /// @return 存在するなら true
  bool HasAny() const { return !vtxDepth_.empty() || !vtxNoDepth_.empty(); }

  /// @brief 設定されているブレンドモードを取得する
  /// @return ブレンドモード
  BlendMode BlendAt3D() const { return blendAt3D_; }

  /// @brief 蓄積された頂点データをクリアする（描画後などに使用）
  void Clear();

private:
  /// @struct Vertex
  /// @brief 3D プリミティブ描画用の頂点構造体
  struct Vertex {
    RC::Vector3 pos;   ///< 位置座標
    RC::Vector4 color; ///< 頂点カラー
  };

  /// @struct PerFrameCB
  /// @brief フレームごとに転送する定数バッファ
  struct alignas(16) PerFrameCB {
    RC::Matrix4x4 View; ///< ビュー行列
    RC::Matrix4x4 Proj; ///< プロジェクション行列
  };

  static constexpr uint32_t kMaxDrawPerFrame = 64; ///< 1 フレームあたりの最大描画呼び出し数
  /// @brief 256 バイト境界に切り上げる（CBV のサイズ制約のため）
  /// @param v 切り上げ前のサイズ（バイト）
  /// @return 256 の倍数に切り上げたサイズ
  static constexpr uint32_t Align256(uint32_t v) { return (v + 255u) & ~255u; }

  /// @brief 指定された頂点数を収容できるように頂点バッファを拡張／確保する
  /// @param vertexCount 必要な頂点数（最低でも 4096 頂点分は確保する）
  void EnsureVB_(size_t vertexCount);

private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_; ///< D3D12 デバイス

  std::vector<Vertex> vtxDepth_;   ///< 深度テストありで描画する頂点リスト
  std::vector<Vertex> vtxNoDepth_; ///< 深度テストなしで描画する頂点リスト

  PerFrameCB perFrame_{};             ///< カレントフレームのカメラ行列
  BlendMode blendAt3D_ = kBlendModeNone; ///< 現在のブレンドモード

  // VB（動的拡張対応）
  Microsoft::WRL::ComPtr<ID3D12Resource> vb_; ///< 固定の頂点バッファ（転送は FrameResource 側を使う）
  Vertex *vbMap_ = nullptr;                   ///< vb_ のマップ先
  size_t vbCapacity_ = 0; ///< 頂点バッファの収容可能頂点数
  D3D12_VERTEX_BUFFER_VIEW vbView_{}; ///< 描画に使う頂点バッファビュー（TransferVertices で設定）

  // CB リングバッファ（行列転送用）
  Microsoft::WRL::ComPtr<ID3D12Resource> cbRes_; ///< 行列用の定数バッファ
  uint8_t *cbMap_ = nullptr;                     ///< cbRes_ のマップ先
  uint32_t cbStride_ = 0;                        ///< 1 スロットのサイズ（256 バイト境界）
  uint32_t cbCursor_ = 0;                        ///< 書き込み位置（BeginFrame で 0 に戻す）
};
