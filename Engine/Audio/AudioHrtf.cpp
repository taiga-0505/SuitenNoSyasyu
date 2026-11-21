#include "AudioHrtf.h"
#include "Common/Log/Log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <type_traits>
#include <vector>
#include <windows.h>
#include <mmreg.h> // WAVE_FORMAT_IEEE_FLOAT / WAVEFORMATEXTENSIBLE
#include <xapo.h>  // IXAPO（基底クラスは使わず自前で実装するので xapobase.lib は不要）

#include "steamaudio/include/phonon.h"

#ifndef XAPO_E_FORMAT_UNSUPPORTED
#define XAPO_E_FORMAT_UNSUPPORTED ((HRESULT)0x88970001L) // MAKE_HRESULT(SEVERITY_ERROR, FACILITY_XAPO, 0x01)
#endif

namespace {

/// @brief phonon.dll から取り出す関数
/// @details リンク時に phonon.lib へ依存しないよう、使う関数だけ GetProcAddress で取る
struct PhononApi {
  HMODULE dll = nullptr;
  decltype(&::iplContextCreate) contextCreate = nullptr;
  decltype(&::iplContextRelease) contextRelease = nullptr;
  decltype(&::iplHRTFCreate) hrtfCreate = nullptr;
  decltype(&::iplHRTFRelease) hrtfRelease = nullptr;
  decltype(&::iplBinauralEffectCreate) binauralCreate = nullptr;
  decltype(&::iplBinauralEffectRelease) binauralRelease = nullptr;
  decltype(&::iplBinauralEffectReset) binauralReset = nullptr;
  decltype(&::iplBinauralEffectApply) binauralApply = nullptr;
};

/// @brief モジュール全体の状態
struct State {
  std::mutex mutex;                    ///< context / hrtfs / status を守る（メインスレッド同士の保護）
  PhononApi api;                       ///< 関数（Init 後は読み取りのみ。オーディオスレッドからも使う）
  IPLContext context = nullptr;        ///< Steam Audio のコンテキスト
  std::atomic<bool> available{false};  ///< 使えるか
  std::string status = "未初期化";     ///< 状態の説明
  std::map<uint64_t, IPLHRTF> hrtfs;   ///< (サンプリングレート, ブロック長) → HRTF
};

/// @brief 状態（静的破棄順の問題を避けるため解放しない）
State &S() {
  static State *s = new State();
  return *s;
}

/// @brief ブロック長（Steam Audio の frameSize）。XAudio2 の処理単位（10ms）に合わせる
/// @param rate サンプリングレート
/// @return フレーム数
uint32_t BlockFramesFor_(uint32_t rate) { return (std::max)((rate + 99u) / 100u, 16u); }

/// @brief HRTF を取得する（無ければ作る）
/// @param rate   サンプリングレート
/// @param frames ブロック長
/// @return HRTF（失敗時 null）
IPLHRTF GetHrtf_(uint32_t rate, uint32_t frames) {
  State &s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  if (!s.context) {
    return nullptr;
  }
  const uint64_t key = (static_cast<uint64_t>(rate) << 32) | frames;
  if (auto it = s.hrtfs.find(key); it != s.hrtfs.end()) {
    return it->second;
  }
  IPLAudioSettings audio{};
  audio.samplingRate = static_cast<IPLint32>(rate);
  audio.frameSize = static_cast<IPLint32>(frames);
  IPLHRTFSettings hs{};
  hs.type = IPL_HRTFTYPE_DEFAULT; // Steam Audio 内蔵の HRTF
  hs.volume = 1.0f;
  hs.normType = IPL_HRTFNORMTYPE_NONE;
  IPLHRTF hrtf = nullptr;
  const IPLerror err = s.api.hrtfCreate(s.context, &audio, &hs, &hrtf);
  if (err != IPL_STATUS_SUCCESS || !hrtf) {
    Log::Print(std::format("[HRTF] iplHRTFCreate failed (err={}, {}Hz / {} frames)", static_cast<int>(err), rate,
                           frames));
    return nullptr;
  }
  s.hrtfs[key] = hrtf;
  Log::Print(std::format("[HRTF] HRTF ready ({}Hz / {} frames)", rate, frames));
  return hrtf;
}

/// @brief 32bit float で指定チャンネル数の形式か
/// @param f  形式
/// @param ch チャンネル数
/// @return 条件を満たせば true
bool IsFloatFormat_(const WAVEFORMATEX &f, WORD ch) {
  if (f.nChannels != ch || f.wBitsPerSample != 32 || f.nSamplesPerSec == 0) {
    return false;
  }
  if (f.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return true;
  }
  if (f.wFormatTag == WAVE_FORMAT_EXTENSIBLE && f.cbSize >= 22) {
    const auto &ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(f);
    return ext.SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
  }
  return false;
}

/// @brief XAudio2 に返す「対応している形式」を確保する（CoTaskMemAlloc。XAudio2 側が解放する）
/// @param ch   チャンネル数
/// @param rate サンプリングレート
/// @return 確保した形式
WAVEFORMATEX *AllocFloatFormat_(WORD ch, DWORD rate) {
  auto *f = static_cast<WAVEFORMATEX *>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
  if (f) {
    f->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f->nChannels = ch;
    f->nSamplesPerSec = rate;
    f->wBitsPerSample = 32;
    f->nBlockAlign = static_cast<WORD>(ch * 4);
    f->nAvgBytesPerSec = rate * f->nBlockAlign;
    f->cbSize = 0;
  }
  return f;
}

/// @brief XAPO の登録情報
const XAPO_REGISTRATION_PROPERTIES kRegistration = {
    {0x6b1a3c2e, 0x4f7d, 0x4a8e, {0x9c, 0x21, 0x5e, 0x13, 0x77, 0xa0, 0xd4, 0x58}}, // 任意の CLSID
    L"Chaso HRTF (Steam Audio)",
    L"",
    1,
    0,
    XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH | XAPO_FLAG_BUFFERCOUNT_MUST_MATCH,
    1,
    1,
    1,
    1,
};

} // namespace

namespace AudioHrtf {

// ================================================================
// エフェクト（XAPO）
// ================================================================
/// @details 入力: 32bit float モノラル、出力: 32bit float ステレオ（サンプリングレートは同じ）。
///          通常 XAudio2 は毎回同じフレーム数（10ms）で呼ぶので、その場合は遅延なしで直接処理する。
///          それ以外のフレーム数が来たら FIFO 方式に切り替え、入力をため、blockFrames_ 単位で
///          Steam Audio に渡し、結果を出力 FIFO から同じフレーム数だけ取り出す。
///          切り替え時に出力 FIFO へ 1 ブロック分の無音を入れるので、取り出し不足は起きない（以後 遅延 1 ブロック）。
class Effect final : public IXAPO {
public:
  Effect() = default;

  /// @brief 方向と立体感の強さを設定する（どのスレッドからでも可）
  void SetParams(float x, float y, float z, float blend) {
    dirX_.store(x, std::memory_order_relaxed);
    dirY_.store(y, std::memory_order_relaxed);
    dirZ_.store(z, std::memory_order_relaxed);
    blend_.store(blend, std::memory_order_relaxed);
  }

  // --- IUnknown ---
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IXAPO)) {
      *ppv = static_cast<IXAPO *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --ref_;
    if (r == 0) {
      delete this;
    }
    return r;
  }

  // --- IXAPO ---
  HRESULT STDMETHODCALLTYPE GetRegistrationProperties(XAPO_REGISTRATION_PROPERTIES **props) override {
    if (!props) {
      return E_POINTER;
    }
    auto *p = static_cast<XAPO_REGISTRATION_PROPERTIES *>(CoTaskMemAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES)));
    if (!p) {
      return E_OUTOFMEMORY;
    }
    *p = kRegistration;
    *props = p;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE IsInputFormatSupported(const WAVEFORMATEX *outputFmt, const WAVEFORMATEX *requested,
                                                   WAVEFORMATEX **supported) override {
    if (!requested) {
      return E_POINTER;
    }
    const bool rateOk = !outputFmt || outputFmt->nSamplesPerSec == requested->nSamplesPerSec;
    if (IsFloatFormat_(*requested, 1) && rateOk) {
      if (supported) {
        *supported = nullptr;
      }
      return S_OK;
    }
    if (supported) {
      *supported = AllocFloatFormat_(1, outputFmt ? outputFmt->nSamplesPerSec : requested->nSamplesPerSec);
    }
    return XAPO_E_FORMAT_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(const WAVEFORMATEX *inputFmt, const WAVEFORMATEX *requested,
                                                    WAVEFORMATEX **supported) override {
    if (!requested) {
      return E_POINTER;
    }
    const bool rateOk = !inputFmt || inputFmt->nSamplesPerSec == requested->nSamplesPerSec;
    if (IsFloatFormat_(*requested, 2) && rateOk) {
      if (supported) {
        *supported = nullptr;
      }
      return S_OK;
    }
    if (supported) {
      *supported = AllocFloatFormat_(2, inputFmt ? inputFmt->nSamplesPerSec : requested->nSamplesPerSec);
    }
    return XAPO_E_FORMAT_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE Initialize(const void *, UINT32) override { return S_OK; }

  void STDMETHODCALLTYPE Reset() override {
    // オーディオスレッドから呼ばれる可能性があるので、実際の処理は次の Process の先頭で行う
    resetPending_.store(true, std::memory_order_release);
  }

  HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 inCount, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *inParams,
                                           UINT32 outCount,
                                           const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *outParams) override {
    if (inCount != 1 || outCount != 1 || !inParams || !outParams || !inParams[0].pFormat || !outParams[0].pFormat) {
      return E_INVALIDARG;
    }
    const WAVEFORMATEX &fi = *inParams[0].pFormat;
    const WAVEFORMATEX &fo = *outParams[0].pFormat;
    if (!IsFloatFormat_(fi, 1) || !IsFloatFormat_(fo, 2) || fi.nSamplesPerSec != fo.nSamplesPerSec) {
      return XAPO_E_FORMAT_UNSUPPORTED;
    }
    DestroyEffect_();
    maxFrames_ = (std::max)(inParams[0].MaxFrameCount, 1u);
    blockFrames_ = (std::min)(BlockFramesFor_(fi.nSamplesPerSec), maxFrames_);

    // 作れなくても失敗にはしない（Process が両耳に同じ音を出すだけになる）
    if (S().available.load()) {
      hrtf_ = GetHrtf_(fi.nSamplesPerSec, blockFrames_);
      if (hrtf_) {
        IPLAudioSettings audio{};
        audio.samplingRate = static_cast<IPLint32>(fi.nSamplesPerSec);
        audio.frameSize = static_cast<IPLint32>(blockFrames_);
        IPLBinauralEffectSettings es{};
        es.hrtf = hrtf_;
        std::lock_guard<std::mutex> lk(S().mutex);
        if (S().context && S().api.binauralCreate(S().context, &audio, &es, &effect_) != IPL_STATUS_SUCCESS) {
          effect_ = nullptr;
          Log::Print("[HRTF] iplBinauralEffectCreate failed. この音は立体化されません");
        }
      }
    }

    inFifo_.assign(static_cast<size_t>(blockFrames_) + maxFrames_, 0.0f);
    outFifo_.assign((static_cast<size_t>(blockFrames_) * 2 + maxFrames_) * 2, 0.0f);
    blockIn_.assign(blockFrames_, 0.0f);
    blockL_.assign(blockFrames_, 0.0f);
    blockR_.assign(blockFrames_, 0.0f);
    ResetFifos_();
    return S_OK;
  }

  void STDMETHODCALLTYPE UnlockForProcess() override { DestroyEffect_(); }

  void STDMETHODCALLTYPE Process(UINT32 inCount, const XAPO_PROCESS_BUFFER_PARAMETERS *inParams, UINT32 outCount,
                                 XAPO_PROCESS_BUFFER_PARAMETERS *outParams, BOOL enabled) override {
    if (inCount != 1 || outCount != 1 || !inParams || !outParams) {
      return;
    }
    // 出力バッファは LockForProcess の MaxFrameCount 分しか無いので、それを超えて書かない
    const UINT32 frames = (std::min)(inParams[0].ValidFrameCount, maxFrames_);
    const auto *in = static_cast<const float *>(inParams[0].pBuffer);
    auto *out = static_cast<float *>(outParams[0].pBuffer);
    const bool silent = inParams[0].BufferFlags == XAPO_BUFFER_SILENT;
    outParams[0].ValidFrameCount = frames;

    if (resetPending_.exchange(false, std::memory_order_acq_rel)) {
      ResetFifos_();
      if (effect_) {
        S().api.binauralReset(effect_);
      }
    }

    if (!effect_ || !enabled || blockFrames_ == 0) {
      // 立体化しない: 両耳に同じ音（無効化されたときも出力チャンネル数は 2 のまま）
      for (UINT32 i = 0; i < frames; ++i) {
        const float s = silent ? 0.0f : in[i];
        out[i * 2] = s;
        out[i * 2 + 1] = s;
      }
      outParams[0].BufferFlags = silent ? XAPO_BUFFER_SILENT : XAPO_BUFFER_VALID;
      return;
    }

    // 0. 直接処理: いつもの 10ms 単位で来ている間は遅延なしで処理する
    if (!fifoMode_) {
      if (frames == blockFrames_) {
        std::memcpy(blockIn_.data(), in, sizeof(float) * frames);
        if (silent) {
          std::memset(blockIn_.data(), 0, sizeof(float) * frames);
        }
        ApplyBlock_();
        for (uint32_t i = 0; i < blockFrames_; ++i) {
          out[i * 2] = blockL_[i];
          out[i * 2 + 1] = blockR_[i];
        }
        outParams[0].BufferFlags = XAPO_BUFFER_VALID;
        return;
      }
      // 違うフレーム数が来た → FIFO 方式に切り替える（1 ブロック分の無音を先に入れて取り出し不足を防ぐ）
      fifoMode_ = true;
      std::fill(outFifo_.begin(), outFifo_.begin() + static_cast<std::ptrdiff_t>(blockFrames_) * 2, 0.0f);
      outCount_ = blockFrames_;
    }

    // 1. 入力を FIFO に積む
    float *fifoIn = inFifo_.data() + inCount_;
    if (silent) {
      std::memset(fifoIn, 0, sizeof(float) * frames);
    } else {
      std::memcpy(fifoIn, in, sizeof(float) * frames);
    }
    inCount_ += frames;

    // 2. ブロック単位で HRTF を掛け、出力 FIFO に積む
    size_t consumed = 0;
    while (inCount_ - consumed >= blockFrames_) {
      std::memcpy(blockIn_.data(), inFifo_.data() + consumed, sizeof(float) * blockFrames_);
      ApplyBlock_();

      float *dst = outFifo_.data() + outCount_ * 2;
      for (uint32_t i = 0; i < blockFrames_; ++i) {
        dst[i * 2] = blockL_[i];
        dst[i * 2 + 1] = blockR_[i];
      }
      outCount_ += blockFrames_;
      consumed += blockFrames_;
    }
    if (consumed > 0) {
      std::memmove(inFifo_.data(), inFifo_.data() + consumed, sizeof(float) * (inCount_ - consumed));
      inCount_ -= consumed;
    }

    // 3. 出力 FIFO から同じフレーム数だけ取り出す（先頭に 1 ブロックの余裕があるので必ず足りる）
    const size_t take = (std::min)(static_cast<size_t>(frames), outCount_);
    std::memcpy(out, outFifo_.data(), sizeof(float) * take * 2);
    if (take < frames) {
      std::memset(out + take * 2, 0, sizeof(float) * (frames - take) * 2);
    }
    std::memmove(outFifo_.data(), outFifo_.data() + take * 2, sizeof(float) * (outCount_ - take) * 2);
    outCount_ -= take;
    outParams[0].BufferFlags = XAPO_BUFFER_VALID;
  }

  UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 outputFrames) override { return outputFrames; }
  UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 inputFrames) override { return inputFrames; }

private:
  ~Effect() { DestroyEffect_(); } // Release() 経由でのみ破棄する

  /// @brief blockIn_ の 1 ブロックに HRTF を掛けて blockL_ / blockR_ に出す（オーディオスレッド）
  void ApplyBlock_() {
    float *inPtr = blockIn_.data();
    float *outPtrs[2] = {blockL_.data(), blockR_.data()};
    IPLAudioBuffer inBuf{1, static_cast<IPLint32>(blockFrames_), &inPtr};
    IPLAudioBuffer outBuf{2, static_cast<IPLint32>(blockFrames_), outPtrs};

    // 方向は別スレッドから 1 成分ずつ書かれるので、読み出し途中の組み合わせでも単位ベクトルに直す
    float x = dirX_.load(std::memory_order_relaxed);
    float y = dirY_.load(std::memory_order_relaxed);
    float z = dirZ_.load(std::memory_order_relaxed);
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-6f && std::isfinite(len)) {
      x /= len;
      y /= len;
      z /= len;
    } else {
      x = 0.0f;
      y = 0.0f;
      z = 1.0f;
    }
    // エンジン（左手系: +x 右 / +y 上 / +z 正面）→ Steam Audio のリスナー空間（右手系: -z が正面）
    IPLBinauralEffectParams p{};
    p.direction = IPLVector3{x, y, -z};
    p.interpolation = IPL_HRTFINTERPOLATION_BILINEAR; // 頭の周りを動く音でも切り替わりが滑らか
    const float blend = blend_.load(std::memory_order_relaxed);
    p.spatialBlend = std::isfinite(blend) ? std::clamp(blend, 0.0f, 1.0f) : 0.0f;
    p.hrtf = hrtf_;
    p.peakDelays = nullptr;
    S().api.binauralApply(effect_, &p, &inBuf, &outBuf);
  }

  void ResetFifos_() {
    std::fill(inFifo_.begin(), inFifo_.end(), 0.0f);
    std::fill(outFifo_.begin(), outFifo_.end(), 0.0f);
    inCount_ = 0;
    outCount_ = 0;
    fifoMode_ = false; // まずは遅延なしの直接処理から始める
  }

  void DestroyEffect_() {
    if (effect_) {
      S().api.binauralRelease(&effect_);
      effect_ = nullptr;
    }
    hrtf_ = nullptr; // HRTF はキャッシュが持つ
  }

  std::atomic<ULONG> ref_{1};
  std::atomic<float> dirX_{0.0f};
  std::atomic<float> dirY_{0.0f};
  std::atomic<float> dirZ_{1.0f};
  std::atomic<float> blend_{0.0f};
  std::atomic<bool> resetPending_{false}; ///< Reset() が呼ばれた（次の Process で処理する）
  bool fifoMode_ = false;                 ///< FIFO 方式（遅延 1 ブロック）に切り替わったか

  IPLBinauralEffect effect_ = nullptr; ///< Steam Audio のエフェクト（LockForProcess で作る）
  IPLHRTF hrtf_ = nullptr;             ///< 使う HRTF（キャッシュが所有）
  uint32_t blockFrames_ = 0;           ///< Steam Audio に渡すブロック長
  uint32_t maxFrames_ = 0;             ///< XAudio2 が 1 回に渡す最大フレーム数
  std::vector<float> inFifo_;          ///< 入力 FIFO（モノラル）
  size_t inCount_ = 0;                 ///< 入力 FIFO のフレーム数
  std::vector<float> outFifo_;         ///< 出力 FIFO（ステレオ・インターリーブ）
  size_t outCount_ = 0;                ///< 出力 FIFO のフレーム数
  std::vector<float> blockIn_;         ///< Steam Audio への入力
  std::vector<float> blockL_;          ///< Steam Audio からの出力（左）
  std::vector<float> blockR_;          ///< Steam Audio からの出力（右）
};

void EffectReleaser::operator()(Effect *e) const noexcept {
  if (e) {
    e->Release();
  }
}

EffectPtr CreateEffect() {
  if (!IsAvailable()) {
    return nullptr;
  }
  return EffectPtr(new Effect()); // 参照 1
}

IUnknown *AsXapo(Effect *e) { return e ? static_cast<IXAPO *>(e) : nullptr; }

void SetParams(Effect *e, float x, float y, float z, float spatialBlend) {
  if (e) {
    e->SetParams(x, y, z, spatialBlend);
  }
}

// ================================================================
// 初期化 / 終了
// ================================================================
bool Init() {
  State &s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  if (s.available.load()) {
    return true;
  }
  if (!s.api.dll) {
    // 実行ファイルと同じフォルダの phonon.dll だけを読む（カレントディレクトリや PATH 上の DLL を拾わない）
    wchar_t exePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
      std::wstring dllPath(exePath, len);
      const size_t slash = dllPath.find_last_of(L"\\/");
      dllPath = (slash == std::wstring::npos ? std::wstring() : dllPath.substr(0, slash + 1)) + L"phonon.dll";
      s.api.dll = LoadLibraryExW(dllPath.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!s.api.dll) {
      s.status = "phonon.dll が見つかりません（実行ファイルと同じフォルダに置いてください）";
      Log::Print("[HRTF] phonon.dll not found. HRTF は無効（X3DAudio のパンニングで鳴ります）");
      return false;
    }
  }
  auto load = [&](auto &fn, const char *name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(s.api.dll, name));
    return fn != nullptr;
  };
  const bool ok = load(s.api.contextCreate, "iplContextCreate") && load(s.api.contextRelease, "iplContextRelease") &&
                  load(s.api.hrtfCreate, "iplHRTFCreate") && load(s.api.hrtfRelease, "iplHRTFRelease") &&
                  load(s.api.binauralCreate, "iplBinauralEffectCreate") &&
                  load(s.api.binauralRelease, "iplBinauralEffectRelease") &&
                  load(s.api.binauralReset, "iplBinauralEffectReset") &&
                  load(s.api.binauralApply, "iplBinauralEffectApply");
  if (!ok) {
    s.status = "phonon.dll に必要な関数がありません（Steam Audio 4.x を使ってください）";
    Log::Print("[HRTF] phonon.dll is missing required functions");
    return false;
  }

  IPLContextSettings cs{};
  cs.version = STEAMAUDIO_VERSION;
  cs.simdLevel = IPL_SIMDLEVEL_AVX2; // AVX-512 はクロック低下を招くことがあるので使わない
  const IPLerror err = s.api.contextCreate(&cs, &s.context);
  if (err != IPL_STATUS_SUCCESS || !s.context) {
    s.context = nullptr;
    s.status = std::format("Steam Audio の初期化に失敗しました（err={}。DLL とヘッダのバージョン不一致の可能性）",
                           static_cast<int>(err));
    Log::Print("[HRTF] " + s.status);
    return false;
  }
  s.available = true;
  s.status = std::format("有効 (Steam Audio {}.{}.{})", STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR,
                         STEAMAUDIO_VERSION_PATCH);
  Log::Print("[HRTF] Steam Audio initialized");
  return true;
}

void Term() {
  State &s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  s.available = false;
  for (auto &kv : s.hrtfs) {
    IPLHRTF h = kv.second;
    s.api.hrtfRelease(&h);
  }
  s.hrtfs.clear();
  if (s.context) {
    s.api.contextRelease(&s.context);
    s.context = nullptr;
  }
  // DLL は解放しない（終了間際に XAudio2 側が XAPO を破棄する可能性に備える）
  if (s.api.dll) {
    s.status = "終了済み";
  }
}

bool IsAvailable() { return S().available.load(); }

std::string StatusText() {
  State &s = S();
  std::lock_guard<std::mutex> lk(s.mutex);
  return s.status;
}

void Prewarm(uint32_t sampleRate) {
  if (sampleRate == 0 || !IsAvailable()) {
    return;
  }
  GetHrtf_(sampleRate, BlockFramesFor_(sampleRate));
}

} // namespace AudioHrtf
