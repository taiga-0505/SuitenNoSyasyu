#include "Sound.h"
#include "Common/ResourcePath.h"
#include "Common/Log/Log.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mmreg.h> // WAVE_FORMAT_EXTENSIBLE / WAVE_FORMAT_IEEE_FLOAT / WAVEFORMATEXTENSIBLE
#include <string>
#include <vector>

// ==== Media Foundation ====
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

// =========================
// ユーティリティ
// =========================
/// @brief パスが指定された拡張子を持っているか判定する（大文字小文字を区別しない）
/// @param path 判定対象のパス
/// @param ext  拡張子（例: ".wav", "wav"）
/// @return 指定した拡張子であれば true
static bool HasExtInsensitive(const char *path, const char *ext) {
  if (!path || !ext)
    return false;
  std::string s(path);
  std::string e(ext);
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  if (e.size() && e[0] != '.')
    e = "." + e;
  if (s.size() < e.size())
    return false;
  return s.rfind(e) == s.size() - e.size();
}

/// @brief パス文字列を Wide 文字列（UTF-16）に変換する
/// @param path UTF-8 の文字列（ソース中のリテラルは /utf-8 でビルドしているため UTF-8）。
///             UTF-8 として不正なら ANSI コードページ（Content Browser 経由の
///             std::filesystem::path::string() など）とみなして変換する
/// @return Wide 文字列（path が nullptr なら空）
static std::wstring ToWideFromUTF8(const char *path) {
  if (!path)
    return L"";
  int lenW = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
  if (lenW <= 0) {
    // UTF-8 ではない → OS のナロー文字列（ANSI）として扱う
    return std::filesystem::path(path).wstring();
  }
  std::wstring w(lenW, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, &w[0], lenW);
  if (!w.empty() && w.back() == L'\0')
    w.pop_back();
  return w;
}

// =========================
// WAV ローダ
// =========================
/// @details 失敗しても assert で落とさず、空の SoundData（bufferSize == 0）を返す。
///          エディタから任意のパスを指定できるようになったため、壊れたファイルや
///          未対応形式で停止しないようにしている。呼び出し側は bufferSize で判定すること。
///          "fmt " と "data" 以外のチャンク（LIST / JUNK / fact など）はすべて読み飛ばす。
SoundData SoundLoadWave(const char *filename) {
  const std::string name = filename ? filename : "";

  std::ifstream file(name, std::ios_base::binary);
  if (!file.is_open()) {
    Log::Print("[Sound] Failed to open: " + name);
    return {};
  }

  // ヘッダに書かれたサイズを信用せず、実ファイルサイズで上限を切る（壊れたヘッダで巨大確保しない）
  file.seekg(0, std::ios_base::end);
  const std::streamoff fileSize = file.tellg();
  file.seekg(0, std::ios_base::beg);
  if (fileSize < static_cast<std::streamoff>(sizeof(RiffHeader))) {
    Log::Print("[Sound] File too small: " + name);
    return {};
  }

  RiffHeader riff{};
  file.read(reinterpret_cast<char *>(&riff), sizeof(riff));
  if (!file || strncmp(riff.chunk.id, "RIFF", 4) != 0 || strncmp(riff.type, "WAVE", 4) != 0) {
    Log::Print("[Sound] Not a RIFF/WAVE file: " + name);
    return {};
  }

  WAVEFORMATEX fmt{};
  bool hasFmt = false;
  std::unique_ptr<BYTE[]> pcm;
  uint32_t pcmSize = 0;

  // チャンクを順に読み、fmt と data を拾う。他は読み飛ばす
  for (;;) {
    ChunkHeader chunk{};
    file.read(reinterpret_cast<char *>(&chunk), sizeof(chunk));
    if (!file) {
      break; // ファイル末尾に達した
    }
    if (chunk.size < 0) {
      break; // 壊れたヘッダ
    }
    // RIFF のチャンクは 2 バイト境界に揃えられる（奇数サイズなら 1 バイトのパディング）
    const std::streamoff padded = static_cast<std::streamoff>(chunk.size) + (chunk.size & 1);

    if (strncmp(chunk.id, "fmt ", 4) == 0) {
      // WAVEFORMATEXTENSIBLE(40byte) まで受け取れるようにバッファで読む
      BYTE raw[64] = {};
      const std::streamsize toRead =
          (std::min)(static_cast<std::streamsize>(chunk.size), static_cast<std::streamsize>(sizeof(raw)));
      file.read(reinterpret_cast<char *>(raw), toRead);
      if (!file) {
        break;
      }
      if (toRead < static_cast<std::streamsize>(offsetof(WAVEFORMATEX, cbSize))) {
        Log::Print("[Sound] fmt chunk too small: " + name);
        return {};
      }
      memcpy(&fmt, raw, (std::min)(static_cast<size_t>(toRead), sizeof(WAVEFORMATEX)));

      if (fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE && toRead >= 40) {
        // SubFormat の先頭 DWORD が 1=PCM / 3=IEEE float。基本形式に読み替える
        // （SoundData は WAVEFORMATEX しか持たないため）
        uint32_t subFormat = 0;
        memcpy(&subFormat, raw + 24, sizeof(subFormat));
        if (subFormat == WAVE_FORMAT_PCM || subFormat == WAVE_FORMAT_IEEE_FLOAT) {
          fmt.wFormatTag = static_cast<WORD>(subFormat);
        } else {
          Log::Print("[Sound] Unsupported WAVE_FORMAT_EXTENSIBLE subformat: " + name);
          return {};
        }
      }
      if (fmt.wFormatTag != WAVE_FORMAT_PCM && fmt.wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
        Log::Print("[Sound] Unsupported WAV format tag (" + std::to_string(fmt.wFormatTag) + "): " + name);
        return {};
      }
      fmt.cbSize = 0;
      hasFmt = true;
      file.seekg(padded - toRead, std::ios_base::cur);
    } else if (strncmp(chunk.id, "data", 4) == 0) {
      if (chunk.size <= 0) {
        Log::Print("[Sound] Empty data chunk: " + name);
        return {};
      }
      // 残りバイト数を超える宣言サイズは切り詰める
      const std::streamoff remain = fileSize - static_cast<std::streamoff>(file.tellg());
      if (remain <= 0) {
        Log::Print("[Sound] data chunk has no payload: " + name);
        return {};
      }
      pcmSize = static_cast<uint32_t>((std::min)(static_cast<std::streamoff>(chunk.size), remain));
      pcm = std::make_unique<BYTE[]>(pcmSize);
      file.read(reinterpret_cast<char *>(pcm.get()), pcmSize);
      if (!file) {
        // 末尾が欠けているファイル。読めた分だけ使う
        pcmSize = static_cast<uint32_t>(file.gcount());
        file.clear();
      }
      break; // data はストリーム末尾にあるのが通例。以降は読まない
    } else {
      file.seekg(padded, std::ios_base::cur);
    }
  }

  if (!hasFmt || !pcm || pcmSize == 0) {
    Log::Print("[Sound] Missing fmt/data chunk: " + name);
    return {};
  }
  if (fmt.nChannels == 0 || fmt.nSamplesPerSec == 0 || fmt.nBlockAlign == 0) {
    Log::Print("[Sound] Broken fmt chunk: " + name);
    return {};
  }

  SoundData soundData{};
  soundData.wfex = fmt;
  soundData.pBuffer = std::move(pcm);
  soundData.bufferSize = pcmSize;
  return soundData;
}

// =========================
// Media Foundation 初期化
// =========================
/// @brief Media Foundation を初期化する（初回呼び出し時のみ実行）
/// @return 初期化できていれば true
static bool EnsureMediaFoundationStartup() {
  static bool attempted = false;
  static bool ok = false;
  if (!attempted) {
    attempted = true;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ok = SUCCEEDED(hr);
    if (!ok) {
      Log::Print(std::format("[Sound] MFStartup failed (hr=0x{:08X})。mp3 等のデコードは無効になります",
                             static_cast<unsigned>(hr)));
    }
  }
  return ok;
}

/// @brief WAVEFORMATEXTENSIBLE を基本形式（PCM / IEEE float）の WAVEFORMATEX に読み替える
/// @param wfex 入出力（EXTENSIBLE でなければ cbSize を 0 にするだけ）
/// @param cb   wfex の実サイズ（バイト）
/// @return 読み替えられた（または元から基本形式だった）ら true。未対応のサブフォーマットなら false
static bool CollapseExtensibleFormat(WAVEFORMATEX &wfex, size_t cb) {
  if (wfex.wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
    wfex.cbSize = 0;
    return true;
  }
  if (cb < sizeof(WAVEFORMATEXTENSIBLE)) {
    return false;
  }
  // SubFormat GUID の先頭 DWORD が 1=PCM / 3=IEEE float
  const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(&wfex);
  const unsigned long sub = ext->SubFormat.Data1;
  if (sub != WAVE_FORMAT_PCM && sub != WAVE_FORMAT_IEEE_FLOAT) {
    return false;
  }
  wfex.wFormatTag = static_cast<WORD>(sub);
  wfex.cbSize = 0;
  return true;
}

// =========================
// MF を使った汎用オーディオ読み込み（MP3→PCM）
// =========================
/// @brief Media Foundation を使用してオーディオファイルをロードする（MP3 等を PCM にデコード）
/// @param wpath      Wide 文字列のファイルパス
/// @param nameForLog ログ出力用のファイル名
/// @return ロードされた音声データ
/// @note 失敗時は空の SoundData を返す（assert で落とさない）。
static SoundData SoundLoadWithMediaFoundation(const wchar_t *wpath, const std::string &nameForLog) {
  if (!EnsureMediaFoundationStartup()) {
    return {};
  }

  auto fail = [&](const char *what, HRESULT hr) -> SoundData {
    Log::Print(std::format("[Sound] {} failed (hr=0x{:08X}): {}", what, static_cast<unsigned>(hr),
                           nameForLog));
    return {};
  };

  Microsoft::WRL::ComPtr<IMFSourceReader> reader;
  HRESULT hr = MFCreateSourceReaderFromURL(wpath, nullptr, &reader);
  if (FAILED(hr) || !reader) {
    return fail("MFCreateSourceReaderFromURL", hr);
  }

  // 出力を PCM に固定
  Microsoft::WRL::ComPtr<IMFMediaType> outType;
  hr = MFCreateMediaType(&outType);
  if (FAILED(hr)) {
    return fail("MFCreateMediaType", hr);
  }
  hr = outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (FAILED(hr)) {
    return fail("SetGUID(MAJOR_TYPE)", hr);
  }
  hr = outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  if (FAILED(hr)) {
    return fail("SetGUID(SUBTYPE)", hr);
  }
  hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                   outType.Get());
  if (FAILED(hr)) {
    return fail("SetCurrentMediaType", hr);
  }

  // 実際の PCM 出力形式を WAVEFORMATEX（基本形式）として取り出す
  WAVEFORMATEX wfex{};
  auto queryFormat = [&](WAVEFORMATEX &out) -> HRESULT {
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    HRESULT r = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type);
    if (FAILED(r) || !type) {
      return FAILED(r) ? r : E_FAIL;
    }
    WAVEFORMATEX *pwfx = nullptr;
    UINT32 cbwfx = 0;
    r = MFCreateWaveFormatExFromMFMediaType(type.Get(), &pwfx, &cbwfx);
    if (FAILED(r) || !pwfx) {
      return FAILED(r) ? r : E_FAIL;
    }
    // マルチチャンネル等では WAVEFORMATEXTENSIBLE が返る。SoundData は WAVEFORMATEX しか
    // 持たないので基本形式に読み替える（読み替えられなければ失敗）
    const bool okFmt = CollapseExtensibleFormat(*pwfx, cbwfx);
    out = *pwfx; // 先頭 sizeof(WAVEFORMATEX) バイトだけコピー
    CoTaskMemFree(pwfx);
    return okFmt ? S_OK : E_FAIL;
  };
  hr = queryFormat(wfex);
  if (FAILED(hr)) {
    return fail("Query output format", hr);
  }

  std::vector<BYTE> pcmBytes;
  for (;;) {
    DWORD flags = 0;
    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                            &flags, nullptr, &sample);
    if (FAILED(hr)) {
      return fail("ReadSample", hr);
    }
    if (flags & MF_SOURCE_READERF_ERROR) {
      return fail("ReadSample (stream error)", E_FAIL);
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      // 途中で出力形式が変わった。同じ形式のまま追記できないので取り直す。
      // （先頭から形式が変わるケースはほぼ無いが、変わった場合はここまでの PCM を捨てる）
      WAVEFORMATEX changed{};
      hr = queryFormat(changed);
      if (FAILED(hr)) {
        return fail("Query output format (changed)", hr);
      }
      if (memcmp(&changed, &wfex, sizeof(WAVEFORMATEX)) != 0) {
        wfex = changed;
        pcmBytes.clear();
      }
    }

    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      break;
    }
    if (!sample) {
      continue;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || !buffer) {
      return fail("ConvertToContiguousBuffer", hr);
    }

    BYTE *pData = nullptr;
    DWORD cbData = 0;
    hr = buffer->Lock(&pData, nullptr, &cbData);
    if (FAILED(hr)) {
      return fail("IMFMediaBuffer::Lock", hr);
    }

    size_t oldSize = pcmBytes.size();
    pcmBytes.resize(oldSize + cbData);
    memcpy(pcmBytes.data() + oldSize, pData, cbData);

    buffer->Unlock();
  }

  if (pcmBytes.empty()) {
    Log::Print("[Sound] Decoded stream is empty: " + nameForLog);
    return {};
  }

  SoundData sd{};
  sd.wfex = wfex;
  sd.bufferSize = static_cast<unsigned int>(pcmBytes.size());
  sd.pBuffer = std::make_unique<BYTE[]>(sd.bufferSize);
  memcpy(sd.pBuffer.get(), pcmBytes.data(), sd.bufferSize);
  return sd;
}

// =========================
// 拡張子で自動判定して読み込み
// =========================
SoundData SoundLoadAudio(const char *filenameIn) {
  if (!filenameIn || !*filenameIn) {
    return {};
  }
  // ゲーム側 Resources → エンジン側（Chaso/Resources）の順で探す
  const std::string resolvedPath = Chaso::ResolvePath(filenameIn);
  const char *filename = resolvedPath.c_str();
  if (HasExtInsensitive(filename, ".wav")) {
    return SoundLoadWave(filename);
  }
  // .mp3 以外にも Media Foundation が扱える形式は同じ経路で読める
  if (HasExtInsensitive(filename, ".mp3") || HasExtInsensitive(filename, ".m4a") ||
      HasExtInsensitive(filename, ".aac") || HasExtInsensitive(filename, ".wma")) {
    std::wstring w = ToWideFromUTF8(filename);
    return SoundLoadWithMediaFoundation(w.c_str(), filename);
  }
  Log::Print(std::string("[Sound] Unsupported audio format: ") + filename);
  return {};
}

void SoundUnload(SoundData *soundData) {
  if (!soundData) {
    return;
  }
  soundData->pBuffer.reset();
  soundData->bufferSize = 0;
  soundData->wfex = {};
}
