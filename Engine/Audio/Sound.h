#pragma once
#include "EngineConfig.h"
#include <cstdint>
#include <iosfwd>
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

// ================================================================
// ストリーミング再生（AudioStream.cpp）と共有するヘルパ
// ================================================================

/// @brief リソースパスを解決する（ゲーム側 Resources → エンジン側 Resources の順）
/// @param filename 元のパス
/// @return 実際に開くパス
std::string SoundResolvePath(const char *filename);

/// @brief パス文字列を Wide 文字列（UTF-16）に変換する（UTF-8 として不正なら ANSI とみなす）
/// @param path 変換元
/// @return Wide 文字列（path が nullptr なら空）
std::wstring SoundPathToWide(const char *path);

/// @brief パスが指定された拡張子を持っているか（大文字小文字を区別しない）
/// @param path 判定対象のパス
/// @param ext  拡張子（例: ".wav"）
/// @return 一致すれば true
bool SoundHasExt(const char *path, const char *ext);

/// @brief Media Foundation で読む形式か（.mp3 / .m4a / .aac / .wma）
/// @param path 判定対象のパス
/// @return Media Foundation 経由で読む拡張子なら true
bool SoundIsMediaFoundationFormat(const char *path);

/// @brief Media Foundation を初期化する（初回呼び出し時のみ実行、スレッドセーフ）
/// @return 初期化できていれば true
bool SoundEnsureMediaFoundation();

/// @brief WAVEFORMATEXTENSIBLE を基本形式（PCM / IEEE float）の WAVEFORMATEX に読み替える
/// @param wfex 入出力（EXTENSIBLE でなければ cbSize を 0 にするだけ）
/// @param cb   wfex の実サイズ（バイト）
/// @return 読み替えられた（または元から基本形式だった）ら true
bool SoundCollapseExtensibleFormat(WAVEFORMATEX &wfex, size_t cb);

/// @brief WAV のヘッダを解析し、fmt と data チャンクの位置を求める
/// @param file       先頭に位置したバイナリストリーム
/// @param fileSize   ファイルサイズ（バイト）
/// @param fmt        [out] 音声フォーマット（PCM / IEEE float の基本形式）
/// @param dataOffset [out] data チャンク本体の先頭位置（バイト）
/// @param dataSize   [out] data チャンク本体のサイズ（実ファイルに収まる分だけ）
/// @param nameForLog ログ用の名前
/// @return 解析できたら true（失敗理由はログに出す）
bool SoundParseWaveHeader(std::istream &file, int64_t fileSize, WAVEFORMATEX &fmt, int64_t &dataOffset,
                          uint64_t &dataSize, const std::string &nameForLog);
