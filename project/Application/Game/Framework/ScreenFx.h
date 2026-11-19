#pragma once

class PostProcess;

/// @file ScreenFx.h
/// @brief PostProcess に値を流し込む「画面演出」の共通インタフェース
/// @details 墨（InkScreenFx）・力尽きた演出（DeathScreenFx）・露出フラッシュ（ExposureFlash）は、
///          どれも「毎フレーム時間を進めて PostProcess に値を書く」「シーンを抜けるときに
///          自分が積んだ・書いたものを元へ戻す」「いま効いているかを答える」という同じ契約を持つ。
///          以前は 3 クラスがそれぞれ同じ名前の関数をばらばらに持っていただけだったので、
///          ここで基底クラスにまとめ、呼び出し側が「画面演出」として同じ扱い
///          （例: シーン終了時に ScreenFx& の一覧へまとめて Clear）をできるようにした（ポリモーフィズム）。
///
///          水中の見た目（UnderwaterLook）は状態を持たない関数群とパラメータの束なので、
///          ここには含めない（「画面演出の 1 つ」という is-a が成り立たないため）。
class ScreenFx {
public:
  virtual ~ScreenFx() = default;

  /// @brief 時間を進め、PostProcess に値を書き込む（毎フレーム呼ぶ）
  /// @param pp         反映先の PostProcess（nullptr なら内部の時間だけ進める）
  /// @param dt         前フレームからの経過時間（秒）
  /// @param underwater カメラが水中にいるか（使わない演出は無視する）
  virtual void Update(PostProcess *pp, float dt, bool underwater) = 0;

  /// @brief 演出を止め、自分が積んだエフェクト・書いた値を元に戻す
  /// @param pp 反映先の PostProcess（nullptr なら内部状態だけ消す）
  virtual void Clear(PostProcess *pp) = 0;

  /// @brief 演出が動作中か
  /// @return 何かを画面に出している（出す予定がある）なら true
  virtual bool IsActive() const = 0;
};
