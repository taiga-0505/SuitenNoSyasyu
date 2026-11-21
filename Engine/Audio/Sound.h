#pragma once
#include "EngineConfig.h"
#include <cstdint>
#include <memory>
#include <string>
#include <windows.h>
#include <wrl/client.h>
#include <xaudio2.h>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "xaudio2.lib")

/// @brief チャンクヘッダー構造体
struct ChunkHeader {
  char id[4];   ///< チャンクの識別子
  int32_t size; ///< チャンクのサイズ
};

/// @brief RIFF ヘッダー構造体
struct RiffHeader {
  ChunkHeader chunk; ///< チャンクヘッダー
  char type[4];      ///< RIFF のタイプ（例: "WAVE"）
};

/// @brief フォーマットチャンク構造体
struct FormatChunk {
  ChunkHeader chunk; ///< チャンクヘッダー
  WAVEFORMATEX fmt;  ///< 音声フォーマット情報
};

/// @brief 音声データ構造体
struct SoundData {
  WAVEFORMATEX wfex{};               ///< 音声フォーマット（PCM / IEEE float の基本形式）
  std::unique_ptr<BYTE[]> pBuffer{}; ///< 音声データ本体
  unsigned int bufferSize = 0;       ///< データのバイト数（0 なら読み込み失敗）
};

/// @brief WAV ファイルをロードする
/// @param filename ファイルパス
/// @return ロードされた音声データ（失敗時は bufferSize == 0 の空データ）
SoundData SoundLoadWave(const char *filename);

/// @brief 拡張子を自動判別してオーディオファイルをロードする（.wav / .mp3 / .m4a / .aac / .wma）
/// @param filename ファイルパス（ゲーム側 Resources → エンジン側 Resources の順で解決する）
/// @return ロードされた音声データ（失敗時は bufferSize == 0 の空データ）
SoundData SoundLoadAudio(const char *filename);

/// @brief 音声データを解放する
/// @param soundData 解放対象の音声データのポインタ
void SoundUnload(SoundData *soundData);
