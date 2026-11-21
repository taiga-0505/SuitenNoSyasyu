#pragma once
#include "Sound.h" // WAVEFORMATEX / BYTE
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

/// @class AudioStreamReader
/// @brief 音声ファイルを少しずつ PCM にデコードするリーダー（ストリーミング再生用）
/// @details ファイル全体をメモリに展開せず、再生に必要な分だけ読む。
///          30 分の ASMR 音声（48kHz / ステレオ / 16bit）でも常駐メモリは数百 KB で済み、
///          ロード時のヒッチ（全体のデコード待ち）も起きない。
///          .wav はファイルを直接読み、.mp3 / .m4a / .aac / .wma は Media Foundation でデコードする。
/// @note スレッドセーフではない。AudioEngine がストリーム用ミューテックスで守って使う。
class AudioStreamReader {
public:
  virtual ~AudioStreamReader() = default;

  /// @brief ファイルを開く
  /// @param path ファイルパス（ゲーム側 Resources → エンジン側 Resources の順で解決する）
  /// @return 開けたリーダー。失敗時 nullptr（理由はログに出す）
  static std::unique_ptr<AudioStreamReader> Open(const std::string &path);

  /// @brief 出力される PCM の形式
  /// @return PCM / IEEE float の基本形式
  const WAVEFORMATEX &Format() const { return fmt_; }

  /// @brief 全体の長さ（フレーム数 = サンプル数 / チャンネル）
  /// @return フレーム数（不明なら 0）
  uint64_t TotalFrames() const { return totalFrames_; }

  /// @brief PCM を読む
  /// @param dst      書き込み先
  /// @param maxBytes 最大バイト数
  /// @return 書き込んだバイト数（nBlockAlign の倍数）。0 なら末尾に達したか読み込みエラー
  virtual size_t Read(BYTE *dst, size_t maxBytes) = 0;

  /// @brief 指定フレームへ移動する
  /// @param frame 先頭からのフレーム位置
  /// @return 移動できたら true
  virtual bool SeekFrame(uint64_t frame) = 0;

  /// @brief 末尾に達したか（エラーで読めなくなった場合も true）
  /// @return 末尾なら true
  bool IsEnd() const { return end_; }

  /// @brief 直前のシークで、目標より後ろに着地した分（フレーム）を取り出す（取り出すと 0 に戻る）
  /// @return ずれ（フレーム）。Read() の後に呼ぶ
  /// @note mp3 等はフレーム単位でしかシークできず、目標を少し過ぎた位置から始まることがある。
  ///       再生位置の計算を実際の位置に合わせるために使う。
  uint64_t TakeSeekCorrection() {
    const uint64_t c = seekCorrection_;
    seekCorrection_ = 0;
    return c;
  }

protected:
  WAVEFORMATEX fmt_{};      ///< 出力形式
  uint64_t totalFrames_ = 0; ///< 全体のフレーム数（不明なら 0）
  bool end_ = false;         ///< 末尾に達したか
  uint64_t seekCorrection_ = 0; ///< シークが目標を過ぎて着地した分（フレーム）
};
