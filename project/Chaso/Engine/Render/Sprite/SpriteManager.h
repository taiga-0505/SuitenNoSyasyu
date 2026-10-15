#pragma once

// SpriteManager
// -----------------------------------------------------------------------------
// 「スプライト周り」の管理クラス
// - スプライトはハンドル制（int）
// - Sprite2D 実体の生成/破棄/状態変更をここに集約
// - 全スプライト共通の Quad( SpriteMesh2D ) もここで共有
// -----------------------------------------------------------------------------

#include <d3d12.h>

#include <memory>
#include <string>
#include <vector>

#include "Sprite/Sprite2D.h"

class TextureManager; // 前方宣言

namespace RC {

/// @class SpriteManager
/// @brief スプライトリソースを管理するマネージャクラス
/// @details スプライトをハンドル（int）で管理し、共通のメッシュ（Quad）を共有して描画する。
class SpriteManager {
public:
  /// @brief デバイスとテクスチャマネージャを受け取って初期化する
  /// @param device D3D12 デバイス
  /// @param texman テクスチャマネージャ（画像ロードに使う）
  void Init(ID3D12Device *device, TextureManager *texman);

  /// @brief 終了処理。管理しているスプライトと共有 Quad メッシュを解放する
  void Term();

  /// @brief スプライトをロードしてハンドルを返す
  /// @param path    画像ファイルのパス
  /// @param screenW 基準となる画面幅（ピクセル）
  /// @param screenH 基準となる画面高さ（ピクセル）
  /// @param srgb    sRGB として読み込むか
  /// @return スプライトハンドル（失敗時は -1）
  /// @note スロットは再利用せず、常に末尾に追加する（AllocatedCount 参照）。
  int Load(const std::string &path, float screenW, float screenH,
           bool srgb = true);

  /// @brief 指定したハンドルに対応するスプライトを解放する
  /// @param handle スプライトハンドル
  void Unload(int handle);

  /// @brief 有効なハンドルか確認する
  /// @param handle スプライトハンドル
  /// @return 有効なら true
  bool IsValid(int handle) const;

  /// @brief ハンドルからスプライトの実体を取得する
  /// @param handle スプライトハンドル
  /// @return Sprite2D へのポインタ。無効なハンドルの場合は nullptr
  /// @note 非同期ロードが終わっていれば、ここで実テクスチャの SRV に差し替える。
  Sprite2D *Get(int handle);

  /// @brief ハンドルからスプライトの実体を取得する（const 版）
  /// @param handle スプライトハンドル
  /// @return Sprite2D への const ポインタ。無効なハンドルの場合は nullptr
  /// @note const 版はテクスチャの差し替えを行わない。
  const Sprite2D *Get(int handle) const;

  /// @brief テクスチャ全体を描画する
  /// @param handle スプライトハンドル
  /// @param cl     グラフィックスコマンドリスト
  /// @note DrawRect / DrawRectUV で変えた UV 変換は、ここで単位行列に戻す。
  void Draw(int handle, ID3D12GraphicsCommandList *cl);

  /// @brief テクスチャ内の矩形範囲（ピクセル単位）を指定して描画する
  /// @param handle  スプライトハンドル
  /// @param srcX    矩形開始 X（ピクセル）
  /// @param srcY    矩形開始 Y（ピクセル）
  /// @param srcW    矩形幅（ピクセル）
  /// @param srcH    矩形高さ（ピクセル）
  /// @param texW    元テクスチャ全体の幅（ピクセル）
  /// @param texH    元テクスチャ全体の高さ（ピクセル）
  /// @param insetPx インセット（にじみ防止用の余白、ピクセル）
  /// @param cl      グラフィックスコマンドリスト
  void DrawRect(int handle, float srcX, float srcY, float srcW, float srcH,
                float texW, float texH, float insetPx,
                ID3D12GraphicsCommandList *cl);

  /// @brief UV 座標（0.0〜1.0）で範囲を指定して描画する
  /// @param handle スプライトハンドル
  /// @param u0     開始 U
  /// @param v0     開始 V
  /// @param u1     終了 U
  /// @param v1     終了 V
  /// @param cl     グラフィックスコマンドリスト
  void DrawRectUV(int handle, float u0, float v0, float u1, float v1,
                  ID3D12GraphicsCommandList *cl);

  /// @brief スプライトのトランスフォーム（位置・回転・スケール）を設定する
  /// @param handle スプライトハンドル
  /// @param t      設定する Transform
  void SetTransform(int handle, const Transform &t);

  /// @brief スプライトの乗算カラーを設定する
  /// @param handle スプライトハンドル
  /// @param color  設定する RGBA カラー
  void SetColor(int handle, const Vector4 &color);

  /// @brief スプライトの表示サイズ（ピクセル単位）を設定する
  /// @param handle スプライトハンドル
  /// @param w      幅（ピクセル）
  /// @param h      高さ（ピクセル）
  void SetSize(int handle, float w, float h);

  /// @brief ワールド空間（3D）モードを切り替える
  /// @param handle スプライトハンドル
  /// @param enable true でワールド空間モード
  void SetWorldSpace(int handle, bool enable);

  /// @brief ワールド空間モードで使うカメラ行列を設定する
  /// @param handle スプライトハンドル
  /// @param view ビュー行列
  /// @param proj プロジェクション行列
  void SetCamera(int handle, const Matrix4x4 &view, const Matrix4x4 &proj);

  /// @brief スプライトが参照しているテクスチャのハンドルを取得する
  /// @param handle スプライトハンドル
  /// @return テクスチャハンドル（無効なら -1）
  int GetTexHandle(int handle) const;

  /// @brief これまでに確保したスロットの総数（＝次に発行されるハンドル値）
  /// @details Load() はスロットを再利用せず常に末尾へ足すため、この値は
  ///          「起動してから何回 Load したか」に等しく、単調増加する。
  ///          InUseCount() との差が「解放済みだが空いたままのスロット数」。
  /// @return 確保済みスロットの総数
  /// @note D-04 / D-05 の確認用。シーンを一周して InUseCount() が
  ///       元の値へ戻るなら、ハンドルは取りこぼしなく回収されている。
  size_t AllocatedCount() const { return sprites_.size(); }

  /// @brief 現在使用中（Load 済みで Unload されていない）のスプライト数
  /// @return 使用中のスプライト数
  size_t InUseCount() const {
    size_t n = 0;
    for (const auto &s : sprites_) {
      if (s.inUse) ++n;
    }
    return n;
  }

private:
  /// @struct Slot
  /// @brief スプライト保持用スロット
  struct Slot {
    std::unique_ptr<Sprite2D> ptr; ///< スプライト実体
    bool inUse = false;           ///< 使用中フラグ
    int texHandle = -1;           ///< テクスチャハンドル
    bool texResolved = false;     ///< 実テクスチャの SRV を Sprite2D に反映済みか（非同期ロード対応）
  };

  /// @brief 共有 Quad メッシュを用意する（未作成なら作る）
  /// @return 共有 Quad メッシュ（Init 前なら nullptr）
  std::shared_ptr<SpriteMesh2D> EnsureQuad_();

  /// @brief 非同期ロード完了後の実テクスチャ SRV を Sprite2D に反映する（未完了なら仮 SRV）
  /// @param s 対象のスロット
  void ResolveTexture_(Slot &s);

private:
  ID3D12Device *device_ = nullptr;     ///< デバイス
  TextureManager *texman_ = nullptr;   ///< テクスチャマネージャ

  std::vector<Slot> sprites_;         ///< スプライトスロット配列
  std::shared_ptr<SpriteMesh2D> quad_; ///< 全スプライト共通の Quad メッシュ
};

} // namespace RC
