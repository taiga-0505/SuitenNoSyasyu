#include "AudioEngine.h"
#include "AudioStream.h"
#include "Common/Log/Log.h"
#include "Common/Math/Math.h" // Inverse（ビュー行列 → カメラのワールド行列）
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <mmdeviceapi.h> // 出力デバイスの変更通知（IMMNotificationClient）と種類の判定
#include <x3daudio.h>    // 3D 音響（定位・ドップラーの計算）。xaudio2.lib に含まれる
#include <xapofx.h>      // CreateFX（マスターのリミッター）。xaudio2.lib に含まれる
#include <xaudio2fx.h>   // XAudio2CreateReverb / I3DL2 プリセット（部屋の響き）

static_assert(X3DAUDIO_HANDLE_BYTESIZE == 20, "AudioEngine::x3dInstance_ のサイズを X3DAUDIO_HANDLE に合わせてください");

#ifndef X3DAUDIO_PI
#define X3DAUDIO_PI 3.141592654f
#endif
#ifndef X3DAUDIO_2PI
#define X3DAUDIO_2PI 6.283185307f
#endif

// ================================================================
// ローカルヘルパ
// ================================================================
namespace {

constexpr float kMaxVolume = static_cast<float>(XAUDIO2_MAX_VOLUME_LEVEL);   ///< ボイス音量の上限
constexpr float kMaxFreqRatio = static_cast<float>(XAUDIO2_DEFAULT_FREQ_RATIO); ///< ピッチ倍率の上限
constexpr float kPauseFadeSec = 0.04f;    ///< 一時停止・再開でクリック音を出さないための絞り時間
constexpr float kDeviceDebounceSec = 0.5f; ///< デバイス変更通知から作り直しまでの待ち（通知は連続で来る）
constexpr float kMaxUpdateDt = 0.25f;     ///< フェード等の 1 フレームあたりの最大進行（ヒッチ対策）

/// @brief FXMasteringLimiter の CLSID（SDK によって __uuidof の可否が違うので直接持つ）
const CLSID kClsidFXMasteringLimiter = {0xc4137916, 0x2be1, 0x46fd, {0x85, 0x99, 0x44, 0x15, 0x36, 0xf4, 0x98, 0x56}};

/// @brief FXMASTERINGLIMITER_PARAMETERS と同じレイアウト（SDK 差を避けるため自前で定義）
struct LimiterParams_ {
  UINT32 Release;  ///< 1〜20
  UINT32 Loudness; ///< 1〜1800
};

/// @brief PKEY_AudioEndpoint_FormFactor（functiondiscoverykeys_devpkey.h の INITGUID 依存を避けるため直接持つ）
const PROPERTYKEY kPkeyAudioEndpointFormFactor = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};

// ベクトルの小物。Math.h（グローバル）と MathUtils.h（RC::）の両方に同名関数があり、
// 両方を include する翻訳単位では ADL で曖昧になるため、ここで閉じた実装を使う。
RC::Vector3 Sub_(const RC::Vector3 &a, const RC::Vector3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
RC::Vector3 Scale_(const RC::Vector3 &v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float Dot_(const RC::Vector3 &a, const RC::Vector3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
RC::Vector3 Cross_(const RC::Vector3 &a, const RC::Vector3 &b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float LengthSq_(const RC::Vector3 &v) { return Dot_(v, v); }
float Length_(const RC::Vector3 &v) { return std::sqrt(LengthSq_(v)); }
bool IsFinite_(const RC::Vector3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

/// @brief 正規化する（長さがほぼ 0 か NaN なら fallback を返す）
/// @param v        正規化するベクトル
/// @param fallback 正規化できないときに返すベクトル
/// @return 正規化したベクトル、または fallback
RC::Vector3 Normalize_(const RC::Vector3 &v, const RC::Vector3 &fallback) {
  const float len = Length_(v);
  if (!(len > 1e-6f) || !std::isfinite(len)) {
    return fallback;
  }
  return Scale_(v, 1.0f / len);
}

/// @brief forward / up を正規直交にする（X3DAudio は OrientFront / OrientTop が正規直交であることを要求する）
/// @param forward 正面方向（入出力）
/// @param up      上方向（入出力。forward と平行なら別の軸から作り直す）
void Orthonormalize_(RC::Vector3 &forward, RC::Vector3 &up) {
  forward = Normalize_(forward, {0.0f, 0.0f, 1.0f});
  // up から forward 成分を取り除く
  RC::Vector3 u = Sub_(up, Scale_(forward, Dot_(up, forward)));
  if (LengthSq_(u) < 1e-8f) {
    // up が forward と平行（真上・真下を見ている）→ 別の軸から作り直す
    const RC::Vector3 ref = (std::fabs(forward.y) < 0.99f) ? RC::Vector3{0.0f, 1.0f, 0.0f} : RC::Vector3{0.0f, 0.0f, 1.0f};
    u = Sub_(ref, Scale_(forward, Dot_(ref, forward)));
  }
  up = Normalize_(u, {0.0f, 1.0f, 0.0f});
}

/// @brief 前回位置との差分から速度を推定する
/// @details テレポート（1 フレームで音速の半分以上動いた）と判定したら 0 にする。
///          ドップラー効果に使うだけなので、多少粗くても問題ない。
/// @param prev     前回の位置
/// @param cur      今回の位置
/// @param dt       経過時間（秒。0 以下なら速度 0）
/// @param maxSpeed これを超える速さはテレポートとみなす上限（ワールド単位/秒）
/// @return 推定した速度（ワールド単位/秒）
RC::Vector3 EstimateVelocity_(const RC::Vector3 &prev, const RC::Vector3 &cur, float dt, float maxSpeed) {
  if (!(dt > 0.0f)) {
    return {0.0f, 0.0f, 0.0f};
  }
  const RC::Vector3 v = Scale_(Sub_(cur, prev), 1.0f / dt);
  if (!IsFinite_(v) || Length_(v) > maxSpeed) {
    return {0.0f, 0.0f, 0.0f};
  }
  return v;
}

/// @brief RC::Vector3 を X3DAudio のベクトル型に変換する
/// @param v 変換元のベクトル
/// @return 同じ値の X3DAUDIO_VECTOR
X3DAUDIO_VECTOR ToX3D_(const RC::Vector3 &v) {
  X3DAUDIO_VECTOR r{};
  r.x = v.x;
  r.y = v.y;
  r.z = v.z;
  return r;
}

/// @brief ローパスのカットオフ周波数を XAudio2 のフィルタ設定にする
/// @param hz         カットオフ周波数（Hz。0 以下か sampleRate/6 以上なら無効 = Frequency 1.0）
/// @param sampleRate フィルタが動くサンプリングレート
/// @return フィルタ設定
XAUDIO2_FILTER_PARAMETERS LowPassParams_(float hz, uint32_t sampleRate) {
  XAUDIO2_FILTER_PARAMETERS p{};
  p.Type = LowPassFilter;
  p.Frequency = 1.0f; // = フィルタなし（XAUDIO2_MAX_FILTER_FREQUENCY）
  p.OneOverQ = 1.0f;
  if (hz > 0.0f && hz < AudioEngine::kLowPassOpenHz && sampleRate > 0 && hz * 6.0f < static_cast<float>(sampleRate)) {
    p.Frequency = 2.0f * std::sin(3.14159265f * hz / static_cast<float>(sampleRate));
  }
  return p;
}

/// @brief ローパスを対数周波数で滑らかに目標へ近づける
/// @param current 現在値（Hz）
/// @param target  目標値（Hz）
/// @param dt      経過秒（0 以下なら即座に目標値）
/// @return 新しい値（Hz）
float SmoothLowPass_(float current, float target, float dt) {
  if (!(dt > 0.0f) || std::fabs(current - target) < 0.5f) {
    return target;
  }
  const float k = 1.0f - std::exp(-dt / 0.035f); // 約 0.1 秒で 95%
  const float lc = std::log((std::max)(current, 10.0f));
  const float lt = std::log((std::max)(target, 10.0f));
  const float v = std::exp(lc + (lt - lc) * k);
  return (std::fabs(v - target) < 0.5f) ? target : v;
}

/// @brief ローパスの値を正規化する（0 以下・上限以上は「無効」）
/// @param hz 指定値
/// @return 10〜kLowPassOpenHz の値
float NormalizeLowPass_(float hz) {
  if (!std::isfinite(hz) || hz <= 0.0f || hz >= AudioEngine::kLowPassOpenHz) {
    return AudioEngine::kLowPassOpenHz;
  }
  return (std::max)(hz, 10.0f);
}

/// @brief dB を音量倍率にする
/// @param db デシベル
/// @return 倍率
float DbToGain_(float db) { return std::pow(10.0f, db / 20.0f); }

/// @brief 2 つの PCM 形式が同じか（ストリームを開き直したときの確認用）
/// @param a 形式 A
/// @param b 形式 B
/// @return 同じなら true
bool SameFormat_(const WAVEFORMATEX &a, const WAVEFORMATEX &b) {
  return a.wFormatTag == b.wFormatTag && a.nChannels == b.nChannels && a.nSamplesPerSec == b.nSamplesPerSec &&
         a.wBitsPerSample == b.wBitsPerSample && a.nBlockAlign == b.nBlockAlign;
}

// スピーカー位置ビット（WAVEFORMATEXTENSIBLE.dwChannelMask と同じ値）
constexpr uint32_t kSpkFrontLeft = 0x1;
constexpr uint32_t kSpkFrontRight = 0x2;
constexpr uint32_t kSpkFrontCenter = 0x4;
constexpr uint32_t kSpkLowFrequency = 0x8;
constexpr uint32_t kSpkBackLeft = 0x10;
constexpr uint32_t kSpkBackRight = 0x20;
constexpr uint32_t kSpkFrontLeftOfCenter = 0x40;
constexpr uint32_t kSpkFrontRightOfCenter = 0x80;
constexpr uint32_t kSpkBackCenter = 0x100;

/// @brief チャンネル数から標準的なスピーカー構成を推定する（GetChannelMask が使えないときの代替）
/// @param channels 出力のチャンネル数
/// @return チャンネルマスク（未知のチャンネル数ならステレオ）
uint32_t DefaultChannelMask_(uint32_t channels) {
  switch (channels) {
  case 1: return kSpkFrontCenter;
  case 2: return kSpkFrontLeft | kSpkFrontRight;
  case 3: return kSpkFrontLeft | kSpkFrontRight | kSpkLowFrequency;
  case 4: return kSpkFrontLeft | kSpkFrontRight | kSpkBackLeft | kSpkBackRight;
  case 5: return kSpkFrontLeft | kSpkFrontRight | kSpkLowFrequency | kSpkBackLeft | kSpkBackRight;
  case 6: return kSpkFrontLeft | kSpkFrontRight | kSpkFrontCenter | kSpkLowFrequency | kSpkBackLeft | kSpkBackRight;
  case 7:
    return kSpkFrontLeft | kSpkFrontRight | kSpkFrontCenter | kSpkLowFrequency | kSpkBackLeft | kSpkBackRight |
           kSpkBackCenter;
  case 8:
    return kSpkFrontLeft | kSpkFrontRight | kSpkFrontCenter | kSpkLowFrequency | kSpkBackLeft | kSpkBackRight |
           kSpkFrontLeftOfCenter | kSpkFrontRightOfCenter;
  default: return kSpkFrontLeft | kSpkFrontRight;
  }
}

} // namespace

// ================================================================
// ストリーミングの状態
// ================================================================
/// @details ストリーミングスレッドと共有する。デコード中も含めて mutex で守り、
///          再生位置の計算に使う segments だけは短時間しか持たない segMutex で守る
///          （メインスレッドが毎フレーム GetTime() してもデコード待ちにならないように）。
///          eos だけは IsVoiceFinished_ からロックなしで読むため atomic。
struct AudioEngine::StreamState {
  /// @brief XAudio2 に積んだ PCM が、クリップのどこに当たるか（再生位置の逆算用）
  struct Segment {
    uint64_t cumStart = 0;  ///< このボイスで積んだ累計フレーム数（SamplesPlayed と同じ基準）
    uint64_t fileStart = 0; ///< クリップ内の開始フレーム
    uint64_t frames = 0;    ///< フレーム数
  };

  std::unique_ptr<AudioStreamReader> reader;      ///< デコーダ
  IXAudio2SourceVoice *voice = nullptr;           ///< 補充先（所有しない。null なら補充しない）
  std::vector<BYTE> buffers[kStreamBufferCount];  ///< XAudio2 に積むバッファ（輪番で使う）
  uint32_t next = 0;                              ///< 次に使うバッファ
  uint32_t blockAlign = 0;                        ///< 1 フレームのバイト数
  bool loop = false;                              ///< ループ再生か
  std::atomic<bool> eos{false};                   ///< 最後まで積み終わったか
  bool loopedWithoutData = false;                 ///< 先頭に戻った直後にまだ何も読めていない（空ファイル対策）
  uint64_t readFrame = 0;                         ///< reader の現在位置（フレーム）
  uint64_t cumSubmitted = 0;                      ///< 積んだ累計フレーム数
  std::deque<Segment> segments;                   ///< 積んだ PCM の内訳（segMutex で守る）
  std::string name;                               ///< クリップのパス（開き直し・ログ用）
  std::mutex mutex;                               ///< reader / voice / バッファを守る（デコード中も持つ）
  std::mutex segMutex;                            ///< segments を守る（短時間だけ持つ）

  /// @brief 指定位置へシークする。先頭へのシークに失敗した場合は開き直す（シーク非対応の形式のループ対策）
  /// @param frame 目標のフレーム
  /// @return 成功したら true
  bool SeekOrReopen(uint64_t frame) {
    if (reader && reader->SeekFrame(frame)) {
      return true;
    }
    if (frame != 0) {
      return false;
    }
    std::unique_ptr<AudioStreamReader> r = AudioStreamReader::Open(name);
    if (!r || (reader && !SameFormat_(r->Format(), reader->Format()))) {
      return false;
    }
    reader = std::move(r);
    return true;
  }
};

// ================================================================
// デバイス監視
// ================================================================
/// @details IMMNotificationClient（既定デバイスの変更）と IXAudio2EngineCallback（致命的エラー = デバイス喪失）を
///          受け取り、フラグを立てるだけ。実際の作り直しはメインスレッドの Update() で行う。
struct AudioEngine::DeviceWatcher final : public IMMNotificationClient, public IXAudio2EngineCallback {
  std::atomic<ULONG> ref{1};                ///< 参照カウント
  std::atomic<bool> deviceChanged{false};   ///< 既定の出力デバイスが変わった
  std::atomic<bool> criticalError{false};   ///< XAudio2 が致命的エラーを報告した（デバイス喪失など）
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator; ///< デバイス列挙（種類の判定にも使う）
  bool registered = false;                  ///< 通知を登録できたか

  // --- IUnknown ---
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --ref;
    if (r == 0) {
      delete this;
    }
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *ppv = static_cast<IMMNotificationClient *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  // --- IMMNotificationClient（別スレッドから呼ばれる。フラグを立てるだけ）---
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && (role == eConsole || role == eMultimedia)) {
      deviceChanged = true;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

  // --- IXAudio2EngineCallback（音声スレッドから呼ばれる）---
  void STDMETHODCALLTYPE OnProcessingPassStart() override {}
  void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnCriticalError(HRESULT) override { criticalError = true; }

private:
  ~DeviceWatcher() = default; // Release() 経由でのみ破棄する
};

void AudioEngine::DeviceWatcherDeleter::operator()(DeviceWatcher *w) const noexcept {
  if (!w) {
    return;
  }
  if (w->enumerator && w->registered) {
    w->enumerator->UnregisterEndpointNotificationCallback(w);
    w->registered = false;
  }
  w->enumerator.Reset();
  w->Release();
}

// ================================================================
// シングルトン
// ================================================================
AudioEngine &AudioEngine::Get() {
  // 関数ローカル static だと、エンティティ（コンポーネント）の破棄より先に
  // エンジンが破棄される順序になり得る。コンポーネントのデストラクタから
  // UnloadClip() を呼ぶため、意図的に解放しないインスタンスにしておく。
  static AudioEngine *instance = new AudioEngine();
  return *instance;
}

AudioEngine::~AudioEngine() = default; // 不完全型（DeviceWatcher）を持つため .cpp で定義する

// ================================================================
// ライフサイクル
// ================================================================
bool AudioEngine::Init() {
  if (xaudio_) {
    return true;
  }
  initRequested_ = true;

  // デバイス列挙（IMMDeviceEnumerator）に COM が要る。既に別モードで初期化済みでも使えるので続行する
  if (!comInitialized_) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInitialized_ = SUCCEEDED(hr); // S_FALSE（初期化済み）も対で CoUninitialize が要る
  }

  // デバイス変更の監視（失敗しても再生には影響しない。追従しなくなるだけ）
  if (!watcher_) {
    auto *w = new DeviceWatcher();
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&w->enumerator));
    if (SUCCEEDED(hr) && w->enumerator) {
      hr = w->enumerator->RegisterEndpointNotificationCallback(w);
      w->registered = SUCCEEDED(hr);
    }
    if (!w->registered) {
      Log::Print(std::format("[Audio] Device change notification unavailable (hr=0x{:08X})。"
                             "ヘッドホンの抜き差しには XAudio2 の既定動作でのみ追従します",
                             static_cast<unsigned>(hr)));
    }
    watcher_.reset(w);
  }

  // HRTF（Steam Audio）。phonon.dll が無ければ X3DAudio のパンニングで鳴らす
  if (!hrtfInitTried_) {
    hrtfInitTried_ = true;
    AudioHrtf::Init();
  }

  // ストリーミングの補充スレッド
  if (!streamThread_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(streamMutex_);
      streamQuit_ = false;
    }
    streamThread_ = std::thread([this] { StreamThread_(); });
  }

  hasLastUpdate_ = false;
  return InitCore_();
}

bool AudioEngine::InitCore_() {
  if (xaudio_) {
    return true;
  }
  HRESULT hr = XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr)) {
    Log::Print(std::format("[Audio] XAudio2Create failed (hr=0x{:08X})。音声なしで続行します",
                           static_cast<unsigned>(hr)));
    xaudio_.Reset();
    return false;
  }
  if (watcher_) {
    xaudio_->RegisterForCallbacks(static_cast<IXAudio2EngineCallback *>(watcher_.get()));
  }

  IXAudio2MasteringVoice *rawMaster = nullptr;
  hr = xaudio_->CreateMasteringVoice(&rawMaster);
  if (FAILED(hr) || !rawMaster) {
    Log::Print(std::format("[Audio] CreateMasteringVoice failed (hr=0x{:08X})。"
                           "オーディオデバイスが無いため音声なしで続行します（接続されれば自動で再初期化します）",
                           static_cast<unsigned>(hr)));
    TermCore_();
    return false;
  }
  master_.reset(rawMaster);
  master_->SetVolume(masterVolume_);

  // バスごとのサブミックス。入力形式はマスターに合わせる
  XAUDIO2_VOICE_DETAILS md{};
  master_->GetVoiceDetails(&md);
  dstChannels_ = md.InputChannels;
  outputSampleRate_ = md.InputSampleRate;
  for (int i = 0; i < static_cast<int>(AudioBus::Count); ++i) {
    IXAudio2SubmixVoice *raw = nullptr;
    hr = xaudio_->CreateSubmixVoice(&raw, md.InputChannels, md.InputSampleRate, XAUDIO2_VOICE_USEFILTER, 0, nullptr,
                                    nullptr);
    if (FAILED(hr) || !raw) {
      Log::Print(std::format("[Audio] CreateSubmixVoice({}) failed (hr=0x{:08X})", AudioBusName(static_cast<AudioBus>(i)),
                             static_cast<unsigned>(hr)));
      // サブミックスが無くてもマスターへ直接送れば鳴らせるので続行する
      continue;
    }
    submix_[i].reset(raw);
  }
  ApplyBusVolumes_();
  UpdateBusFilters_(0.0f);
  InitReverb_();

  // 3D 音響。出力のスピーカー構成（チャンネルマスク）を X3DAudio に教える
  DWORD mask = 0;
  if (FAILED(master_->GetChannelMask(&mask)) || mask == 0) {
    mask = DefaultChannelMask_(dstChannels_);
  }
  channelMask_ = static_cast<uint32_t>(mask);
  InitX3D_();
  InitLimiter_();
  DetectOutputType_();
  if (AudioHrtf::IsAvailable()) {
    // 最初の Play() でヒッチしないよう、出力レートの HRTF を先に作っておく
    // （ソースボイスのエフェクトは出力レートに変換された後で処理される想定。違うレートは初回に作る）
    AudioHrtf::Prewarm(outputSampleRate_);
  }

  const char *typeName = outputType_ == AudioOutputType::Headphones ? "headphones"
                         : outputType_ == AudioOutputType::Speakers ? "speakers"
                                                                    : "unknown";
  Log::Print(std::format("[Audio] Initialized ({}ch / {}Hz / output={} / limiter={} / reverb={} / hrtf={})",
                         md.InputChannels, md.InputSampleRate, typeName, limiterReady_ ? "on" : "unavailable",
                         reverb_ ? "on" : "unavailable",
                         IsHrtfActive() ? "on" : (AudioHrtf::IsAvailable() ? "off (not stereo)" : "unavailable")));
  return true;
}

void AudioEngine::TermCore_() {
  // 破棄順: （ソースボイスは呼び出し側で破棄済み）→ サブミックス → マスター → エンジン
  for (auto &s : submix_) {
    s.reset();
  }
  reverb_.reset();
  reverbChannels_ = 0;
  master_.reset();
  if (xaudio_) {
    if (watcher_) {
      xaudio_->UnregisterForCallbacks(static_cast<IXAudio2EngineCallback *>(watcher_.get()));
    }
    xaudio_.Reset();
  }
  x3dReady_ = false;
  limiterReady_ = false;
  dstChannels_ = 0;
  outputSampleRate_ = 0;
}

bool AudioEngine::InitX3D_() {
  x3dReady_ = false;
  if (!xaudio_ || dstChannels_ == 0) {
    return false;
  }
  HRESULT hr = X3DAudioInitialize(channelMask_, speedOfSound_, x3dInstance_);
  if (FAILED(hr)) {
    // デバイスが報告したマスクを受け付けないことがある → チャンネル数から標準構成で再試行
    const uint32_t fallback = DefaultChannelMask_(dstChannels_);
    if (fallback != channelMask_) {
      channelMask_ = fallback;
      hr = X3DAudioInitialize(channelMask_, speedOfSound_, x3dInstance_);
    }
  }
  if (FAILED(hr)) {
    Log::Print(std::format("[Audio] X3DAudioInitialize failed (hr=0x{:08X})。3D 音響は無効（2D で鳴ります）",
                           static_cast<unsigned>(hr)));
    return false;
  }
  x3dReady_ = true;
  Log::Print(std::format("[Audio] 3D audio ready (speakers=0x{:X}, {}ch, speedOfSound={:.1f})", channelMask_,
                         dstChannels_, speedOfSound_));
  return true;
}

void AudioEngine::InitLimiter_() {
  limiterReady_ = false;
  if (!master_ || dstChannels_ == 0) {
    return;
  }
  IUnknown *fx = nullptr;
  HRESULT hr = CreateFX(kClsidFXMasteringLimiter, &fx);
  if (FAILED(hr) || !fx) {
    Log::Print(std::format("[Audio] CreateFX(MasteringLimiter) failed (hr=0x{:08X})。リミッターなしで続行します",
                           static_cast<unsigned>(hr)));
    return;
  }
  XAUDIO2_EFFECT_DESCRIPTOR desc{};
  desc.pEffect = fx;
  desc.InitialState = limiter_.enabled ? TRUE : FALSE;
  desc.OutputChannels = dstChannels_;
  XAUDIO2_EFFECT_CHAIN chain{};
  chain.EffectCount = 1;
  chain.pEffectDescriptors = &desc;
  hr = master_->SetEffectChain(&chain);
  fx->Release(); // チェーンが参照を持つ
  if (FAILED(hr)) {
    Log::Print(std::format("[Audio] SetEffectChain(limiter) failed (hr=0x{:08X})", static_cast<unsigned>(hr)));
    return;
  }
  limiterReady_ = true;
  SetLimiter(limiter_); // パラメータを反映
}

void AudioEngine::DetectOutputType_() {
  outputType_ = AudioOutputType::Unknown;
  if (!watcher_ || !watcher_->enumerator) {
    return;
  }
  Microsoft::WRL::ComPtr<IMMDevice> device;
  if (FAILED(watcher_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
    return;
  }
  Microsoft::WRL::ComPtr<IPropertyStore> props;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) {
    return;
  }
  PROPVARIANT var;
  PropVariantInit(&var);
  if (SUCCEEDED(props->GetValue(kPkeyAudioEndpointFormFactor, &var)) && var.vt == VT_UI4) {
    // EndpointFormFactor: Speakers=1, LineLevel=2, Headphones=3, Headset=5, Handset=6, SPDIF=8, DigitalAudioDisplay=9
    switch (var.ulVal) {
    case 3:
    case 5:
    case 6: outputType_ = AudioOutputType::Headphones; break;
    case 1:
    case 2:
    case 8:
    case 9: outputType_ = AudioOutputType::Speakers; break;
    default: break;
    }
  }
  PropVariantClear(&var);
}

void AudioEngine::Term() {
  // 破棄順: ソースボイス → ストリーミングスレッド → サブミックス → マスター → エンジン
  StopAll();
  if (streamThread_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(streamMutex_);
      streamQuit_ = true;
    }
    streamCv_.notify_all();
    streamThread_.join();
  }
  {
    std::lock_guard<std::mutex> lk(streamMutex_);
    streams_.clear();
    streamQuit_ = false;
  }
  for (auto &c : clips_) {
    if (c.used) {
      FreeClip_(c);
    }
  }
  clips_.clear();
  clipByPath_.clear();
  TermCore_();
  AudioHrtf::Term(); // すべてのボイス（= HRTF エフェクト）を破棄した後
  hrtfInitTried_ = false;
  watcher_.reset();
  if (comInitialized_) {
    CoUninitialize();
    comInitialized_ = false;
  }
  initRequested_ = false;
  bgmKeepAlive_ = 0;
  bgmVoiceId_ = -1;
  duckGain_ = 1.0f;
  paused_ = false;
  editorPaused_ = false;
  appliedGamePause_ = false;
  listenerHasPrev_ = false;
  hasLastUpdate_ = false;
  deviceEventDelay_ = 0.0f;
}

void AudioEngine::HandleDeviceEvents_(float dt) {
  bool resetNow = false;
  if (watcher_) {
    if (watcher_->criticalError.exchange(false)) {
      Log::Print("[Audio] XAudio2 critical error (device lost?)。エンジンを作り直します");
      resetNow = true;
    }
    if (watcher_->deviceChanged.exchange(false)) {
      deviceEventDelay_ = kDeviceDebounceSec; // 通知は連続で来るので落ち着くまで待つ
    }
  }
  if (manualResetRequested_) {
    manualResetRequested_ = false;
    resetNow = true;
  }
  if (!resetNow && deviceEventDelay_ > 0.0f) {
    deviceEventDelay_ -= dt;
    if (deviceEventDelay_ <= 0.0f) {
      resetNow = true;
    }
  }
  if (!resetNow) {
    return;
  }
  deviceEventDelay_ = 0.0f;
  if (initRequested_) {
    ResetDevice_();
  }
}

void AudioEngine::ResetDevice_() {
  Log::Print("[Audio] Output device changed. Rebuilding audio engine...");
  // 鳴っていた位置を覚えてから XAudio2 側のボイスを捨てる
  std::vector<uint64_t> frames(voices_.size(), 0);
  for (size_t i = 0; i < voices_.size(); ++i) {
    if (voices_[i].voice) {
      frames[i] = CurrentFrame_(voices_[i]);
    }
    ReleaseSourceVoice_(voices_[i]);
  }
  TermCore_();
  ++deviceResetCount_;

  if (!InitCore_()) {
    // 出力先が無い。鳴っていた音はすべて捨てる（デバイスが戻れば次の通知で初期化し直す）
    for (auto &v : voices_) {
      DestroyVoice_(v);
    }
    voices_.clear();
    bgmVoiceId_ = -1;
    bgmKeepAlive_ = 0;
    return;
  }
  for (size_t i = voices_.size(); i-- > 0;) {
    Voice &v = voices_[i];
    if (!v.stopped) {
      v.pauseGain = 0.0f; // 再開時はフェードインしてクリックを防ぐ
    }
    if (!BuildVoice_(v, frames[i])) {
      if (v.id == bgmVoiceId_) {
        bgmVoiceId_ = -1;
      }
      DestroyVoice_(v);
      voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }
  Log::Print(std::format("[Audio] Rebuilt. {} voice(s) resumed", voices_.size()));
}

void AudioEngine::Update() {
  // 経過時間はエンジン側で測る（呼び出し側の dt に依存しない。エディタの一時停止中も進む）
  const auto now = std::chrono::steady_clock::now();
  float dt = 0.0f;
  if (hasLastUpdate_) {
    dt = std::chrono::duration<float>(now - lastUpdate_).count();
  }
  lastUpdate_ = now;
  hasLastUpdate_ = true;
  dt = std::clamp(dt, 0.0f, kMaxUpdateDt);

  HandleDeviceEvents_(dt);
  if (!xaudio_) {
    return;
  }

  // ボイスごとの更新（後ろから消す）
  for (size_t i = voices_.size(); i-- > 0;) {
    Voice &v = voices_[i];
    bool remove = !v.voice || IsVoiceFinished_(v);
    if (remove && v.voice && v.hrtf && v.hrtfTailSec > 0.0f) {
      // HRTF は最後のブロックとフィルタの残響が XAPO 内に残るので、少し待ってから破棄する（末尾の欠け・クリック防止）
      v.hrtfTailSec -= (std::max)(dt, 0.001f);
      remove = false;
    }
    if (!remove) {
      UpdatePause_(v, dt);
      if (!v.pausedUser && !v.pausedGame) {
        remove = UpdateFade_(v, dt);
      }
    }
    if (remove) {
      if (v.id == bgmVoiceId_) {
        bgmVoiceId_ = -1;
        bgmKeepAlive_ = 0;
      }
      DestroyVoice_(v);
      voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    // 3D: リスナーとエミッターの最新位置で定位・音量・ピッチを更新
    if (v.spatial) {
      ApplySpatial_(v);
    }
    ApplyVolume_(v);
    // ローパス（目標へ滑らかに）とリバーブの送り量（バス音量・ダッキングの変化にも追従）
    if (v.lowPassCurrent != v.lowPassTarget) {
      v.lowPassCurrent = SmoothLowPass_(v.lowPassCurrent, v.lowPassTarget, dt);
      ApplyVoiceFilter_(v);
    }
    ApplyReverbSend_(v);
  }

  // BGM: 持ち主不在による停止
  if (FindVoice_(bgmVoiceId_)) {
    if (bgmKeepAlive_ > 0) {
      --bgmKeepAlive_;
    } else {
      if (const Voice *bgm = FindVoice_(bgmVoiceId_)) {
        Log::Print("[Audio] BGM stopped (no owner): " + ClipPath(bgm->clip));
      }
      StopBgm(bgmFadeOutSec_);
    }
  }

  UpdateDucking_(dt);
  UpdateBusFilters_(dt);
}

// ================================================================
// クリップ
// ================================================================
std::string AudioEngine::NormalizePath_(const std::string &path) {
  // "./Resources\\Sounds/../Sounds/a.wav" → "Resources/Sounds/a.wav"
  std::string p = path;
  std::replace(p.begin(), p.end(), '\\', '/');
  return std::filesystem::path(p).lexically_normal().generic_string();
}

std::string AudioEngine::CacheKey_(const std::string &normalizedPath) {
  // Windows のファイルシステムは大文字小文字を区別しないので、キーは小文字に寄せる
  std::string k = normalizedPath;
  std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return k;
}

int AudioEngine::AllocClipSlot_() {
  for (size_t i = 0; i < clips_.size(); ++i) {
    if (!clips_[i].used) {
      return static_cast<int>(i);
    }
  }
  clips_.emplace_back();
  return static_cast<int>(clips_.size() - 1);
}

int AudioEngine::LoadClip(const std::string &rawPath, AudioLoadMode mode) {
  if (rawPath.empty()) {
    return -1;
  }
  const std::string path = NormalizePath_(rawPath);
  const std::string key = CacheKey_(path);

  // キャッシュ済み（参照 0 で残っているものも含む）
  if (auto it = clipByPath_.find(key); it != clipByPath_.end()) {
    clips_[it->second].refCount++;
    return it->second;
  }

  // ゲーム側 Resources → エンジン側 Resources の順で解決したパスで存在を確かめる（ローダーと同じ解決）
  std::error_code ec;
  const std::string resolved = SoundResolvePath(path.c_str());
  if (!std::filesystem::is_regular_file(std::filesystem::path(SoundPathToWide(resolved.c_str())), ec)) {
    Log::Print("[Audio] File not found: " + path);
    return -1;
  }

  // 読み込み方を決める。Auto なら長さを調べ、長いものはストリーミングにする
  bool streamed = (mode == AudioLoadMode::Stream);
  WAVEFORMATEX streamFmt{};
  uint64_t streamFrames = 0;
  if (mode != AudioLoadMode::Memory) {
    std::unique_ptr<AudioStreamReader> probe = AudioStreamReader::Open(path);
    if (!probe) {
      Log::Print("[Audio] Load failed (unsupported or broken file): " + path);
      return -1;
    }
    streamFmt = probe->Format();
    streamFrames = probe->TotalFrames();
    if (mode == AudioLoadMode::Auto) {
      const float seconds = static_cast<float>(streamFrames) / static_cast<float>(streamFmt.nSamplesPerSec);
      streamed = (streamFrames == 0) || seconds > streamThresholdSec_; // 長さ不明も安全側でストリーミング
    }
  }

  Clip loaded;
  if (streamed) {
    loaded.fmt = streamFmt;
    loaded.totalFrames = streamFrames;
    loaded.streamed = true;
  } else {
    SoundData data = SoundLoadAudio(path.c_str());
    if (!data.pBuffer || data.bufferSize == 0 || data.wfex.nChannels == 0 || data.wfex.nSamplesPerSec == 0 ||
        data.wfex.nBlockAlign == 0) {
      Log::Print("[Audio] Load failed (unsupported or broken file): " + path);
      return -1;
    }
    // XAudio2 はブロック単位で読むため、端数バイトは切り捨てておく
    data.bufferSize -= data.bufferSize % data.wfex.nBlockAlign;
    if (data.bufferSize == 0) {
      Log::Print("[Audio] Load failed (no audio frames): " + path);
      return -1;
    }
    loaded.fmt = data.wfex;
    loaded.totalFrames = data.bufferSize / data.wfex.nBlockAlign;
    loaded.data = std::move(data);
  }

  const int handle = AllocClipSlot_();
  Clip &c = clips_[handle];
  c = std::move(loaded);
  c.path = path;
  c.refCount = 1;
  c.used = true;
  clipByPath_[key] = handle;

  Log::Print(std::format("[Audio] Loaded clip #{}: {} ({:.1f}s, {}ch {}Hz, {})", handle, path, ClipDuration(handle),
                         c.fmt.nChannels, c.fmt.nSamplesPerSec, c.streamed ? "stream" : "memory"));
  return handle;
}

void AudioEngine::UnloadClip(int clip) {
  if (!IsValidClip(clip)) {
    return;
  }
  Clip &c = clips_[clip];
  if (c.refCount > 0) {
    c.refCount--;
  } else {
    // 参照の返し過ぎ。放っておくと本来より早く解放されて再生中に消えるので気付けるようにしておく
    Log::Print("[Audio] Warning: UnloadClip called on clip with no references: " + c.path);
  }
  // 参照 0 でも解放しない。エディタの再生/停止のたびにデコードし直さないように、
  // PurgeUnusedClips()（シーン遷移後）まではキャッシュに残す。
}

void AudioEngine::ReleaseClipRef_(int clip) {
  // ボイス側からの参照返却。挙動は UnloadClip と同じ
  UnloadClip(clip);
}

void AudioEngine::FreeClip_(Clip &c) {
  if (!c.used) {
    return;
  }
  Log::Print("[Audio] Freed clip: " + c.path);
  SoundUnload(&c.data);
  clipByPath_.erase(CacheKey_(c.path));
  c.path.clear();
  c.fmt = {};
  c.totalFrames = 0;
  c.streamed = false;
  c.refCount = 0;
  c.used = false;
}

void AudioEngine::PurgeUnusedClips() {
  for (auto &c : clips_) {
    if (c.used && c.refCount <= 0) {
      FreeClip_(c);
    }
  }
}

bool AudioEngine::IsValidClip(int clip) const {
  return clip >= 0 && static_cast<size_t>(clip) < clips_.size() && clips_[clip].used;
}

const std::string &AudioEngine::ClipPath(int clip) const {
  static const std::string kEmpty;
  return IsValidClip(clip) ? clips_[clip].path : kEmpty;
}

float AudioEngine::ClipDuration(int clip) const {
  if (!IsValidClip(clip)) {
    return 0.0f;
  }
  const Clip &c = clips_[clip];
  if (c.fmt.nSamplesPerSec == 0) {
    return 0.0f;
  }
  return static_cast<float>(static_cast<double>(c.totalFrames) / static_cast<double>(c.fmt.nSamplesPerSec));
}

int AudioEngine::ClipChannels(int clip) const {
  return IsValidClip(clip) ? static_cast<int>(clips_[clip].fmt.nChannels) : 0;
}

bool AudioEngine::IsClipStreamed(int clip) const { return IsValidClip(clip) && clips_[clip].streamed; }

size_t AudioEngine::ClipCount() const {
  size_t n = 0;
  for (const auto &c : clips_) {
    if (c.used) {
      ++n;
    }
  }
  return n;
}

// ================================================================
// ボイス共通
// ================================================================
AudioEngine::SourceVoicePtr AudioEngine::CreateSourceVoice_(int clip, AudioBus bus, AudioHrtf::Effect *hrtf) {
  if (!xaudio_ || !IsValidClip(clip)) {
    return nullptr;
  }
  const Clip &c = clips_[clip];

  // 送り先: バスのサブミックス（無ければマスターへ直接）と、共有リバーブ（あれば）
  IXAudio2Voice *target = submix_[static_cast<int>(bus)] ? static_cast<IXAudio2Voice *>(submix_[static_cast<int>(bus)].get())
                                                         : static_cast<IXAudio2Voice *>(master_.get());
  XAUDIO2_SEND_DESCRIPTOR send[2]{};
  send[0].Flags = 0;
  send[0].pOutputVoice = target;
  send[1].Flags = XAUDIO2_SEND_USEFILTER; // バスのこもりを響きにも掛けるため
  send[1].pOutputVoice = reverb_.get();
  XAUDIO2_VOICE_SENDS sends{};
  sends.SendCount = reverb_ ? 2u : 1u;
  sends.pSends = send;

  // HRTF: モノラル → ステレオの XAPO を挿す（ボイスの出力は 2ch になる）
  XAUDIO2_EFFECT_DESCRIPTOR fx{};
  XAUDIO2_EFFECT_CHAIN chain{};
  if (hrtf) {
    fx.pEffect = AudioHrtf::AsXapo(hrtf);
    fx.InitialState = TRUE;
    fx.OutputChannels = 2;
    chain.EffectCount = 1;
    chain.pEffectDescriptors = &fx;
  }

  IXAudio2SourceVoice *raw = nullptr;
  // XAUDIO2_VOICE_USEFILTER: ボイスごとのローパス（こもり）を使えるようにする
  HRESULT hr = xaudio_->CreateSourceVoice(&raw, &c.fmt, XAUDIO2_VOICE_USEFILTER, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr,
                                          target ? &sends : nullptr, hrtf ? &chain : nullptr);
  if (FAILED(hr) || !raw) {
    Log::Print(std::format("[Audio] CreateSourceVoice failed (hr=0x{:08X}): {}", static_cast<unsigned>(hr),
                           c.path));
    return nullptr;
  }
  return SourceVoicePtr(raw);
}

bool AudioEngine::BuildVoice_(Voice &v, uint64_t frame) {
  ReleaseSourceVoice_(v); // 作り直し（シーク / デバイス変更）なら古い方を先に捨てる
  if (!IsValidClip(v.clip)) {
    return false;
  }
  const Clip &c = clips_[v.clip];
  // 3D のモノラル音源は、HRTF が有効なら Steam Audio で立体化する（それ以外は X3DAudio のパンニング）
  if (v.spatial && v.bus != AudioBus::BGM && c.fmt.nChannels == 1 && IsHrtfActive()) {
    v.hrtf = AudioHrtf::CreateEffect();
  }
  SourceVoicePtr sv = CreateSourceVoice_(v.clip, v.bus, v.hrtf.get());
  if (!sv && v.hrtf) {
    Log::Print("[Audio] HRTF effect could not be attached. Falling back to panning: " + c.path);
    v.hrtf.reset();
    sv = CreateSourceVoice_(v.clip, v.bus, nullptr);
  }
  if (!sv) {
    return false;
  }
  v.dest = submix_[static_cast<int>(v.bus)] ? static_cast<IXAudio2Voice *>(submix_[static_cast<int>(v.bus)].get())
                                            : static_cast<IXAudio2Voice *>(master_.get());
  v.srcChannels = v.hrtf ? 2u : c.fmt.nChannels;
  v.hrtfTailSec = v.hrtf ? 0.05f : 0.0f;
  v.flatMatrix.clear(); // 出力構成が変わっている可能性があるので取り直す
  v.workMatrix.clear();
  v.playedOffset = 0;
  v.playedAtStop = 0;
  v.checkPlayedReset = false;

  if (c.streamed) {
    if (!v.stream) {
      v.stream = std::make_shared<StreamState>();
    }
    StreamState &s = *v.stream;
    if (!s.reader) {
      s.reader = AudioStreamReader::Open(c.path);
      if (!s.reader) {
        return false;
      }
      if (!SameFormat_(s.reader->Format(), c.fmt)) {
        Log::Print("[Audio] Stream format mismatch: " + c.path);
        s.reader.reset();
        return false;
      }
      s.blockAlign = c.fmt.nBlockAlign;
      size_t bytes = static_cast<size_t>(static_cast<float>(c.fmt.nAvgBytesPerSec) * kStreamBufferSeconds);
      bytes = (std::max)(bytes - bytes % s.blockAlign, static_cast<size_t>(s.blockAlign) * 256);
      for (auto &b : s.buffers) {
        b.assign(bytes, 0);
      }
      s.name = c.path;
    }
    if (c.totalFrames > 0 && frame > c.totalFrames) {
      frame = c.totalFrames;
    }
    {
      std::lock_guard<std::mutex> lk(s.mutex);
      s.loop = v.loop;
      if (!s.SeekOrReopen(frame)) {
        return false;
      }
      s.eos = false;
      s.loopedWithoutData = false;
      s.readFrame = frame;
      s.cumSubmitted = 0;
      {
        std::lock_guard<std::mutex> seg(s.segMutex);
        s.segments.clear();
      }
      s.next = 0;
      s.voice = sv.get();
      // 鳴らし始める前に 2 つだけ積む（0.5 秒分）。残りはストリーミングスレッドが補充する
      PumpStream_(s, 2);
    }
    {
      std::lock_guard<std::mutex> lk(streamMutex_);
      if (std::find(streams_.begin(), streams_.end(), v.stream) == streams_.end()) {
        streams_.push_back(v.stream);
      }
    }
    streamCv_.notify_one();
  } else {
    const uint64_t total = c.totalFrames;
    if (total == 0) {
      return false;
    }
    if (frame >= total) {
      frame = v.loop ? 0 : total - 1;
    }
    XAUDIO2_BUFFER buf{};
    buf.pAudioData = c.data.pBuffer.get();
    buf.AudioBytes = c.data.bufferSize;
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.PlayBegin = static_cast<UINT32>(frame);
    buf.PlayLength = 0; // 末尾まで
    // ループは常にクリップ全体（LoopBegin=0, LoopLength=0）。シーク後も 2 周目からは先頭に戻る
    buf.LoopCount = v.loop ? XAUDIO2_LOOP_INFINITE : 0;
    const HRESULT hr = sv->SubmitSourceBuffer(&buf);
    if (FAILED(hr)) {
      Log::Print(std::format("[Audio] SubmitSourceBuffer failed (hr=0x{:08X}): {}", static_cast<unsigned>(hr),
                             c.path));
      return false;
    }
  }
  v.startFrame = frame;
  v.voice = std::move(sv);

  // 3D: 鳴らし始める前に定位・距離減衰を反映しておく（最初の 1 フレームが 2D の大音量にならない）
  if (v.spatial) {
    if ((x3dReady_ || v.hrtf) && CaptureFlatMatrix_(v)) {
      ApplySpatial_(v);
    } else {
      v.spatial = false;
    }
  }
  ApplyVolume_(v);
  // フィルタは作り直し時も今の値で即座に反映し、リバーブ送りは既定（全量）を必ず上書きする
  v.lowPassCurrent = v.lowPassTarget;
  ApplyVoiceFilter_(v);
  v.appliedReverbGain = -1.0f;
  v.appliedSendLowPass = -1.0f;
  ApplyReverbSend_(v, true);

  if (!v.stopped) {
    const HRESULT hr = v.voice->Start();
    if (FAILED(hr)) {
      Log::Print(std::format("[Audio] Start failed (hr=0x{:08X}): {}", static_cast<unsigned>(hr), c.path));
      ReleaseSourceVoice_(v);
      return false;
    }
  }
  return true;
}

void AudioEngine::ReleaseSourceVoice_(Voice &v) {
  if (v.stream) {
    // ストリーミングスレッドが触らないようにしてから破棄する（バッファは StreamState が持ち続ける）
    std::lock_guard<std::mutex> lk(v.stream->mutex);
    v.stream->voice = nullptr;
  }
  v.voice.reset(); // 音声スレッドがバッファを読み終わるまでブロックする
  v.hrtf.reset();  // XAudio2 がエフェクトを手放した後に自分の参照を返す
}

void AudioEngine::UnregisterStream_(Voice &v) {
  if (!v.stream) {
    return;
  }
  {
    std::lock_guard<std::mutex> lk(v.stream->mutex);
    v.stream->voice = nullptr;
  }
  std::lock_guard<std::mutex> lk(streamMutex_);
  streams_.erase(std::remove(streams_.begin(), streams_.end(), v.stream), streams_.end());
}

void AudioEngine::DestroyVoice_(Voice &v) {
  // ボイスを先に破棄（音声スレッドが PCM を読み終わるのを待つ）→ その後クリップ参照を返す
  ReleaseSourceVoice_(v);
  UnregisterStream_(v);
  v.stream.reset();
  const int clip = v.clip;
  const AudioBus bus = v.bus;
  if (clip >= 0) {
    ReleaseClipRef_(clip);
  }
  v.clip = -1;
  v.id = -1;
  v.spatial = false;
  v.dest = nullptr;
  // BGM はデコード済み PCM が大きい（数十 MB）ので、どのコンポーネントも参照していなければ
  // PurgeUnusedClips() を待たずにここで解放する（前のシーンの BGM が差し替わったときなど）
  if (bus == AudioBus::BGM && IsValidClip(clip) && clips_[clip].refCount <= 0) {
    FreeClip_(clips_[clip]);
  }
}

bool AudioEngine::IsVoiceFinished_(const Voice &v) const {
  if (!v.voice) {
    return true;
  }
  // ストリームは eos を先に読む（後に読むと、空キューを見た直後に最後のバッファが積まれて
  // eos が立った場合に、まだ鳴っていない末尾ごと破棄してしまう）
  const bool eos = v.stream ? v.stream->eos.load() : true;
  XAUDIO2_VOICE_STATE st{};
  v.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
  // 補充が間に合わず一瞬空になっただけ（アンダーラン）は「終わり」ではない
  return eos && st.BuffersQueued == 0;
}

uint64_t AudioEngine::PlayedSamples_(const Voice &v) const {
  if (!v.voice) {
    return 0;
  }
  XAUDIO2_VOICE_STATE st{};
  v.voice->GetState(&st, 0);
  return st.SamplesPlayed + v.playedOffset;
}

uint64_t AudioEngine::CurrentFrame_(const Voice &v) const {
  if (!IsValidClip(v.clip)) {
    return 0;
  }
  const Clip &c = clips_[v.clip];
  const uint64_t played = PlayedSamples_(v);

  if (v.stream) {
    std::lock_guard<std::mutex> lk(v.stream->segMutex);
    const auto &segs = v.stream->segments;
    if (segs.empty()) {
      return v.startFrame;
    }
    for (const auto &sg : segs) {
      if (played < sg.cumStart + sg.frames) {
        return sg.fileStart + (played > sg.cumStart ? played - sg.cumStart : 0);
      }
    }
    const auto &last = segs.back();
    return last.fileStart + last.frames;
  }

  const uint64_t total = c.totalFrames;
  if (total == 0) {
    return 0;
  }
  const uint64_t start = (std::min)(v.startFrame, total);
  const uint64_t firstPass = total - start; // 開始位置から末尾まで
  if (played < firstPass) {
    return start + played;
  }
  if (!v.loop) {
    return total;
  }
  return (played - firstPass) % total;
}

void AudioEngine::ApplyVolume_(Voice &v) {
  if (!v.voice) {
    return;
  }
  const float g = v.volume * v.fadeGain * v.pauseGain;
  v.voice->SetVolume(std::isfinite(g) ? (std::max)(0.0f, g) : 0.0f);
}

void AudioEngine::UpdatePause_(Voice &v, float dt) {
  if (!v.voice) {
    return;
  }
  const bool wantPause = v.pausedUser || v.pausedGame;
  if (wantPause) {
    if (v.stopped) {
      return;
    }
    if (v.pauseGain <= 0.0f) {
      // 前のフレームで音量 0 を反映済み → ここで止める（位置は保持される）
      XAUDIO2_VOICE_STATE st{};
      v.voice->GetState(&st, 0);
      if (v.checkPlayedReset) {
        // 再開直後にまた止める場合も、SamplesPlayed の巻き戻り確認を先に済ませる
        v.checkPlayedReset = false;
        if (st.SamplesPlayed < v.playedAtStop) {
          v.playedOffset += v.playedAtStop;
        }
      }
      v.playedAtStop = st.SamplesPlayed;
      v.voice->Stop(0);
      v.stopped = true;
    } else {
      v.pauseGain = (std::max)(0.0f, v.pauseGain - dt / kPauseFadeSec);
    }
    return;
  }

  if (v.stopped) {
    v.voice->Start();
    v.stopped = false;
    v.checkPlayedReset = true;
  } else if (v.checkPlayedReset) {
    // SamplesPlayed が Start() で 0 に戻る実装でも再生位置がずれないように補正する
    v.checkPlayedReset = false;
    XAUDIO2_VOICE_STATE st{};
    v.voice->GetState(&st, 0);
    if (st.SamplesPlayed < v.playedAtStop) {
      v.playedOffset += v.playedAtStop;
    }
  }
  if (v.pauseGain < 1.0f) {
    v.pauseGain = (std::min)(1.0f, v.pauseGain + dt / kPauseFadeSec);
  }
}

void AudioEngine::StartFade_(Voice &v, float target, float seconds, FadeEnd end) {
  target = std::clamp(target, 0.0f, 1.0f);
  v.fadeEnd = end;
  v.fadeTime = 0.0f;
  if (!(seconds > 0.0f)) {
    v.fadeGain = target;
    v.fadeFrom = target;
    v.fadeTo = target;
    v.fadeDuration = 0.0f;
    return;
  }
  v.fadeFrom = v.fadeGain;
  v.fadeTo = target;
  v.fadeDuration = seconds;
}

bool AudioEngine::UpdateFade_(Voice &v, float dt) {
  if (v.fadeDuration <= 0.0f) {
    // フェードアウトが終わり、前のフレームで音量 0 を反映済み → 破棄する（クリック防止のため 1 フレーム待つ）
    return v.fadeEnd == FadeEnd::Stop && v.fadeGain <= 0.0f;
  }
  v.fadeTime += dt;
  const float t = std::clamp(v.fadeTime / v.fadeDuration, 0.0f, 1.0f);
  // 等パワーカーブ: 上げるときは sin、下げるときは cos。クロスフェードの合計音量がへこまない
  const float halfPi = X3DAUDIO_PI * 0.5f;
  if (v.fadeTo >= v.fadeFrom) {
    v.fadeGain = v.fadeFrom + (v.fadeTo - v.fadeFrom) * std::sin(t * halfPi);
  } else {
    v.fadeGain = v.fadeTo + (v.fadeFrom - v.fadeTo) * std::cos(t * halfPi);
  }
  if (t >= 1.0f) {
    v.fadeGain = v.fadeTo;
    v.fadeDuration = 0.0f;
  }
  return false;
}

AudioEngine::Voice *AudioEngine::FindVoice_(int voiceId) {
  if (voiceId < 0) {
    return nullptr;
  }
  for (auto &v : voices_) {
    if (v.id == voiceId) {
      return &v;
    }
  }
  return nullptr;
}

const AudioEngine::Voice *AudioEngine::FindVoice_(int voiceId) const {
  if (voiceId < 0) {
    return nullptr;
  }
  for (const auto &v : voices_) {
    if (v.id == voiceId) {
      return &v;
    }
  }
  return nullptr;
}

// ================================================================
// ストリーミング
// ================================================================
void AudioEngine::StreamThread_() {
  // Media Foundation のデコーダを触るので COM を初期化しておく
  const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  std::vector<std::shared_ptr<StreamState>> work;
  for (;;) {
    {
      // 一覧のコピーだけをロック中に取る。デコードは各ストリームのロックで行い、
      // メインスレッドの Play / Stop を長く待たせない
      std::unique_lock<std::mutex> lk(streamMutex_);
      // 先読みは 1 秒あるので 10ms 間隔の補充で十分間に合う
      streamCv_.wait_for(lk, std::chrono::milliseconds(10));
      if (streamQuit_) {
        break;
      }
      work = streams_;
    }
    for (auto &s : work) {
      if (s) {
        std::lock_guard<std::mutex> lk(s->mutex);
        PumpStream_(*s);
      }
    }
    work.clear(); // 参照を早めに返す（破棄されたボイスのバッファを持ち続けない）
  }
  if (SUCCEEDED(hrCom)) {
    CoUninitialize();
  }
}

void AudioEngine::PumpStream_(StreamState &s, uint32_t maxQueued) {
  if (!s.voice || !s.reader || s.blockAlign == 0) {
    return;
  }
  maxQueued = (std::min)(maxQueued, kStreamBufferCount);
  XAUDIO2_VOICE_STATE st{};
  s.voice->GetState(&st, 0);
  uint32_t queued = st.BuffersQueued;

  // 再生し終えた内訳を捨てる（最後の 1 つは位置の計算用に残す）
  {
    std::lock_guard<std::mutex> seg(s.segMutex);
    while (s.segments.size() > 1 && s.segments.front().cumStart + s.segments.front().frames <= st.SamplesPlayed) {
      s.segments.pop_front();
    }
  }

  while (queued < maxQueued && !s.eos.load()) {
    std::vector<BYTE> &buf = s.buffers[s.next];
    const size_t cap = buf.size();
    size_t filled = 0;
    while (filled < cap) {
      const size_t got = s.reader->Read(buf.data() + filled, cap - filled);
      if (got > 0) {
        s.readFrame += s.reader->TakeSeekCorrection(); // シークが目標を過ぎて着地した分
        const uint64_t frames = got / s.blockAlign;
        StreamState::Segment seg;
        seg.cumStart = s.cumSubmitted + filled / s.blockAlign;
        seg.fileStart = s.readFrame;
        seg.frames = frames;
        {
          std::lock_guard<std::mutex> segLock(s.segMutex);
          s.segments.push_back(seg);
        }
        s.readFrame += frames;
        filled += got;
        s.loopedWithoutData = false;
        continue;
      }
      // 読めなかった: 末尾ならループで先頭へ、そうでなければこのバッファはここまで
      if (s.loop && s.reader->IsEnd() && !s.loopedWithoutData) {
        s.loopedWithoutData = true; // 先頭に戻っても読めなければ空ファイルとみなして終わる
        if (!s.SeekOrReopen(0)) {
          Log::Print("[Audio] Stream loop failed (cannot rewind): " + s.name);
          break;
        }
        s.readFrame = 0;
        continue;
      }
      break;
    }
    if (filled == 0) {
      if (s.reader->IsEnd()) {
        s.eos = true;
      }
      break;
    }

    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = static_cast<UINT32>(filled);
    xb.pAudioData = buf.data();
    const bool last = !s.loop && s.reader->IsEnd();
    if (last) {
      xb.Flags = XAUDIO2_END_OF_STREAM;
    }
    if (FAILED(s.voice->SubmitSourceBuffer(&xb))) {
      Log::Print("[Audio] Stream submit failed: " + s.name);
      s.eos = true;
      break;
    }
    s.cumSubmitted += filled / s.blockAlign;
    s.next = (s.next + 1) % kStreamBufferCount;
    ++queued;
    if (last) {
      s.eos = true;
    }
  }
}

// ================================================================
// 再生
// ================================================================
int AudioEngine::Play(int clip, AudioBus bus, float volume, bool loop, const AudioEmitter *emitter, float fadeInSec,
                      float lowPassHz, float reverbSend) {
  if (bus == AudioBus::BGM) {
    PlayBgm(clip, volume, loop, fadeInSec, lowPassHz, reverbSend);
    return CurrentBgmVoice();
  }
  if (!xaudio_ || !IsValidClip(clip) || static_cast<int>(bus) < 0 || bus >= AudioBus::Count) {
    return -1;
  }
  Voice v;
  v.id = nextVoiceId_++;
  v.clip = clip;
  v.bus = bus;
  v.volume = (std::max)(0.0f, volume);
  v.loop = loop;
  if (emitter && emitter->spatialBlend > 0.0f && x3dReady_) {
    v.emitter = *emitter;
    v.spatial = true;
  }
  // こもりと響きは鳴らし始める前に設定する（最初の数 ms が素の音で漏れない）
  v.lowPassTarget = NormalizeLowPass_(lowPassHz);
  v.lowPassCurrent = v.lowPassTarget;
  v.reverbSend = std::isfinite(reverbSend) ? std::clamp(reverbSend, 0.0f, 1.0f) : 0.0f;
  if (fadeInSec > 0.0f) {
    v.fadeGain = 0.0f;
    StartFade_(v, 1.0f, fadeInSec, FadeEnd::None);
  }
  if (!BuildVoice_(v, 0)) {
    ReleaseSourceVoice_(v);
    UnregisterStream_(v);
    return -1; // クリップ参照はまだ増やしていない
  }
  clips_[clip].refCount++; // ボイスが生きている間はクリップを解放させない
  voices_.push_back(std::move(v));
  return voices_.back().id;
}

void AudioEngine::StopVoice(int voiceId, float fadeOutSec) {
  for (size_t i = 0; i < voices_.size(); ++i) {
    Voice &v = voices_[i];
    if (v.id != voiceId) {
      continue;
    }
    const bool audible = v.voice && !v.stopped && !v.pausedUser && !v.pausedGame;
    if (v.id == bgmVoiceId_) {
      // フェードアウト中はもう「現在の BGM」ではない（同じ曲の再要求は PlayBgm が呼び戻す）
      bgmVoiceId_ = -1;
      bgmKeepAlive_ = 0;
    }
    if (fadeOutSec > 0.0f && audible) {
      StartFade_(v, 0.0f, fadeOutSec, FadeEnd::Stop); // Update() でフェードし終えたら破棄
      return;
    }
    DestroyVoice_(v);
    voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
    return;
  }
}

bool AudioEngine::IsVoicePlaying(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  return v && !IsVoiceFinished_(*v);
}

bool AudioEngine::IsVoiceLooping(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  return v && v->loop && !IsVoiceFinished_(*v);
}

bool AudioEngine::IsVoiceStreaming(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  return v && v->stream != nullptr;
}

void AudioEngine::SetVoiceVolume(int voiceId, float volume) {
  if (Voice *v = FindVoice_(voiceId)) {
    v->volume = std::isfinite(volume) ? (std::max)(0.0f, volume) : 0.0f;
    ApplyVolume_(*v);
  }
}

void AudioEngine::SetVoiceLowPass(int voiceId, float cutoffHz, bool immediate) {
  Voice *v = FindVoice_(voiceId);
  if (!v) {
    return;
  }
  v->lowPassTarget = NormalizeLowPass_(cutoffHz);
  if (immediate && v->lowPassCurrent != v->lowPassTarget) {
    v->lowPassCurrent = v->lowPassTarget;
    ApplyVoiceFilter_(*v);
  }
}

void AudioEngine::SetVoiceReverbSend(int voiceId, float level) {
  if (Voice *v = FindVoice_(voiceId)) {
    v->reverbSend = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
    ApplyReverbSend_(*v);
  }
}

void AudioEngine::ApplyVoiceFilter_(Voice &v) {
  if (!v.voice) {
    return;
  }
  const XAUDIO2_FILTER_PARAMETERS p = LowPassParams_(v.lowPassCurrent, outputSampleRate_);
  v.voice->SetFilterParameters(&p);
}

float AudioEngine::BusGain_(AudioBus bus) const {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= static_cast<int>(AudioBus::Count)) {
    return 0.0f;
  }
  const uint32_t mask = ducking_.enabled ? (ducking_.targetMask & ~AudioBusBit(AudioBus::Voice)) : 0u;
  const bool ducked = (mask & AudioBusBit(bus)) != 0;
  return busVolume_[i] * (ducked ? duckGain_ : 1.0f);
}

void AudioEngine::ApplyReverbSend_(Voice &v, bool force) {
  if (!v.voice || !reverb_ || v.srcChannels == 0 || reverbChannels_ == 0) {
    return;
  }
  // バスのこもりを響きの送りにも掛ける（バスを通らないので送り側のフィルタで揃える）
  const float busLp = busLowPassCurrent_[static_cast<int>(v.bus)];
  if (force || v.appliedSendLowPass != busLp) {
    v.appliedSendLowPass = busLp;
    const XAUDIO2_FILTER_PARAMETERS p = LowPassParams_(busLp, outputSampleRate_);
    v.voice->SetOutputFilterParameters(reverb_.get(), &p);
  }
  // 共有リバーブはバスを通らないので、バス音量・ダッキング・3D の距離減衰をここで掛ける
  float g = 0.0f;
  if (room_ != AudioRoom::None && v.reverbSend > 0.0f) {
    g = v.reverbSend * BusGain_(v.bus) * v.spatialGain;
  }
  if (!force && std::fabs(g - v.appliedReverbGain) < 1e-4f) {
    return;
  }
  v.appliedReverbGain = g;
  const uint32_t src = v.srcChannels;
  const uint32_t rc = reverbChannels_;
  std::vector<float> m(static_cast<size_t>(src) * rc, 0.0f);
  for (uint32_t s = 0; s < src; ++s) {
    if (rc == 1) {
      m[s] = g / std::sqrt(static_cast<float>(src)); // モノラルの響きへまとめる
      continue;
    }
    if (src == 1) {
      m[s] = g; // モノラル → 両チャンネル
      m[src + s] = g;
    } else if (src == 2) {
      m[src * s + s] = g; // ステレオ → そのまま
    } else if (s == 3) {
      // LFE は響かせない
    } else if (s == 2) {
      m[s] = g * 0.7071f; // センター → 両チャンネル
      m[src + s] = g * 0.7071f;
    } else {
      m[src * (s % 2) + s] = g * 0.7071f; // L 系 → 左 / R 系 → 右
    }
  }
  v.voice->SetOutputMatrix(reverb_.get(), src, rc, m.data());
}

void AudioEngine::FadeVoice(int voiceId, float targetGain, float seconds) {
  if (Voice *v = FindVoice_(voiceId)) {
    StartFade_(*v, targetGain, seconds, FadeEnd::None);
    ApplyVolume_(*v);
  }
}

void AudioEngine::PauseVoice(int voiceId) {
  if (Voice *v = FindVoice_(voiceId)) {
    v->pausedUser = true;
  }
}

void AudioEngine::ResumeVoice(int voiceId) {
  if (Voice *v = FindVoice_(voiceId)) {
    v->pausedUser = false;
  }
}

bool AudioEngine::IsVoicePaused(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  return v && (v->pausedUser || v->pausedGame);
}

bool AudioEngine::IsVoicePausedByUser(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  return v && v->pausedUser;
}

float AudioEngine::GetVoiceTime(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  if (!v || !IsValidClip(v->clip)) {
    return 0.0f;
  }
  const uint32_t sr = clips_[v->clip].fmt.nSamplesPerSec;
  if (sr == 0) {
    return 0.0f;
  }
  return static_cast<float>(static_cast<double>(CurrentFrame_(*v)) / static_cast<double>(sr));
}

uint32_t AudioEngine::GetVoiceSeekCount(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  return v ? v->seekCount : 0u;
}

float AudioEngine::GetVoiceDuration(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  return v ? ClipDuration(v->clip) : 0.0f;
}

void AudioEngine::SeekVoice(int voiceId, float seconds) {
  Voice *v = FindVoice_(voiceId);
  if (!v || !IsValidClip(v->clip)) {
    return;
  }
  const Clip &c = clips_[v->clip];
  const double sec = std::isfinite(seconds) ? (std::max)(0.0, static_cast<double>(seconds)) : 0.0;
  uint64_t frame = static_cast<uint64_t>(sec * static_cast<double>(c.fmt.nSamplesPerSec));
  if (c.totalFrames > 0) {
    frame = (std::min)(frame, c.totalFrames);
  }
  if (!v->stopped) {
    v->pauseGain = 0.0f; // 新しい位置は短くフェードインして鳴らす（飛んだ瞬間のクリック防止）
  }
  ++v->seekCount;
  if (!BuildVoice_(*v, frame)) {
    StopVoice(voiceId);
  }
}

void AudioEngine::SetVoiceEmitter(int voiceId, const AudioEmitter &emitter) {
  Voice *v = FindVoice_(voiceId);
  if (!v || !v->voice || v->bus == AudioBus::BGM) {
    return;
  }
  if (!v->spatial) {
    // 2D のボイスに「2D のまま」を伝えられただけなら何もしない（軽量経路）
    if (emitter.spatialBlend <= 0.0f || !x3dReady_) {
      return;
    }
    if (!CaptureFlatMatrix_(*v)) {
      return;
    }
    v->spatial = true;
  }
  v->emitter = emitter; // 反映は次の Update()。spatialBlend が 0 ならそこで 2D に戻る
}

bool AudioEngine::IsVoiceSpatial(int voiceId) const {
  const Voice *v = FindVoice_(voiceId);
  return v && v->spatial;
}

void AudioEngine::StopAllSe(float fadeOutSec) {
  std::vector<int> ids;
  for (const auto &v : voices_) {
    if (v.bus != AudioBus::BGM) {
      ids.push_back(v.id);
    }
  }
  for (int id : ids) {
    StopVoice(id, fadeOutSec);
  }
}

size_t AudioEngine::ActiveSeCount() const {
  size_t n = 0;
  for (const auto &v : voices_) {
    if (v.bus != AudioBus::BGM && v.voice) {
      ++n;
    }
  }
  return n;
}

size_t AudioEngine::ActiveSpatialCount() const {
  size_t n = 0;
  for (const auto &v : voices_) {
    if (v.spatial) {
      ++n;
    }
  }
  return n;
}

size_t AudioEngine::ActiveHrtfCount() const {
  size_t n = 0;
  for (const auto &v : voices_) {
    if (v.hrtf) {
      ++n;
    }
  }
  return n;
}

bool AudioEngine::IsHrtfAvailable() const { return AudioHrtf::IsAvailable(); }

bool AudioEngine::IsHrtfActive() const {
  if (!AudioHrtf::IsAvailable() || dstChannels_ == 0) {
    return false;
  }
  switch (spatializer_) {
  case AudioSpatializer::Panning: return false;
  case AudioSpatializer::Hrtf: return true;
  case AudioSpatializer::Auto:
  default: return dstChannels_ == 2; // ステレオ出力（ヘッドホン想定）のときだけ
  }
}

void AudioEngine::SetSpatializer(AudioSpatializer mode) {
  if (static_cast<int>(mode) < 0 || mode >= AudioSpatializer::Count || mode == spatializer_) {
    return;
  }
  spatializer_ = mode;
  RebuildSpatialVoices_();
}

void AudioEngine::RebuildSpatialVoices_() {
  if (!xaudio_) {
    return;
  }
  const bool want = IsHrtfActive();
  for (size_t i = voices_.size(); i-- > 0;) {
    Voice &v = voices_[i];
    if (!v.voice || !v.spatial || v.bus == AudioBus::BGM || !IsValidClip(v.clip) ||
        clips_[v.clip].fmt.nChannels != 1 || want == (v.hrtf != nullptr)) {
      continue;
    }
    const uint64_t frame = CurrentFrame_(v);
    if (!v.stopped) {
      v.pauseGain = 0.0f; // 作り直した位置から短くフェードインする
    }
    if (!BuildVoice_(v, frame)) {
      if (v.id == bgmVoiceId_) {
        bgmVoiceId_ = -1;
      }
      DestroyVoice_(v);
      voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }
}

size_t AudioEngine::ActiveStreamCount() const {
  size_t n = 0;
  for (const auto &v : voices_) {
    if (v.stream) {
      ++n;
    }
  }
  return n;
}

// ================================================================
// BGM
// ================================================================
void AudioEngine::PlayBgm(int clip, float volume, bool loop, float fadeSec, float lowPassHz, float reverbSend) {
  if (!xaudio_ || !IsValidClip(clip)) {
    return;
  }
  fadeSec = (std::max)(0.0f, fadeSec);
  // 同じ曲なら継続。音量だけ反映する
  if (Voice *cur = FindVoice_(bgmVoiceId_)) {
    if (cur->clip == clip) {
      KeepBgmAlive(clip, volume, bgmFadeOutSec_);
      return;
    }
    // 違う曲 → 今の曲はフェードアウトして止める（0 秒なら即時）
    StopVoice(cur->id, fadeSec);
    bgmVoiceId_ = -1;
  }

  // 同じ曲がフェードアウト中なら、止めずに呼び戻す（シーン遷移で一瞬持ち主がいなくなった場合など）
  for (auto &v : voices_) {
    if (v.bus == AudioBus::BGM && v.clip == clip && v.fadeEnd == FadeEnd::Stop && v.voice) {
      StartFade_(v, 1.0f, (std::max)(fadeSec, 0.05f), FadeEnd::None);
      v.volume = (std::max)(0.0f, volume);
      bgmVoiceId_ = v.id;
      bgmKeepAlive_ = kBgmGraceFrames;
      return;
    }
  }

  Voice v;
  v.id = nextVoiceId_++;
  v.clip = clip;
  v.bus = AudioBus::BGM;
  v.volume = (std::max)(0.0f, volume);
  v.loop = loop;
  v.lowPassTarget = NormalizeLowPass_(lowPassHz);
  v.lowPassCurrent = v.lowPassTarget;
  v.reverbSend = std::isfinite(reverbSend) ? std::clamp(reverbSend, 0.0f, 1.0f) : 0.0f;
  v.spatial = false; // BGM は常に 2D
  if (fadeSec > 0.0f) {
    v.fadeGain = 0.0f;
    StartFade_(v, 1.0f, fadeSec, FadeEnd::None);
  }
  if (!BuildVoice_(v, 0)) {
    ReleaseSourceVoice_(v);
    UnregisterStream_(v);
    return;
  }
  clips_[clip].refCount++;
  bgmVoiceId_ = v.id;
  bgmKeepAlive_ = kBgmGraceFrames;
  voices_.push_back(std::move(v));
  Log::Print("[Audio] BGM start: " + clips_[clip].path);
}

void AudioEngine::KeepBgmAlive(int clip, float volume, float fadeOutSec) {
  Voice *v = FindVoice_(bgmVoiceId_);
  if (!v || v->clip != clip) {
    return;
  }
  bgmKeepAlive_ = kBgmGraceFrames;
  bgmFadeOutSec_ = (std::max)(0.0f, fadeOutSec);
  const float vol = (std::max)(0.0f, volume);
  if (vol != v->volume) {
    v->volume = vol;
    ApplyVolume_(*v);
  }
}

void AudioEngine::StopBgm(float fadeOutSec) {
  const int id = bgmVoiceId_;
  bgmVoiceId_ = -1;
  bgmKeepAlive_ = 0;
  if (id >= 0) {
    StopVoice(id, fadeOutSec);
  }
}

int AudioEngine::CurrentBgmClip() const {
  const Voice *v = FindVoice_(bgmVoiceId_);
  return v ? v->clip : -1;
}

// ================================================================
// 一時停止（ゲーム全体）
// ================================================================
void AudioEngine::SetPaused(bool paused) {
  paused_ = paused;
  ApplyGamePause_();
}

void AudioEngine::SetEditorPaused(bool paused) {
  editorPaused_ = paused;
  ApplyGamePause_();
}

void AudioEngine::ApplyGamePause_() {
  const bool eff = paused_ || editorPaused_;
  if (eff == appliedGamePause_) {
    return;
  }
  appliedGamePause_ = eff;
  for (auto &v : voices_) {
    if (eff) {
      // 一時停止した時点で鳴っている音だけを止める。以降に鳴らした音（メニューの UI 音）は止めない
      if (v.bus != AudioBus::BGM || pauseAffectsBgm_) {
        v.pausedGame = true;
      }
    } else {
      v.pausedGame = false;
    }
  }
}

// ================================================================
// 3D 音響
// ================================================================
void AudioEngine::SetListener(const AudioListenerPose &pose) {
  AudioListenerPose p = pose;
  Orthonormalize_(p.forward, p.up);
  if (!IsFinite_(p.position)) {
    p.position = {0.0f, 0.0f, 0.0f};
  }
  if (!IsFinite_(p.velocity)) {
    p.velocity = {0.0f, 0.0f, 0.0f};
  }
  listener_ = p;
  listenerPrevPos_ = p.position;
  listenerHasPrev_ = true;
}

void AudioEngine::SetListenerPose(const RC::Vector3 &position, const RC::Vector3 &forward, const RC::Vector3 &up,
                                  float dt) {
  AudioListenerPose p;
  p.position = IsFinite_(position) ? position : RC::Vector3{0.0f, 0.0f, 0.0f};
  p.forward = forward;
  p.up = up;
  Orthonormalize_(p.forward, p.up);
  p.velocity = listenerHasPrev_ ? EstimateVelocity_(listenerPrevPos_, p.position, dt, speedOfSound_ * 0.5f)
                                : RC::Vector3{0.0f, 0.0f, 0.0f};
  listener_ = p;
  listenerPrevPos_ = p.position;
  listenerHasPrev_ = true;
}

void AudioEngine::SetListenerFromView(const RC::Matrix4x4 &view, float dt) {
  const AudioListenerPose p = PoseFromViewMatrix(view);
  SetListenerPose(p.position, p.forward, p.up, dt);
}

void AudioEngine::SetSpeedOfSound(float unitsPerSecond) {
  const float v = (std::max)(1.0f, unitsPerSecond);
  if (v == speedOfSound_) {
    return;
  }
  speedOfSound_ = v;
  if (xaudio_) {
    InitX3D_(); // 音速はインスタンスに焼き込まれるので作り直す
  }
}

AudioListenerPose AudioEngine::PoseFromWorldMatrix(const RC::Matrix4x4 &world) {
  // 行ベクトル規約（v * M）: 行 0..2 がローカル軸のワールド方向、行 3 が位置
  AudioListenerPose p;
  p.position = {world.m[3][0], world.m[3][1], world.m[3][2]};
  p.up = {world.m[1][0], world.m[1][1], world.m[1][2]};
  p.forward = {world.m[2][0], world.m[2][1], world.m[2][2]};
  Orthonormalize_(p.forward, p.up); // スケールを落として正規直交にする
  if (!IsFinite_(p.position)) {
    p.position = {0.0f, 0.0f, 0.0f};
  }
  return p;
}

AudioListenerPose AudioEngine::PoseFromViewMatrix(const RC::Matrix4x4 &view) {
  // ビュー行列（ワールド → カメラ）の逆行列がカメラのワールド行列
  return PoseFromWorldMatrix(::Inverse(view));
}

float AudioEngine::ComputeRolloff(float distance, float minDistance, float maxDistance, AudioRolloff rolloff) {
  const float minD = (std::max)(minDistance, 0.001f);
  const float maxD = (std::max)(maxDistance, minD + 0.001f);
  if (!(distance > minD)) {
    return 1.0f; // NaN もここで 1 に落とす
  }
  if (distance >= maxD) {
    return 0.0f;
  }
  switch (rolloff) {
  case AudioRolloff::Linear:
    return std::clamp(1.0f - (distance - minD) / (maxD - minD), 0.0f, 1.0f);
  case AudioRolloff::Logarithmic:
  default: {
    // 1/d を「minD で 1、maxD で 0」になるように平行移動して正規化する。
    // maxD >> minD なら実質 minD / d（-6dB / 距離 2 倍）。maxD で不連続にならず 0 に着地する
    const float inv = minD / distance;
    const float invMax = minD / maxD;
    return std::clamp((inv - invMax) / (1.0f - invMax), 0.0f, 1.0f);
  }
  }
}

bool AudioEngine::CaptureFlatMatrix_(Voice &v) {
  if (!v.voice || v.srcChannels == 0 || dstChannels_ == 0) {
    return false;
  }
  const size_t n = static_cast<size_t>(v.srcChannels) * dstChannels_;
  if (v.flatMatrix.size() == n) {
    return true; // 取得済み
  }
  // 作成直後の出力行列 = XAudio2 の既定のチャンネル割り当て（モノラル → 両方、ステレオ → そのまま 等）。
  // これを「2D のときの音」として、3D の行列とのブレンドの基準にする
  v.flatMatrix.assign(n, 0.0f);
  v.voice->GetOutputMatrix(v.dest, v.srcChannels, dstChannels_, v.flatMatrix.data());
  v.workMatrix = v.flatMatrix;
  return true;
}

void AudioEngine::ResetSpatial_(Voice &v) {
  if (v.voice && v.srcChannels > 0 && dstChannels_ > 0 &&
      v.flatMatrix.size() == static_cast<size_t>(v.srcChannels) * dstChannels_) {
    v.voice->SetOutputMatrix(v.dest, v.srcChannels, dstChannels_, v.flatMatrix.data());
    v.voice->SetFrequencyRatio(1.0f);
  }
  if (v.hrtf) {
    AudioHrtf::SetParams(v.hrtf.get(), 0.0f, 0.0f, 1.0f, 0.0f); // 立体化しない（両耳に同じ音）
  }
  v.spatialGain = 1.0f;
  v.spatial = false;
}

/// @brief 近接効果（耳元）: 音源が近く、かつ横にあるほど遠い側の耳を下げる
/// @details ステレオ出力（ヘッドホン想定）のときだけ。頭が音を遮る効果（両耳間レベル差）の簡易版で、
///          片耳だけに囁かれている感覚を強める。HRTF でも遠距離用のデータしか無いので、耳元では上乗せが効く。
/// @param m        出力行列（[src × dst]。dst 行 d・src 列 s が m[src * d + s]）
/// @param src      ボイスの出力チャンネル数
/// @param dst      出力チャンネル数
/// @param em       エミッター
/// @param l        リスナー
/// @param rel      リスナー → 音源のベクトル
/// @param distance その長さ
/// @param blend    2D/3D ブレンド
static void ApplyNearField_(std::vector<float> &m, uint32_t src, uint32_t dst, const AudioEmitter &em,
                            const AudioListenerPose &l, const RC::Vector3 &rel, float distance, float blend) {
  const float nearDist = em.nearFieldDistance;
  if (dst != 2 || !(nearDist > 0.0f) || !(em.nearFieldIldDb > 0.0f) || !(distance > 1e-5f) || distance >= nearDist) {
    return;
  }
  const RC::Vector3 right = Cross_(l.up, l.forward); // 左手系: up × forward = 右
  const float lateral = std::clamp(Dot_(rel, right) / distance, -1.0f, 1.0f);
  float closeness = 1.0f - distance / nearDist;
  closeness = closeness * closeness * (3.0f - 2.0f * closeness); // smoothstep
  const float dropDb = em.nearFieldIldDb * closeness * std::fabs(lateral) * blend;
  const float farGain = DbToGain_(-dropDb);
  const uint32_t farCh = (lateral > 0.0f) ? 0u : 1u; // 右にある音源なら左耳（ch0）が遠い
  for (uint32_t s = 0; s < src; ++s) {
    m[static_cast<size_t>(src) * farCh + s] *= farGain;
  }
}

void AudioEngine::ApplySpatial_(Voice &v) {
  if (!v.voice || !v.spatial) {
    return;
  }
  const uint32_t src = v.srcChannels;
  const uint32_t dst = dstChannels_;
  const size_t n = static_cast<size_t>(src) * dst;
  if (src == 0 || dst == 0 || v.flatMatrix.size() != n) {
    v.spatial = false;
    return;
  }
  const float blend = std::clamp(v.emitter.spatialBlend, 0.0f, 1.0f);
  if (blend <= 0.0f || (!x3dReady_ && !v.hrtf)) {
    ResetSpatial_(v); // 2D に戻す（Inspector で 3D を切った / X3DAudio が無い）
    return;
  }

  // --- HRTF（Steam Audio）---
  // 方向は XAPO に渡して HRTF で立体化し、距離減衰と近接効果だけを出力行列で掛ける。ドップラーは使わない
  if (v.hrtf) {
    const RC::Vector3 rel = Sub_(v.emitter.position, listener_.position);
    const float distance = Length_(rel);
    const float atten = ComputeRolloff(distance, v.emitter.minDistance, v.emitter.maxDistance, v.emitter.rolloff);
    const RC::Vector3 right = Cross_(listener_.up, listener_.forward);
    const RC::Vector3 local =
        Normalize_({Dot_(rel, right), Dot_(rel, listener_.up), Dot_(rel, listener_.forward)}, {0.0f, 0.0f, 1.0f});
    // innerRadius の内側では立体感を弱める（頭の中を通過しても定位が暴れない）
    float hrtfBlend = blend;
    const float innerR = std::isfinite(v.emitter.innerRadius) ? (std::max)(v.emitter.innerRadius, 0.0f) : 0.0f;
    if (innerR > 0.0f && distance < innerR) {
      hrtfBlend *= distance / innerR;
    }
    AudioHrtf::SetParams(v.hrtf.get(), local.x, local.y, local.z, hrtfBlend);

    float g = (1.0f - blend) + atten * blend;
    v.spatialGain = std::isfinite(g) ? g : 0.0f;
    g = std::isfinite(g) ? std::clamp(g, 0.0f, kMaxVolume) : 0.0f;
    v.workMatrix.assign(n, 0.0f);
    if (dst == 1) {
      v.workMatrix[0] = g * 0.5f; // モノラル出力: L+R
      v.workMatrix[1] = g * 0.5f;
    } else {
      v.workMatrix[0] = g;           // L → 出力 ch0
      v.workMatrix[src * 1 + 1] = g; // R → 出力 ch1
    }
    ApplyNearField_(v.workMatrix, src, dst, v.emitter, listener_, rel, distance, blend);
    v.voice->SetOutputMatrix(v.dest, src, dst, v.workMatrix.data());
    v.voice->SetFrequencyRatio(1.0f);
    return;
  }

  // --- リスナー ---
  X3DAUDIO_LISTENER listener{};
  listener.OrientFront = ToX3D_(listener_.forward);
  listener.OrientTop = ToX3D_(listener_.up);
  listener.Position = ToX3D_(listener_.position);
  listener.Velocity = ToX3D_(listener_.velocity);
  listener.pCone = nullptr;

  // --- エミッター ---
  // 距離減衰は ComputeRolloff() で自前計算する（カーブの意味を Inspector で説明しやすくするため）。
  // X3DAudio には平坦なカーブを渡し、定位（出力行列）とドップラーだけを計算させる
  X3DAUDIO_DISTANCE_CURVE_POINT flatPts[2] = {{0.0f, 1.0f}, {1.0f, 1.0f}};
  X3DAUDIO_DISTANCE_CURVE flatCurve{flatPts, 2};

  // マルチチャンネル音源（ステレオ SE など）は、各チャンネルを音源位置の周りに配置する。
  // 方位はエミッターの正面基準・時計回り。2π は LFE 扱いになるので使わない
  std::vector<float> azimuths;
  if (src > 1) {
    azimuths.resize(src);
    if (src == 2) {
      azimuths[0] = X3DAUDIO_PI * 1.5f; // L: 左 90°
      azimuths[1] = X3DAUDIO_PI * 0.5f; // R: 右 90°
    } else {
      for (uint32_t i = 0; i < src; ++i) {
        azimuths[i] = X3DAUDIO_2PI * static_cast<float>(i) / static_cast<float>(src);
      }
    }
  }

  const float minD = (std::max)(v.emitter.minDistance, 0.0f);
  const float innerR = std::isfinite(v.emitter.innerRadius) ? (std::max)(v.emitter.innerRadius, 0.0f) : minD;
  X3DAUDIO_EMITTER em{};
  em.pCone = nullptr;
  // 音源の向きはリスナーと揃える: ステレオ音源の L/R がそのままリスナーの左右になる
  // （モノラル音源では向きは定位に影響しない）
  em.OrientFront = listener.OrientFront;
  em.OrientTop = listener.OrientTop;
  em.Position = ToX3D_(v.emitter.position);
  em.Velocity = ToX3D_(v.emitter.velocity);
  // innerRadius の内側では全スピーカーに滲ませる（リスナーの真横・真上を通過してもパンが暴れない）。
  // 耳元の定位を残したい場合は小さくする
  em.InnerRadius = innerR;
  em.InnerRadiusAngle = X3DAUDIO_PI / 4.0f;
  em.ChannelCount = src;
  em.ChannelRadius = (src > 1) ? (std::max)((std::min)(minD, innerR > 0.0f ? innerR * 4.0f : minD) * 0.5f, 0.02f) : 0.0f;
  em.pChannelAzimuths = (src > 1) ? azimuths.data() : nullptr;
  em.pVolumeCurve = &flatCurve;
  em.pLFECurve = nullptr;
  em.pLPFDirectCurve = nullptr;
  em.pLPFReverbCurve = nullptr;
  em.pReverbCurve = nullptr;
  em.CurveDistanceScaler = 1.0f; // 平坦カーブなので実質未使用（FLT_MIN 以上であればよい）
  em.DopplerScaler = (std::max)(v.emitter.dopplerLevel, 0.0f);

  // --- 計算 ---
  v.workMatrix.assign(n, 0.0f);
  X3DAUDIO_DSP_SETTINGS dsp{};
  dsp.pMatrixCoefficients = v.workMatrix.data();
  dsp.pDelayTimes = nullptr;
  dsp.SrcChannelCount = src;
  dsp.DstChannelCount = dst;
  UINT32 flags = X3DAUDIO_CALCULATE_MATRIX;
  const bool doppler = em.DopplerScaler > 0.0f;
  if (doppler) {
    flags |= X3DAUDIO_CALCULATE_DOPPLER;
  }
  X3DAudioCalculate(x3dInstance_, &listener, &em, flags, &dsp);

  // --- 出力行列 ---
  // X3DAudio の等パワーパンは 2D の既定割り当てより小さい値を返す（モノラル → ステレオ正面で 0.707）。
  // 合計エネルギーを 2D の行列に揃え、spatialBlend を動かしても音量が跳ばないようにする
  double flatEnergy = 0.0;
  double panEnergy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    flatEnergy += static_cast<double>(v.flatMatrix[i]) * v.flatMatrix[i];
    panEnergy += static_cast<double>(v.workMatrix[i]) * v.workMatrix[i];
  }
  float norm = 0.0f;
  if (panEnergy > 1e-12 && std::isfinite(panEnergy)) {
    norm = (std::min)(static_cast<float>(std::sqrt(flatEnergy / panEnergy)), 4.0f);
  }

  const RC::Vector3 rel = Sub_(v.emitter.position, listener_.position);
  const float distance = Length_(rel);
  const float atten = ComputeRolloff(distance, v.emitter.minDistance, v.emitter.maxDistance, v.emitter.rolloff);
  const float gain3d = norm * atten * blend;
  v.spatialGain = std::clamp((1.0f - blend) + atten * blend, 0.0f, 1.0f);
  const float gain2d = 1.0f - blend;
  for (size_t i = 0; i < n; ++i) {
    float m = v.flatMatrix[i] * gain2d + v.workMatrix[i] * gain3d;
    if (!std::isfinite(m)) {
      m = 0.0f;
    }
    v.workMatrix[i] = std::clamp(m, 0.0f, kMaxVolume);
  }

  // --- 近接効果（耳元）---
  ApplyNearField_(v.workMatrix, src, dst, v.emitter, listener_, rel, distance, blend);
  v.voice->SetOutputMatrix(v.dest, src, dst, v.workMatrix.data());

  // --- ドップラー（ピッチ）---
  float ratio = 1.0f;
  if (doppler) {
    float f = dsp.DopplerFactor;
    if (!std::isfinite(f) || !(f > 0.0f)) {
      f = 1.0f;
    }
    // ボイスは XAUDIO2_DEFAULT_FREQ_RATIO(2.0) で作っているので、その範囲に収める
    f = std::clamp(f, 0.5f, kMaxFreqRatio);
    ratio = 1.0f + (f - 1.0f) * blend;
  }
  v.voice->SetFrequencyRatio(ratio);
}

// ================================================================
// ミキサー
// ================================================================
void AudioEngine::ApplyBusVolumes_() {
  const uint32_t mask = ducking_.enabled ? (ducking_.targetMask & ~AudioBusBit(AudioBus::Voice)) : 0u;
  for (int i = 0; i < static_cast<int>(AudioBus::Count); ++i) {
    if (!submix_[i]) {
      continue;
    }
    const bool ducked = (mask & (1u << static_cast<uint32_t>(i))) != 0;
    submix_[i]->SetVolume(busVolume_[i] * (ducked ? duckGain_ : 1.0f));
  }
}

void AudioEngine::UpdateDucking_(float dt) {
  bool voiceActive = false;
  if (ducking_.enabled) {
    for (const auto &v : voices_) {
      if (v.bus == AudioBus::Voice && v.voice && !v.stopped && !v.pausedUser && !v.pausedGame &&
          v.volume * v.fadeGain > 0.001f) {
        voiceActive = true;
        break;
      }
    }
  }
  const float target = voiceActive ? DbToGain_((std::min)(ducking_.depthDb, 0.0f)) : 1.0f;
  const float tau = (target < duckGain_) ? ducking_.attackSec : ducking_.releaseSec;
  if (!(tau > 0.0f)) {
    duckGain_ = target;
  } else {
    // 指数的に近づける。tau 秒で約 95% 到達（時定数 = tau / 3）
    const float k = 1.0f - std::exp(-dt * 3.0f / tau);
    duckGain_ += (target - duckGain_) * k;
    if (std::fabs(duckGain_ - target) < 1e-4f) {
      duckGain_ = target;
    }
  }
  ApplyBusVolumes_();
}

void AudioEngine::SetDucking(const AudioDuckingSettings &settings) {
  ducking_ = settings;
  ducking_.depthDb = std::isfinite(ducking_.depthDb) ? std::clamp(ducking_.depthDb, -60.0f, 0.0f) : -9.0f;
  ducking_.attackSec = (std::max)(0.0f, ducking_.attackSec);
  ducking_.releaseSec = (std::max)(0.0f, ducking_.releaseSec);
  ducking_.targetMask &= ~AudioBusBit(AudioBus::Voice); // 自分自身は下げない
  if (!ducking_.enabled) {
    duckGain_ = 1.0f;
  }
  ApplyBusVolumes_();
}

void AudioEngine::SetLimiter(const AudioLimiterSettings &settings) {
  limiter_ = settings;
  limiter_.release = std::clamp<uint32_t>(limiter_.release, 1u, 20u);
  limiter_.loudness = std::clamp<uint32_t>(limiter_.loudness, 1u, 1800u);
  if (!limiterReady_ || !master_) {
    return;
  }
  LimiterParams_ p{limiter_.release, limiter_.loudness};
  master_->SetEffectParameters(0, &p, sizeof(p));
  if (limiter_.enabled) {
    master_->EnableEffect(0);
  } else {
    master_->DisableEffect(0);
  }
}

void AudioEngine::SetBusLowPass(AudioBus bus, float cutoffHz) {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= static_cast<int>(AudioBus::Count)) {
    return;
  }
  busLowPassTarget_[i] = NormalizeLowPass_(cutoffHz);
}

float AudioEngine::GetBusLowPass(AudioBus bus) const {
  const int i = static_cast<int>(bus);
  return (i >= 0 && i < static_cast<int>(AudioBus::Count)) ? busLowPassTarget_[i] : kLowPassOpenHz;
}

void AudioEngine::UpdateBusFilters_(float dt) {
  for (int i = 0; i < static_cast<int>(AudioBus::Count); ++i) {
    const bool changed = busLowPassCurrent_[i] != busLowPassTarget_[i];
    if (changed) {
      busLowPassCurrent_[i] = SmoothLowPass_(busLowPassCurrent_[i], busLowPassTarget_[i], dt);
    }
    if ((changed || !(dt > 0.0f)) && submix_[i]) {
      const XAUDIO2_FILTER_PARAMETERS p = LowPassParams_(busLowPassCurrent_[i], outputSampleRate_);
      submix_[i]->SetFilterParameters(&p);
    }
  }
}

void AudioEngine::InitReverb_() {
  reverb_.reset();
  reverbChannels_ = 0;
  if (!xaudio_ || dstChannels_ == 0) {
    return;
  }
  // XAudio2 のリバーブは 20〜48kHz で、入出力が モノ→モノ / ステレオ→ステレオ などの組み合わせのみ
  if (outputSampleRate_ < 20000 || outputSampleRate_ > 48000) {
    Log::Print(std::format("[Audio] Reverb unavailable at {}Hz output (20k〜48kHz のみ)", outputSampleRate_));
    return;
  }
  IUnknown *fx = nullptr;
  HRESULT hr = XAudio2CreateReverb(&fx);
  if (FAILED(hr) || !fx) {
    Log::Print(std::format("[Audio] XAudio2CreateReverb failed (hr=0x{:08X})。部屋の響きは無効",
                           static_cast<unsigned>(hr)));
    return;
  }
  const uint32_t ch = (dstChannels_ == 1) ? 1u : 2u;
  XAUDIO2_EFFECT_DESCRIPTOR desc{};
  desc.pEffect = fx;
  desc.InitialState = TRUE;
  desc.OutputChannels = ch;
  XAUDIO2_EFFECT_CHAIN chain{};
  chain.EffectCount = 1;
  chain.pEffectDescriptors = &desc;
  IXAudio2SubmixVoice *raw = nullptr;
  hr = xaudio_->CreateSubmixVoice(&raw, ch, outputSampleRate_, 0, 0, nullptr, &chain);
  fx->Release(); // チェーンが参照を持つ
  if (FAILED(hr) || !raw) {
    Log::Print(std::format("[Audio] CreateSubmixVoice(reverb) failed (hr=0x{:08X})", static_cast<unsigned>(hr)));
    return;
  }
  reverb_.reset(raw);
  reverbChannels_ = ch;
  ApplyRoom_();
}

void AudioEngine::ApplyRoom_() {
  if (!reverb_) {
    return;
  }
  if (room_ == AudioRoom::None) {
    return; // 送り量が 0 になるだけ。リバーブは止めないので、切り替え前の響きは自然に消える
  }
  XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3dl2 = XAUDIO2FX_I3DL2_PRESET_ROOM;
  switch (room_) {
  case AudioRoom::SmallRoom: i3dl2 = XAUDIO2FX_I3DL2_PRESET_SMALLROOM; break;
  case AudioRoom::MediumRoom: i3dl2 = XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM; break;
  case AudioRoom::LargeRoom: i3dl2 = XAUDIO2FX_I3DL2_PRESET_LARGEROOM; break;
  case AudioRoom::Bathroom: i3dl2 = XAUDIO2FX_I3DL2_PRESET_BATHROOM; break;
  case AudioRoom::LivingRoom: i3dl2 = XAUDIO2FX_I3DL2_PRESET_LIVINGROOM; break;
  case AudioRoom::StoneRoom: i3dl2 = XAUDIO2FX_I3DL2_PRESET_STONEROOM; break;
  case AudioRoom::Hallway: i3dl2 = XAUDIO2FX_I3DL2_PRESET_HALLWAY; break;
  case AudioRoom::Cave: i3dl2 = XAUDIO2FX_I3DL2_PRESET_CAVE; break;
  case AudioRoom::Forest: i3dl2 = XAUDIO2FX_I3DL2_PRESET_FOREST; break;
  case AudioRoom::Underwater: i3dl2 = XAUDIO2FX_I3DL2_PRESET_UNDERWATER; break;
  case AudioRoom::PaddedCell: i3dl2 = XAUDIO2FX_I3DL2_PRESET_PADDEDCELL; break;
  default: break;
  }
  XAUDIO2FX_REVERB_PARAMETERS native{};
  ReverbConvertI3DL2ToNative(&i3dl2, &native);
  native.WetDryMix = 100.0f; // 送り（センド）専用なので原音は混ぜない
  reverb_->SetEffectParameters(0, &native, sizeof(native));
  reverb_->EnableEffect(0);
}

void AudioEngine::SetRoom(AudioRoom room) {
  if (static_cast<int>(room) < 0 || room >= AudioRoom::Count) {
    return;
  }
  room_ = room;
  ApplyRoom_();
  for (auto &v : voices_) {
    ApplyReverbSend_(v);
  }
}

void AudioEngine::SetBusVolume(AudioBus bus, float volume) {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= static_cast<int>(AudioBus::Count)) {
    return;
  }
  busVolume_[i] = std::clamp(volume, 0.0f, 1.0f);
  ApplyBusVolumes_();
}

float AudioEngine::GetBusVolume(AudioBus bus) const {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= static_cast<int>(AudioBus::Count)) {
    return 0.0f;
  }
  return busVolume_[i];
}

void AudioEngine::SetMasterVolume(float volume) {
  masterVolume_ = std::clamp(volume, 0.0f, 1.0f);
  if (master_) {
    master_->SetVolume(masterVolume_);
  }
}

void AudioEngine::StopAll() {
  for (auto &v : voices_) {
    DestroyVoice_(v);
  }
  voices_.clear();
  bgmVoiceId_ = -1;
  bgmKeepAlive_ = 0;
}
