#include "AudioStream.h"
#include "Common/Log/Log.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

// ==== Media Foundation ====
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h> // PROPVARIANT / PropVariantClear

namespace {

// ================================================================
// WAV（ファイルを直接読む）
// ================================================================
class WavStreamReader final : public AudioStreamReader {
public:
  bool Init(const std::string &path) {
    name_ = path;
    file_.open(std::filesystem::path(SoundPathToWide(path.c_str())), std::ios_base::binary);
    if (!file_.is_open()) {
      Log::Print("[AudioStream] Failed to open: " + path);
      return false;
    }
    file_.seekg(0, std::ios_base::end);
    const int64_t fileSize = static_cast<int64_t>(file_.tellg());
    file_.seekg(0, std::ios_base::beg);
    if (!SoundParseWaveHeader(file_, fileSize, fmt_, dataOffset_, dataSize_, path)) {
      return false;
    }
    dataSize_ -= dataSize_ % fmt_.nBlockAlign; // XAudio2 はブロック単位で読む
    if (dataSize_ == 0) {
      Log::Print("[AudioStream] No audio frames: " + path);
      return false;
    }
    totalFrames_ = dataSize_ / fmt_.nBlockAlign;
    return SeekFrame(0);
  }

  size_t Read(BYTE *dst, size_t maxBytes) override {
    if (end_ || !dst) {
      return 0;
    }
    const size_t block = fmt_.nBlockAlign;
    size_t want = maxBytes - maxBytes % block;
    want = static_cast<size_t>((std::min)(static_cast<uint64_t>(want), dataSize_ - pos_));
    if (want == 0) {
      end_ = true;
      return 0;
    }
    file_.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(want));
    size_t got = static_cast<size_t>(file_.gcount());
    got -= got % block;
    pos_ += got;
    if (got < want || pos_ >= dataSize_) {
      end_ = true; // 末尾（またはファイルが途中で欠けている）
      file_.clear();
    }
    return got;
  }

  bool SeekFrame(uint64_t frame) override {
    frame = (std::min)(frame, totalFrames_);
    pos_ = frame * fmt_.nBlockAlign;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_ + static_cast<int64_t>(pos_)), std::ios_base::beg);
    end_ = pos_ >= dataSize_;
    return static_cast<bool>(file_);
  }

private:
  std::ifstream file_;
  std::string name_;
  int64_t dataOffset_ = 0; ///< data チャンク本体の先頭
  uint64_t dataSize_ = 0;  ///< data チャンク本体のサイズ（ブロック境界に切り詰め済み）
  uint64_t pos_ = 0;       ///< data チャンク内の読み込み位置（バイト）
};

// ================================================================
// Media Foundation（mp3 / m4a / aac / wma）
// ================================================================
class MfStreamReader final : public AudioStreamReader {
public:
  bool Init(const std::string &path) {
    name_ = path;
    if (!SoundEnsureMediaFoundation()) {
      return false;
    }
    const std::wstring w = SoundPathToWide(path.c_str());
    HRESULT hr = MFCreateSourceReaderFromURL(w.c_str(), nullptr, &reader_);
    if (FAILED(hr) || !reader_) {
      return Fail_("MFCreateSourceReaderFromURL", hr);
    }
    // 最初の音声ストリームだけを読む（m4a のカバー画像などは無視）
    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(&outType);
    if (FAILED(hr)) {
      return Fail_("MFCreateMediaType", hr);
    }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    hr = reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
                                      outType.Get());
    if (FAILED(hr)) {
      return Fail_("SetCurrentMediaType", hr);
    }
    hr = QueryFormat_(fmt_);
    if (FAILED(hr)) {
      return Fail_("Query output format", hr);
    }
    if (fmt_.nChannels == 0 || fmt_.nSamplesPerSec == 0 || fmt_.nBlockAlign == 0) {
      return Fail_("Invalid output format", E_FAIL);
    }

    // 長さ（100ns 単位）。取れなければ 0（不明）のまま
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                    MF_PD_DURATION, &var)) &&
        var.vt == VT_UI8) {
      totalFrames_ = TimeToFrame_(static_cast<int64_t>(var.uhVal.QuadPart));
    }
    PropVariantClear(&var);
    return true;
  }

  size_t Read(BYTE *dst, size_t maxBytes) override {
    if (!dst) {
      return 0;
    }
    const size_t block = fmt_.nBlockAlign;
    const size_t want = maxBytes - maxBytes % block;
    size_t out = 0;
    while (out < want) {
      if (pendingPos_ >= pending_.size()) {
        if (end_ || !FillPending_()) {
          break;
        }
      }
      const size_t n = (std::min)(want - out, pending_.size() - pendingPos_);
      memcpy(dst + out, pending_.data() + pendingPos_, n);
      out += n;
      pendingPos_ += n;
    }
    // 末尾の端数フレームは捨てる（ブロック境界を保つ）
    return out - out % block;
  }

  bool SeekFrame(uint64_t frame) override {
    if (!reader_) {
      return false;
    }
    if (frame == 0 && !touched_) {
      // まだ何も読んでいない → 既に先頭にいる。シーク非対応の形式（生の .aac など）でも再生できるように
      // SetCurrentPosition を呼ばずに済ませる
      end_ = false;
      return true;
    }
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = static_cast<LONGLONG>(frame * 10'000'000ull / fmt_.nSamplesPerSec);
    const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    if (FAILED(hr)) {
      Log::Print(std::format("[AudioStream] Seek failed (hr=0x{:08X}): {}", static_cast<unsigned>(hr), name_));
      return false;
    }
    pending_.clear();
    pendingPos_ = 0;
    skipBytes_ = 0;
    // mp3 等はフレーム単位でしかシークできないので、最初のサンプルのタイムスタンプを見て
    // 目標位置までの端数を読み捨てる（サンプル精度のシークにする）
    alignAfterSeek_ = true;
    seekTarget_ = frame;
    end_ = false;
    return true;
  }

private:
  bool Fail_(const char *what, HRESULT hr) {
    Log::Print(std::format("[AudioStream] {} failed (hr=0x{:08X}): {}", what, static_cast<unsigned>(hr), name_));
    return false;
  }

  uint64_t TimeToFrame_(int64_t hns) const {
    if (hns <= 0) {
      return 0;
    }
    // 1 時間 × 192kHz でも 64bit に収まる（3.6e10 × 1.92e5 ≒ 6.9e15）
    return (static_cast<uint64_t>(hns) * fmt_.nSamplesPerSec + 5'000'000ull) / 10'000'000ull;
  }

  HRESULT QueryFormat_(WAVEFORMATEX &out) {
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    HRESULT r = reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &type);
    if (FAILED(r) || !type) {
      return FAILED(r) ? r : E_FAIL;
    }
    WAVEFORMATEX *pwfx = nullptr;
    UINT32 cbwfx = 0;
    r = MFCreateWaveFormatExFromMFMediaType(type.Get(), &pwfx, &cbwfx);
    if (FAILED(r) || !pwfx) {
      return FAILED(r) ? r : E_FAIL;
    }
    const bool okFmt = SoundCollapseExtensibleFormat(*pwfx, cbwfx);
    out = *pwfx;
    CoTaskMemFree(pwfx);
    return okFmt ? S_OK : E_FAIL;
  }

  /// @brief 次のサンプルをデコードして pending_ に入れる
  /// @return データが入ったら true。末尾 / エラーなら false（end_ を立てる）
  bool FillPending_() {
    touched_ = true;
    pending_.clear();
    pendingPos_ = 0;
    for (;;) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      Microsoft::WRL::ComPtr<IMFSample> sample;
      HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags,
                                       &ts, &sample);
      if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
        Fail_("ReadSample", FAILED(hr) ? hr : E_FAIL);
        end_ = true;
        return false;
      }
      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        // 再生中に形式が変わると XAudio2 のボイスに送れない。そこで打ち切る
        WAVEFORMATEX changed{};
        if (FAILED(QueryFormat_(changed)) || memcmp(&changed, &fmt_, sizeof(WAVEFORMATEX)) != 0) {
          Log::Print("[AudioStream] Output format changed mid-stream. Stopped: " + name_);
          end_ = true;
          return false;
        }
      }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        end_ = true;
        return false;
      }
      if (!sample) {
        continue;
      }

      if (alignAfterSeek_) {
        alignAfterSeek_ = false;
        const uint64_t sampleFrame = TimeToFrame_(ts);
        if (seekTarget_ > sampleFrame) {
          skipBytes_ = (seekTarget_ - sampleFrame) * fmt_.nBlockAlign;
        } else if (sampleFrame > seekTarget_) {
          seekCorrection_ = sampleFrame - seekTarget_; // 目標を過ぎて着地した（再生位置の計算で補正する）
        }
      }

      Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
      hr = sample->ConvertToContiguousBuffer(&buffer);
      if (FAILED(hr) || !buffer) {
        Fail_("ConvertToContiguousBuffer", hr);
        end_ = true;
        return false;
      }
      BYTE *data = nullptr;
      DWORD cb = 0;
      hr = buffer->Lock(&data, nullptr, &cb);
      if (FAILED(hr)) {
        Fail_("IMFMediaBuffer::Lock", hr);
        end_ = true;
        return false;
      }
      if (skipBytes_ >= cb) {
        skipBytes_ -= cb;
        buffer->Unlock();
        continue; // このサンプルは丸ごとシーク目標より前
      }
      const size_t skip = static_cast<size_t>(skipBytes_);
      skipBytes_ = 0;
      pending_.assign(data + skip, data + cb);
      buffer->Unlock();
      if (!pending_.empty()) {
        return true;
      }
    }
  }

  Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
  std::string name_;
  std::vector<BYTE> pending_; ///< デコード済みで未出力の PCM
  size_t pendingPos_ = 0;     ///< pending_ の読み出し位置
  uint64_t skipBytes_ = 0;    ///< シーク後に読み捨てる残りバイト数
  bool alignAfterSeek_ = false;
  uint64_t seekTarget_ = 0;
  bool touched_ = false; ///< 一度でも ReadSample したか
};

} // namespace

std::unique_ptr<AudioStreamReader> AudioStreamReader::Open(const std::string &path) {
  if (path.empty()) {
    return nullptr;
  }
  const std::string resolved = SoundResolvePath(path.c_str());
  if (SoundHasExt(resolved.c_str(), ".wav")) {
    auto r = std::make_unique<WavStreamReader>();
    if (!r->Init(resolved)) {
      return nullptr;
    }
    return r;
  }
  if (SoundIsMediaFoundationFormat(resolved.c_str())) {
    auto r = std::make_unique<MfStreamReader>();
    if (!r->Init(resolved)) {
      return nullptr;
    }
    return r;
  }
  Log::Print("[AudioStream] Unsupported audio format: " + resolved);
  return nullptr;
}
