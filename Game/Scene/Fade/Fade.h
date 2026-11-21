#pragma once

#include "Math/Math.h" // Transform, Vector4 のため
#include "Scene.h"     // SceneContext のため
#include <d3d12.h>

/// @class Fade
/// @brief 画面全体を黒スプライトで覆い、フェードイン／アウト／半透明オーバーレイを行うクラス
class Fade {
public:
  /// @brief フェードの状態
  enum class Status {
    None,     ///< 何もしない
    FadeIn,   ///< 黒から画面が見えるようになる
    FadeOut,  ///< 画面が黒で覆われていく
    kOverlay, ///< 一定の透明度の黒を重ね続ける
  };

  /// @brief フェード用スプライトを読み込み、画面サイズに合わせて初期化する
  /// @param ctx シーンコンテキスト
  /// @param width 画面幅
  /// @param height 画面高さ
  /// @note 既にスプライトを持っている場合は一度破棄してから作り直す
  void Init(SceneContext &ctx, float width, float height);

  /// @brief フェード用スプライトを解放する
  void Term();

  /// @brief フェードを開始する
  /// @param status 開始するフェードの種類
  /// @param duration フェードにかける時間（秒）
  /// @param alpha kOverlay 時の黒の不透明度（0〜1）
  void Start(Status status, float duration, float alpha = 0.5f);

  /// @brief フェードを止め、状態を None に戻す
  void Stop();

  /// @brief フェードを 1 フレーム進め、スプライトの不透明度を更新する
  /// @note 1 フレーム = 1/60 秒の固定値で進める
  void Update();

  /// @brief フェード用スプライトを描画する
  /// @param cl コマンドリスト（互換のため残しているだけで使用しない）
  /// @note PreDraw2D が呼ばれた後に呼ぶこと
  void
  Draw(ID3D12GraphicsCommandList *cl);

  /// @brief フェードが終わったかを返す
  /// @return FadeIn / FadeOut は経過時間が duration に達したら true、それ以外の状態は常に true
  bool IsFinished() const;

private:
  Status status_ = Status::None; ///< 現在のフェード状態

  int fadeSpriteHandle_ = -1; ///< フェード用スプライトのハンドル（未読み込みなら -1）

  float duration_ = 0.0f;     ///< フェードにかける時間（秒）
  float counter_ = 0.0f;      ///< フェード開始からの経過時間（秒）
  float overlayAlpha_ = 0.0f; ///< kOverlay 時の不透明度

  float width_ = 0.0f;  ///< 画面幅
  float height_ = 0.0f; ///< 画面高さ
};
