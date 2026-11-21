#include "AudioEngine.h"
#include "Common/Log/Log.h"
#include "Common/Math/Math.h" // Inverse（ビュー行列 → カメラのワールド行列）
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <format>
#include <x3daudio.h> // 3D 音響（定位・ドップラーの計算）。xaudio2.lib に含まれる

static_assert(X3DAUDIO_HANDLE_BYTESIZE == 20, "AudioEngine::x3dInstance_ のサイズを X3DAUDIO_HANDLE に合わせてください");

// ================================================================
// ローカルヘルパ
// ================================================================
namespace {

// ベクトルの小物。Math.h（グローバル）と MathUtils.h（RC::）の両方に同名関数があり、
// 両方を include する翻訳単位では ADL で曖昧になるため、ここで閉じた実装を使う。
RC::Vector3 Sub_(const RC::Vector3 &a, const RC::Vector3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
RC::Vector3 Scale_(const RC::Vector3 &v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float Dot_(const RC::Vector3 &a, const RC::Vector3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
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
// シングルトン
// ================================================================
AudioEngine &AudioEngine::Get() {
  // 関数ローカル static だと、エンティティ（コンポーネント）の破棄より先に
  // エンジンが破棄される順序になり得る。コンポーネントのデストラクタから
  // UnloadClip() を呼ぶため、意図的に解放しないインスタンスにしておく。
  static AudioEngine *instance = new AudioEngine();
  return *instance;
}

// ================================================================
// ライフサイクル
// ================================================================
bool AudioEngine::Init() {
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

  IXAudio2MasteringVoice *rawMaster = nullptr;
  hr = xaudio_->CreateMasteringVoice(&rawMaster);
  if (FAILED(hr) || !rawMaster) {
    Log::Print(std::format("[Audio] CreateMasteringVoice failed (hr=0x{:08X})。"
                           "オーディオデバイスが無いため音声なしで続行します",
                           static_cast<unsigned>(hr)));
    xaudio_.Reset();
    return false;
  }
  master_.reset(rawMaster);
  master_->SetVolume(masterVolume_);

  // バスごとのサブミックス。入力形式はマスターに合わせる
  XAUDIO2_VOICE_DETAILS md{};
  master_->GetVoiceDetails(&md);
  for (int i = 0; i < static_cast<int>(AudioBus::Count); ++i) {
    IXAudio2SubmixVoice *raw = nullptr;
    hr = xaudio_->CreateSubmixVoice(&raw, md.InputChannels, md.InputSampleRate, 0, 0, nullptr, nullptr);
    if (FAILED(hr) || !raw) {
      Log::Print(std::format("[Audio] CreateSubmixVoice({}) failed (hr=0x{:08X})", i,
                             static_cast<unsigned>(hr)));
      // サブミックスが無くてもマスターへ直接送れば鳴らせるので続行する
      continue;
    }
    submix_[i].reset(raw);
    submix_[i]->SetVolume(busVolume_[i]);
  }

  // 3D 音響。出力のスピーカー構成（チャンネルマスク）を X3DAudio に教える
  dstChannels_ = md.InputChannels;
  DWORD mask = 0;
  if (FAILED(master_->GetChannelMask(&mask)) || mask == 0) {
    mask = DefaultChannelMask_(dstChannels_);
  }
  channelMask_ = static_cast<uint32_t>(mask);
  InitX3D_();

  Log::Print(std::format("[Audio] Initialized ({}ch / {}Hz)", md.InputChannels, md.InputSampleRate));
  return true;
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

void AudioEngine::Term() {
  // 破棄順: ソースボイス → サブミックス → マスター → エンジン
  StopAll();
  for (auto &c : clips_) {
    if (c.used) {
      FreeClip_(c);
    }
  }
  clips_.clear();
  clipByPath_.clear();
  for (auto &s : submix_) {
    s.reset();
  }
  master_.reset();
  xaudio_.Reset();
  bgmKeepAlive_ = 0;
  x3dReady_ = false;
  dstChannels_ = 0;
  listenerHasPrev_ = false;
}

void AudioEngine::Update() {
  if (!xaudio_) {
    return;
  }

  // 鳴り終わった SE を回収（後ろから消す）
  for (size_t i = seVoices_.size(); i-- > 0;) {
    if (IsVoiceFinished_(seVoices_[i])) {
      DestroyVoice_(seVoices_[i]);
      seVoices_.erase(seVoices_.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }

  // 3D SE: リスナーとエミッターの最新位置で定位・音量・ピッチを更新
  for (auto &v : seVoices_) {
    if (v.spatial) {
      ApplySpatial_(v);
    }
  }

  // BGM: 鳴り終わり（非ループ）の回収と、持ち主不在による停止
  if (bgm_.voice) {
    if (IsVoiceFinished_(bgm_)) {
      StopBgm();
    } else if (bgmKeepAlive_ > 0) {
      --bgmKeepAlive_;
    } else {
      Log::Print("[Audio] BGM stopped (no owner): " + ClipPath(bgm_.clip));
      StopBgm();
    }
  }
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

int AudioEngine::LoadClip(const std::string &rawPath) {
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

  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    Log::Print("[Audio] File not found: " + path);
    return -1;
  }

  SoundData data = SoundLoadAudio(path.c_str());
  if (!data.pBuffer || data.bufferSize == 0 || data.wfex.nChannels == 0 ||
      data.wfex.nSamplesPerSec == 0 || data.wfex.nBlockAlign == 0) {
    Log::Print("[Audio] Load failed (unsupported or broken file): " + path);
    return -1;
  }
  // XAudio2 はブロック単位で読むため、端数バイトは切り捨てておく
  data.bufferSize -= data.bufferSize % data.wfex.nBlockAlign;
  if (data.bufferSize == 0) {
    Log::Print("[Audio] Load failed (no audio frames): " + path);
    return -1;
  }

  const int handle = AllocClipSlot_();
  Clip &c = clips_[handle];
  c.path = path;
  c.data = std::move(data);
  c.refCount = 1;
  c.used = true;
  clipByPath_[key] = handle;

  Log::Print(std::format("[Audio] Loaded clip #{}: {} ({:.1f}s, {}ch {}Hz)", handle, path,
                         ClipDuration(handle), c.data.wfex.nChannels, c.data.wfex.nSamplesPerSec));
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
  const auto &d = clips_[clip].data;
  if (d.wfex.nAvgBytesPerSec == 0) {
    return 0.0f;
  }
  return static_cast<float>(d.bufferSize) / static_cast<float>(d.wfex.nAvgBytesPerSec);
}

int AudioEngine::ClipChannels(int clip) const {
  return IsValidClip(clip) ? static_cast<int>(clips_[clip].data.wfex.nChannels) : 0;
}

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
AudioEngine::SourceVoicePtr AudioEngine::CreateVoice_(int clip, AudioBus bus, float volume, bool loop) {
  if (!xaudio_ || !IsValidClip(clip)) {
    return nullptr;
  }
  const Clip &c = clips_[clip];

  // 送り先: バスのサブミックス（無ければマスターへ直接）
  IXAudio2SubmixVoice *target = submix_[static_cast<int>(bus)].get();
  XAUDIO2_SEND_DESCRIPTOR send{};
  send.Flags = 0;
  send.pOutputVoice = target;
  XAUDIO2_VOICE_SENDS sends{};
  sends.SendCount = 1;
  sends.pSends = &send;

  IXAudio2SourceVoice *raw = nullptr;
  HRESULT hr = xaudio_->CreateSourceVoice(&raw, &c.data.wfex, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr,
                                          target ? &sends : nullptr, nullptr);
  if (FAILED(hr) || !raw) {
    Log::Print(std::format("[Audio] CreateSourceVoice failed (hr=0x{:08X}): {}", static_cast<unsigned>(hr),
                           c.path));
    return nullptr;
  }
  SourceVoicePtr v(raw);
  v->SetVolume((std::max)(0.0f, volume));

  XAUDIO2_BUFFER buf{};
  buf.pAudioData = c.data.pBuffer.get();
  buf.AudioBytes = c.data.bufferSize;
  buf.Flags = XAUDIO2_END_OF_STREAM;
  buf.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
  hr = v->SubmitSourceBuffer(&buf);
  if (FAILED(hr)) {
    Log::Print(std::format("[Audio] SubmitSourceBuffer failed (hr=0x{:08X}): {}", static_cast<unsigned>(hr),
                           c.path));
    return nullptr;
  }
  // Start() は呼び出し側で行う（3D の初期定位を先に反映してから鳴らし始めるため）
  return v;
}

bool AudioEngine::StartVoice_(Voice &v) {
  if (!v.voice) {
    return false;
  }
  const HRESULT hr = v.voice->Start();
  if (FAILED(hr)) {
    Log::Print(std::format("[Audio] Start failed (hr=0x{:08X}): {}", static_cast<unsigned>(hr), ClipPath(v.clip)));
    return false;
  }
  return true;
}

void AudioEngine::DestroyVoice_(Voice &v) {
  // ボイスを先に破棄（音声スレッドが PCM を読み終わるのを待つ）→ その後クリップ参照を返す
  v.voice.reset();
  if (v.clip >= 0) {
    ReleaseClipRef_(v.clip);
  }
  v.clip = -1;
  v.id = -1;
  v.spatial = false;
  v.dest = nullptr;
}

bool AudioEngine::IsVoiceFinished_(const Voice &v) {
  if (!v.voice) {
    return true;
  }
  XAUDIO2_VOICE_STATE st{};
  v.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
  return st.BuffersQueued == 0;
}

AudioEngine::Voice *AudioEngine::FindSeVoice_(int voiceId) {
  if (voiceId < 0) {
    return nullptr;
  }
  for (auto &v : seVoices_) {
    if (v.id == voiceId) {
      return &v;
    }
  }
  return nullptr;
}

const AudioEngine::Voice *AudioEngine::FindSeVoice_(int voiceId) const {
  if (voiceId < 0) {
    return nullptr;
  }
  for (const auto &v : seVoices_) {
    if (v.id == voiceId) {
      return &v;
    }
  }
  return nullptr;
}

// ================================================================
// SE
// ================================================================
int AudioEngine::PlaySe(int clip, float volume, bool loop, const AudioEmitter *emitter) {
  SourceVoicePtr v = CreateVoice_(clip, AudioBus::SE, volume, loop);
  if (!v) {
    return -1;
  }
  Voice av;
  av.id = nextVoiceId_++;
  av.clip = clip;
  av.volume = volume;
  av.loop = loop;
  av.voice = std::move(v);
  av.dest = submix_[static_cast<int>(AudioBus::SE)].get();
  av.srcChannels = clips_[clip].data.wfex.nChannels;

  // 3D: 鳴らし始める前に定位・距離減衰を反映しておく（最初の 1 フレームが 2D の大音量にならない）
  if (emitter && emitter->spatialBlend > 0.0f && x3dReady_) {
    av.emitter = *emitter;
    if (CaptureFlatMatrix_(av)) {
      av.spatial = true;
      ApplySpatial_(av);
    }
  }

  if (!StartVoice_(av)) {
    return -1; // av.voice はデストラクタで破棄される。クリップ参照はまだ増やしていない
  }
  clips_[clip].refCount++; // ボイスが生きている間はクリップを解放させない
  seVoices_.push_back(std::move(av));
  return seVoices_.back().id;
}

void AudioEngine::StopVoice(int voiceId) {
  for (size_t i = 0; i < seVoices_.size(); ++i) {
    if (seVoices_[i].id == voiceId) {
      DestroyVoice_(seVoices_[i]);
      seVoices_.erase(seVoices_.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}

bool AudioEngine::IsVoicePlaying(int voiceId) const {
  const Voice *v = FindSeVoice_(voiceId);
  return v && !IsVoiceFinished_(*v);
}

bool AudioEngine::IsVoiceLooping(int voiceId) const {
  const Voice *v = FindSeVoice_(voiceId);
  return v && v->loop && !IsVoiceFinished_(*v);
}

void AudioEngine::SetVoiceVolume(int voiceId, float volume) {
  if (Voice *v = FindSeVoice_(voiceId)) {
    v->volume = (std::max)(0.0f, volume);
    if (v->voice) {
      v->voice->SetVolume(v->volume);
    }
  }
}

void AudioEngine::SetVoiceEmitter(int voiceId, const AudioEmitter &emitter) {
  Voice *v = FindSeVoice_(voiceId);
  if (!v || !v->voice) {
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
  const Voice *v = FindSeVoice_(voiceId);
  return v && v->spatial;
}

void AudioEngine::StopAllSe() {
  for (auto &v : seVoices_) {
    DestroyVoice_(v);
  }
  seVoices_.clear();
}

size_t AudioEngine::ActiveSpatialCount() const {
  size_t n = 0;
  for (const auto &v : seVoices_) {
    if (v.spatial) {
      ++n;
    }
  }
  return n;
}

// ================================================================
// BGM
// ================================================================
void AudioEngine::PlayBgm(int clip, float volume, bool loop) {
  if (!xaudio_ || !IsValidClip(clip)) {
    return;
  }
  // 同じ曲なら継続。音量だけ反映する
  if (bgm_.voice && bgm_.clip == clip) {
    KeepBgmAlive(clip, volume);
    return;
  }
  StopBgm();
  SourceVoicePtr v = CreateVoice_(clip, AudioBus::BGM, volume, loop);
  if (!v) {
    return;
  }
  bgm_.id = nextVoiceId_++;
  bgm_.clip = clip;
  bgm_.volume = volume;
  bgm_.loop = loop;
  bgm_.voice = std::move(v);
  bgm_.dest = submix_[static_cast<int>(AudioBus::BGM)].get();
  bgm_.srcChannels = clips_[clip].data.wfex.nChannels;
  bgm_.spatial = false; // BGM は常に 2D
  if (!StartVoice_(bgm_)) {
    bgm_.voice.reset();
    bgm_.clip = -1;
    bgm_.id = -1;
    return;
  }
  clips_[clip].refCount++;
  bgmKeepAlive_ = kBgmGraceFrames;
  Log::Print("[Audio] BGM start: " + clips_[clip].path);
}

void AudioEngine::KeepBgmAlive(int clip, float volume) {
  if (!bgm_.voice || bgm_.clip != clip) {
    return;
  }
  bgmKeepAlive_ = kBgmGraceFrames;
  const float v = (std::max)(0.0f, volume);
  if (v != bgm_.volume) {
    bgm_.volume = v;
    bgm_.voice->SetVolume(v);
  }
}

void AudioEngine::StopBgm() {
  const int clip = bgm_.clip;
  if (bgm_.voice) {
    DestroyVoice_(bgm_);
  }
  bgm_.voice.reset();
  bgm_.clip = -1;
  bgm_.id = -1;
  bgmKeepAlive_ = 0;
  // BGM はデコード済み PCM が大きい（数十 MB）ので、どのコンポーネントも参照していなければ
  // PurgeUnusedClips() を待たずにここで解放する（前のシーンの BGM が差し替わったときなど）
  if (IsValidClip(clip) && clips_[clip].refCount <= 0) {
    FreeClip_(clips_[clip]);
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
  v.spatial = false;
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
  if (blend <= 0.0f || !x3dReady_) {
    ResetSpatial_(v); // 2D に戻す（Inspector で 3D を切った / X3DAudio が無い）
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
  X3DAUDIO_EMITTER em{};
  em.pCone = nullptr;
  // 音源の向きはリスナーと揃える: ステレオ音源の L/R がそのままリスナーの左右になる
  // （モノラル音源では向きは定位に影響しない）
  em.OrientFront = listener.OrientFront;
  em.OrientTop = listener.OrientTop;
  em.Position = ToX3D_(v.emitter.position);
  em.Velocity = ToX3D_(v.emitter.velocity);
  // minDistance の内側では全スピーカーに滲ませる（リスナーの真横・真上を通過してもパンが暴れない）
  em.InnerRadius = minD;
  em.InnerRadiusAngle = X3DAUDIO_PI / 4.0f;
  em.ChannelCount = src;
  em.ChannelRadius = (src > 1) ? (std::max)(minD * 0.5f, 0.1f) : 0.0f;
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

  const float distance = Length_(Sub_(v.emitter.position, listener_.position));
  const float atten = ComputeRolloff(distance, v.emitter.minDistance, v.emitter.maxDistance, v.emitter.rolloff);
  const float gain3d = norm * atten * blend;
  const float gain2d = 1.0f - blend;
  for (size_t i = 0; i < n; ++i) {
    float m = v.flatMatrix[i] * gain2d + v.workMatrix[i] * gain3d;
    if (!std::isfinite(m)) {
      m = 0.0f;
    }
    v.workMatrix[i] = std::clamp(m, 0.0f, XAUDIO2_MAX_VOLUME_LEVEL);
  }
  v.voice->SetOutputMatrix(v.dest, src, dst, v.workMatrix.data());

  // --- ドップラー（ピッチ）---
  float ratio = 1.0f;
  if (doppler) {
    float f = dsp.DopplerFactor;
    if (!std::isfinite(f) || !(f > 0.0f)) {
      f = 1.0f;
    }
    // ボイスは XAUDIO2_DEFAULT_FREQ_RATIO(2.0) で作っているので、その範囲に収める
    f = std::clamp(f, 0.5f, XAUDIO2_DEFAULT_FREQ_RATIO);
    ratio = 1.0f + (f - 1.0f) * blend;
  }
  v.voice->SetFrequencyRatio(ratio);
}

// ================================================================
// バス / マスター
// ================================================================
void AudioEngine::SetBusVolume(AudioBus bus, float volume) {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= static_cast<int>(AudioBus::Count)) {
    return;
  }
  busVolume_[i] = std::clamp(volume, 0.0f, 1.0f);
  if (submix_[i]) {
    submix_[i]->SetVolume(busVolume_[i]);
  }
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
  StopAllSe();
  StopBgm();
}
