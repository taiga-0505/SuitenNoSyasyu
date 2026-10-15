#pragma once
#include "IComponent.h"
#include "Entity.h"
#include "TransformComponent.h"
#include "Audio/AudioEngine.h"
#include "Common/Log/Log.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

/// @enum AudioSpatialMode
/// @brief スロットごとの 2D / 3D の指定
enum class AudioSpatialMode : int {
  Inherit = 0, ///< コンポーネントの spatialBlend に従う（既定）
  Force2D = 1, ///< 常に 2D（UI 音・自分の足音など。コンポーネントが 3D でも位置に関係なく鳴る）
  Force3D = 2, ///< 常に 3D（コンポーネントが 2D でも位置から聞こえる）
  Count        ///< 要素数
};

/// @struct AudioClipSlot
/// @brief AudioSourceComponent が持つ 1 つの音（名前付きスロット）
/// @details スクリプトからは name で指定して鳴らす。path は Inspector / JSON で差し替えられる。
struct AudioClipSlot {
  std::string name;    ///< スクリプトから指定する名前（"main", "shoot", "damage" など）
  std::string path;    ///< 音声ファイル（.wav / .mp3）
  float volume = 1.0f; ///< このスロットの音量（コンポーネントの volume に乗算される）
  bool loop = false;   ///< ループ再生するか
  AudioSpatialMode spatial = AudioSpatialMode::Inherit; ///< 2D / 3D の上書き（SE バスのみ意味を持つ）

  // --- ランタイム（シリアライズしない） ---
  int clipHandle = -1;    ///< AudioEngine のクリップハンドル
  std::string loadedPath; ///< clipHandle にロード済みのパス（差し替え検出用）

  /// @brief クリップがロード済みか
  /// @return clipHandle が 0 以上なら true
  bool IsLoaded() const { return clipHandle >= 0; }
};

/// @class AudioSourceComponent
/// @brief エンティティに音を持たせるコンポーネント（Unity の AudioSource 相当）
/// @details 使い方は 2 通り。
///          - BGM: 空のエンティティに付け、bus = BGM、clips に 1 曲、playOnAwake にその名前を設定。
///                 コードを書かずに、再生開始（Playing）と同時に鳴り始める。
///          - SE : プレイヤー等に付け、bus = SE、clips に "shoot" "damage" などを登録。
///                 スクリプトから GetComponent<AudioSourceComponent>()->Play("shoot") で鳴らす。
///
///          1 エンティティには同じ型のコンポーネントを 1 つしか付けられないため、
///          複数の音は「複数のコンポーネント」ではなく「複数のスロット」で持つ。
///
///          再生の性質は bus で決まる（AudioBus 参照）。BGM バスは 1 本だけ鳴り、
///          同じ曲を次のシーンにも置いておけば途切れずに続く。誰も鳴らさなくなれば数フレームで止まる。
///
///          3D 音響（SE バスのみ）: spatialBlend を 0 より大きくすると、エンティティの Transform の
///          位置から聞こえる（左右の定位・距離減衰・ドップラー）。聞き手はシーンの
///          AudioListenerComponent、無ければ描画中のカメラ。BGM バスは常に 2D。
///          スロットごとに AudioClipSlot::spatial で「常に 2D」「常に 3D」に上書きできるので、
///          同じエンティティで足音は 3D、UI 音は 2D といった混在ができる。
///          モノラルの音源が最も自然に定位する（ステレオ音源は L/R を音源の左右に配置する）。
///
///          ランタイムの流れ（DataDrivenScene が呼ぶ）:
///            LoadClips()      … OnEnter / 毎フレーム（パス差し替えの遅延ロード）
///            Tick()           … 毎フレーム。playOnAwake の発火、BGM の keep-alive、3D の位置更新
///            Silence()        … エンティティ非アクティブ / コンポーネント無効時
///            ReleaseRuntime() … OnExit / 破棄時
class AudioSourceComponent : public IComponent {
public:
  AudioBus bus = AudioBus::SE;      ///< 音を流すバス
  float volume = 1.0f;              ///< コンポーネント全体の音量（0.0〜1.0）
  std::vector<AudioClipSlot> clips; ///< 名前付きの音一覧
  std::string playOnAwake;          ///< 再生開始時に自動で鳴らすスロット名（空なら鳴らさない）

  // --- 3D 音響（SE バスのみ。BGM は常に 2D）---
  float spatialBlend = 0.0f;  ///< 0 = 2D（位置に関係なく鳴る。従来通り）、1 = 完全に 3D。中間はブレンド
  float minDistance = 1.0f;   ///< この距離（ワールド単位）までは最大音量で鳴る
  float maxDistance = 50.0f;  ///< この距離で無音になる
  AudioRolloff rolloff = AudioRolloff::Logarithmic; ///< 減衰カーブ（Logarithmic = 距離に反比例、Linear = 直線）
  float dopplerLevel = 1.0f;  ///< ドップラー効果の強さ。0 で無効、1 で物理的に正しい値

  AudioSourceComponent() = default;
  // クリップ参照を持つため、コピーすると UnloadClip が二重に走る。コピー禁止
  AudioSourceComponent(const AudioSourceComponent &) = delete;
  AudioSourceComponent &operator=(const AudioSourceComponent &) = delete;

  /// @brief 鳴らしている音とクリップ参照を解放する
  ~AudioSourceComponent() override {
    // Inspector の Remove Component など、ReleaseRuntimeResources を経由しない破棄でも
    // 音が鳴り続けたりクリップ参照が漏れたりしないようにする。
    // AudioEngine のみを触り、Entity には触らないのでデストラクタから呼んで安全。
    ReleaseRuntime();
  }

  // ================================================================
  // 操作（スクリプトから使う）
  // ================================================================

  /// @brief スロット名を指定して鳴らす
  /// @param slotName clips に登録した名前
  /// @param volScale 個別の音量倍率（連射の音を少し小さく、など）
  /// @return 鳴らせたら true
  /// @details SE バス: 呼ぶたびに重ねて再生される。3D 設定（spatialBlend > 0、またはスロットが
  ///                   Force3D）なら、エンティティの現在位置から聞こえる音として鳴り始める。
  ///          BGM バス: 同じ曲が既に鳴っていれば継続、違う曲なら差し替える。
  bool Play(const std::string &slotName, float volScale = 1.0f) {
    AudioClipSlot *s = FindSlot(slotName);
    if (!s) {
      if (lastMissingSlot_ != slotName) {
        lastMissingSlot_ = slotName;
        Log::Print("[AudioSource] Slot not found: \"" + slotName + "\"");
      }
      return false;
    }
    LoadClips(); // 未ロード / パス差し替え直後でも鳴らせるように
    if (!s->IsLoaded()) {
      return false;
    }

    auto &engine = AudioEngine::Get();
    const float vol = volume * s->volume * volScale;
    if (bus == AudioBus::BGM) {
      engine.PlayBgm(s->clipHandle, vol, s->loop);
      bgmSlotName_ = s->name;
      bgmVolScale_ = volScale;
      return engine.CurrentBgmClip() == s->clipHandle;
    }
    // 3D なら最初のサンプルから正しい定位で鳴らす（Transform が無ければ 2D）
    AudioEmitter emitter{};
    const float blend = EffectiveBlend(*s);
    const bool spatial = blend > 0.0f && BuildEmitter_(emitter, blend);
    const int id = engine.PlaySe(s->clipHandle, vol, s->loop, spatial ? &emitter : nullptr);
    if (id < 0) {
      return false;
    }
    PruneVoices_();
    seVoices_.push_back(SeVoice{id, s->name, spatial});
    return true;
  }

  /// @brief スロット名を省略して鳴らす（playOnAwake、未設定なら先頭のスロット）
  /// @param volScale 個別の音量倍率
  /// @return 鳴らせたら true（スロットが 1 つも無ければ false）
  bool Play(float volScale = 1.0f) {
    if (!playOnAwake.empty()) {
      return Play(playOnAwake, volScale);
    }
    if (!clips.empty()) {
      return Play(clips.front().name, volScale);
    }
    return false;
  }

  /// @brief このコンポーネントが鳴らしている音をすべて止める
  /// @details SE は即時停止。BGM バスの場合、自分が鳴らした曲が流れていれば即時停止する。
  void Stop() {
    StopSe_();
    if (bus == AudioBus::BGM && !bgmSlotName_.empty()) {
      auto &engine = AudioEngine::Get();
      if (const AudioClipSlot *s = FindSlot(bgmSlotName_);
          s && s->IsLoaded() && engine.CurrentBgmClip() == s->clipHandle) {
        engine.StopBgm();
      }
      bgmSlotName_.clear();
    }
  }

  /// @brief 何か鳴っているか
  /// @return BGM バスなら自分の曲が流れていれば true、SE バスなら鳴っているボイスがあれば true
  bool IsPlaying() const {
    const auto &engine = AudioEngine::Get();
    if (bus == AudioBus::BGM) {
      const AudioClipSlot *s = bgmSlotName_.empty() ? nullptr : FindSlot(bgmSlotName_);
      return s && s->IsLoaded() && engine.CurrentBgmClip() == s->clipHandle;
    }
    for (const auto &v : seVoices_) {
      if (engine.IsVoicePlaying(v.id)) {
        return true;
      }
    }
    return false;
  }

  /// @brief BGM バスか
  /// @return bus が BGM なら true
  bool IsBgm() const { return bus == AudioBus::BGM; }

  /// @brief コンポーネントの既定が 3D か（SE バスで spatialBlend > 0）
  /// @return SE バスで spatialBlend が 0 より大きければ true
  /// @note スロットの spatial（Force2D / Force3D）で個別に上書きされる。Any3D() / EffectiveBlend() 参照。
  ///       Transform が無いエンティティでは実際には 2D で鳴る（HasTransform() 参照）。
  bool Is3D() const { return bus == AudioBus::SE && spatialBlend > 0.0f; }

  /// @brief 3D で鳴るスロットが 1 つでもあるか（距離設定が意味を持つか）
  /// @return 3D で鳴るスロットがある、またはコンポーネントの既定が 3D なら true（BGM バスは常に false）
  bool Any3D() const {
    if (bus != AudioBus::SE) {
      return false;
    }
    for (const auto &s : clips) {
      if (EffectiveBlend(s) > 0.0f) {
        return true;
      }
    }
    return spatialBlend > 0.0f; // スロットが無くても既定が 3D なら true
  }

  /// @brief スロットが実際に使う 2D/3D ブレンド（0 = 2D、1 = 3D）
  /// @details Inherit ならコンポーネントの spatialBlend、Force2D なら 0、Force3D なら 1。BGM バスは常に 0。
  /// @param slot 対象のスロット
  /// @return ブレンド値（0〜1）
  float EffectiveBlend(const AudioClipSlot &slot) const { return EffectiveBlend_(slot.spatial); }

  /// @brief 位置の取得に使う Transform を持っているか
  /// @return エンティティに TransformComponent があれば true
  bool HasTransform() const {
    const Entity *e = GetEntity();
    return e && e->GetComponent<TransformComponent>() != nullptr;
  }

  /// @brief 音源のワールド座標を取得する
  /// @param out ワールド座標の出力先
  /// @return Transform が無ければ false
  bool GetWorldPosition(RC::Vector3 &out) const {
    const Entity *e = GetEntity();
    const TransformComponent *tr = e ? e->GetComponent<TransformComponent>() : nullptr;
    if (!tr) {
      return false;
    }
    const RC::Matrix4x4 w = tr->GetWorldMatrix();
    out = {w.m[3][0], w.m[3][1], w.m[3][2]};
    return true;
  }

  /// @brief リスナー（聞き手）までの距離。Transform が無ければ 0
  /// @return リスナーまでの距離（ワールド単位）
  float DistanceToListener() const {
    RC::Vector3 p{};
    if (!GetWorldPosition(p)) {
      return 0.0f;
    }
    const RC::Vector3 &l = AudioEngine::Get().GetListener().position;
    const float dx = p.x - l.x, dy = p.y - l.y, dz = p.z - l.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  /// @brief 現在のリスナー位置で聞こえる距離減衰（0.0〜1.0）。Inspector の表示用
  /// @return 減衰率（3D で鳴らない、または Transform が無ければ 1.0）
  float CurrentRolloffGain() const {
    if (!Any3D() || !HasTransform()) {
      return 1.0f;
    }
    return AudioEngine::ComputeRolloff(DistanceToListener(), minDistance, maxDistance, rolloff);
  }

  /// @brief 推定中の音源速度（ワールド単位/秒）。ドップラー効果に使っている値
  /// @return 推定した速度
  const RC::Vector3 &CurrentVelocity() const { return velocity_; }

  // ================================================================
  // スロット編集（Inspector / セットアップ用）
  // ================================================================

  /// @brief 名前でスロットを探す
  /// @param name スロット名
  /// @return 見つかったスロット（無ければ nullptr）
  AudioClipSlot *FindSlot(const std::string &name) {
    for (auto &s : clips) {
      if (s.name == name) {
        return &s;
      }
    }
    return nullptr;
  }

  /// @brief 名前でスロットを探す（const 版）
  /// @param name スロット名
  /// @return 見つかったスロット（無ければ nullptr）
  const AudioClipSlot *FindSlot(const std::string &name) const {
    for (const auto &s : clips) {
      if (s.name == name) {
        return &s;
      }
    }
    return nullptr;
  }

  /// @brief スロットを追加する（同名があれば上書き）
  /// @param name    スロット名
  /// @param path    音声ファイルのパス
  /// @param vol     スロットの音量
  /// @param loop    ループ再生するか
  /// @param spatial 2D / 3D の上書き（既定はコンポーネントの spatialBlend に従う）
  /// @return 追加（または上書き）したスロット
  AudioClipSlot &AddClip(const std::string &name, const std::string &path, float vol = 1.0f, bool loop = false,
                         AudioSpatialMode spatial = AudioSpatialMode::Inherit) {
    if (AudioClipSlot *s = FindSlot(name)) {
      s->path = path;
      s->volume = vol;
      s->loop = loop;
      s->spatial = spatial;
      return *s;
    }
    AudioClipSlot s;
    s.name = name;
    s.path = path;
    s.volume = vol;
    s.loop = loop;
    s.spatial = spatial;
    clips.push_back(std::move(s));
    return clips.back();
  }

  /// @brief スロット名を変更する（playOnAwake や再生中 BGM の参照も追従させる）
  /// @param index   clips の添え字（範囲外なら何もしない）
  /// @param newName 新しいスロット名
  void RenameClip(size_t index, const std::string &newName) {
    if (index >= clips.size()) {
      return;
    }
    const std::string old = clips[index].name;
    if (old == newName) {
      return;
    }
    clips[index].name = newName;
    if (playOnAwake == old) {
      playOnAwake = newName;
    }
    if (bgmSlotName_ == old) {
      bgmSlotName_ = newName;
    }
    for (auto &v : seVoices_) {
      if (v.slot == old) {
        v.slot = newName; // 鳴っている SE の 2D/3D 判定もこのスロットを追い続ける
      }
    }
  }

  /// @brief 重複しないスロット名を作る（"clip1", "clip2", ...）
  /// @param base 名前の接頭辞
  /// @return 既存スロットと重ならない名前
  std::string MakeUniqueClipName(const std::string &base = "clip") const {
    for (int i = 1; i < 10000; ++i) {
      const std::string candidate = base + std::to_string(i);
      if (!FindSlot(candidate)) {
        return candidate;
      }
    }
    return base + std::to_string(clips.size() + 1);
  }

  /// @brief スロットを削除する（ロード済みならクリップ参照も返す）
  /// @param index clips の添え字（範囲外なら何もしない）
  void RemoveClip(size_t index) {
    if (index >= clips.size()) {
      return;
    }
    AudioClipSlot &s = clips[index];
    if (s.name == bgmSlotName_) {
      bgmSlotName_.clear(); // keep-alive をやめる（数フレーム後に BGM が止まる）
    }
    if (s.name == playOnAwake) {
      playOnAwake.clear();
    }
    if (s.IsLoaded()) {
      AudioEngine::Get().UnloadClip(s.clipHandle);
      s.clipHandle = -1;
    }
    clips.erase(clips.begin() + static_cast<std::ptrdiff_t>(index));
  }

  // ================================================================
  // ランタイム管理（シーンから呼ぶ）
  // ================================================================

  /// @brief パスが変わった／未ロードのスロットをロードする（毎フレーム呼んでも軽い）
  void LoadClips() {
    auto &engine = AudioEngine::Get();
    for (auto &s : clips) {
      if (s.path == s.loadedPath) {
        continue;
      }
      if (s.IsLoaded()) {
        if (s.name == bgmSlotName_) {
          bgmSlotName_.clear(); // 鳴っている BGM のパスが差し替えられた → 古い曲の keep-alive をやめる
        }
        engine.UnloadClip(s.clipHandle);
        s.clipHandle = -1;
      }
      // 失敗しても loadedPath を更新して毎フレーム再試行しない（フォントと同じ方針）
      s.loadedPath = s.path;
      if (!s.path.empty()) {
        s.clipHandle = engine.LoadClip(s.path);
      }
    }
  }

  /// @brief 毎フレームの更新
  /// @param playing 再生中（PlayState::Playing）か
  /// @param paused 一時停止中（PlayState::Paused）か
  /// @param dt 前フレームからの経過時間（秒）。3D 音響の速度推定（ドップラー）に使う
  /// @details Playing に入った最初のフレームで playOnAwake を鳴らす。
  ///          Paused 中は BGM を継続（keep-alive のみ）。
  ///          Stopped（編集モード）では awake フラグを戻すだけで、鳴っている音は止めない。
  ///          Inspector の試聴ボタンで鳴らした音を止めないため。停止ボタンでの停止は
  ///          RestoreState → ReleaseRuntimeResources の経路で行われる。
  ///          3D の SE は毎フレーム Transform の位置をエンジンへ送る（編集モードの試聴でも追従する）。
  void Tick(bool playing, bool paused, float dt = 1.0f / 60.0f) {
    PruneVoices_();
    if (playing) {
      if (!awakeFired_) {
        awakeFired_ = true;
        if (!playOnAwake.empty()) {
          Play(playOnAwake);
        }
      }
    } else if (!paused) {
      awakeFired_ = false;
    }
    KeepBgm_();
    UpdateSpatial_(playing, dt);
  }

  /// @brief エンティティが非アクティブ／コンポーネントが無効なときに呼ぶ
  /// @details SE を止め、BGM の keep-alive をやめる（他に同じ曲の持ち主がいなければ数フレームで止まる）。
  ///          再びアクティブになれば playOnAwake がもう一度発火する。
  void Silence() {
    StopSe_();
    bgmSlotName_.clear();
    awakeFired_ = false;
    ResetMotion_();
  }

  /// @brief ランタイムリソースを解放する（OnExit / 破棄時）
  /// @details 単発の SE は鳴り切らせ、ループ SE だけ止める（撃破音を鳴らして即 Destroy しても切れない）。
  ///          BGM は止めず keep-alive だけをやめる。次のシーンの AudioSource が同じ曲を
  ///          要求すれば途切れずに継続し、誰も要求しなければ AudioEngine が数フレーム後に止める。
  ///          エディタの停止ボタンでは App 側が AudioEngine::StopAll() を呼んで即時に無音にする。
  void ReleaseRuntime() {
    DetachSe_();
    bgmSlotName_.clear();
    awakeFired_ = false;
    ResetMotion_();
    auto &engine = AudioEngine::Get();
    for (auto &s : clips) {
      if (s.IsLoaded()) {
        engine.UnloadClip(s.clipHandle);
        s.clipHandle = -1;
      }
      s.loadedPath.clear();
    }
  }

  // ================================================================
  // Inspector 表示用
  // ================================================================

  /// @brief playOnAwake を発火済みか
  /// @return 発火済みなら true
  bool HasFiredAwake() const { return awakeFired_; }

  /// @brief BGM バスで自分が鳴らしているスロット名（無ければ空）
  /// @return スロット名
  const std::string &CurrentBgmSlot() const { return bgmSlotName_; }

  /// @brief 再生中の SE ボイス数
  /// @return 鳴っているボイスの数
  size_t ActiveSeCount() const {
    const auto &engine = AudioEngine::Get();
    size_t n = 0;
    for (const auto &v : seVoices_) {
      if (engine.IsVoicePlaying(v.id)) {
        ++n;
      }
    }
    return n;
  }

  /// @brief 再生中の SE ボイスのうち 3D で鳴っている数
  /// @return 3D で鳴っているボイスの数
  size_t ActiveSpatialCount() const {
    const auto &engine = AudioEngine::Get();
    size_t n = 0;
    for (const auto &v : seVoices_) {
      if (engine.IsVoiceSpatial(v.id) && engine.IsVoicePlaying(v.id)) {
        ++n;
      }
    }
    return n;
  }

  // ================================================================
  // シリアライズ
  // ================================================================

  /// @brief コンポーネントの型名を返す
  /// @return "AudioSourceComponent"
  const char *TypeName() const override { return "AudioSourceComponent"; }

  /// @brief バス・音量・スロット・3D 音響の設定を JSON に書き出す
  /// @return ランタイム状態以外の設定値を格納した JSON
  nlohmann::json Serialize() const override {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &s : clips) {
      arr.push_back({{"name", s.name},
                     {"path", s.path},
                     {"volume", s.volume},
                     {"loop", s.loop},
                     {"spatial", static_cast<int>(s.spatial)}});
    }
    return {
        {"bus", static_cast<int>(bus)},
        {"volume", volume},
        {"playOnAwake", playOnAwake},
        {"clips", arr},
        // 3D 音響
        {"spatialBlend", spatialBlend},
        {"minDistance", minDistance},
        {"maxDistance", maxDistance},
        {"rolloff", static_cast<int>(rolloff)},
        {"dopplerLevel", dopplerLevel},
    };
  }

  /// @brief JSON からバス・音量・スロット・3D 音響の設定を読み込む
  /// @param j 読み込み元の JSON
  /// @note 先に ReleaseRuntime() してスロットを空にしてから読み込む。範囲外の値は既定値または有効範囲へ丸める。
  void Deserialize(const nlohmann::json &j) override {
    // 既存のランタイム状態は捨てる（Undo / ペーストで再構築されるケース）
    ReleaseRuntime();
    clips.clear();

    if (j.contains("bus")) {
      const int b = j["bus"].get<int>();
      bus = (b >= 0 && b < static_cast<int>(AudioBus::Count)) ? static_cast<AudioBus>(b) : AudioBus::SE;
    }
    if (j.contains("volume")) volume = j["volume"].get<float>();
    if (j.contains("playOnAwake")) playOnAwake = j["playOnAwake"].get<std::string>();

    // 3D 音響（古いシーン JSON には無い → 既定値 = 2D のまま）
    spatialBlend = std::clamp(j.value("spatialBlend", 0.0f), 0.0f, 1.0f);
    minDistance = (std::max)(j.value("minDistance", 1.0f), 0.0f);
    maxDistance = (std::max)(j.value("maxDistance", 50.0f), 0.0f);
    dopplerLevel = (std::max)(j.value("dopplerLevel", 1.0f), 0.0f);
    {
      const int r = j.value("rolloff", static_cast<int>(AudioRolloff::Logarithmic));
      rolloff = (r >= 0 && r < static_cast<int>(AudioRolloff::Count)) ? static_cast<AudioRolloff>(r)
                                                                       : AudioRolloff::Logarithmic;
    }
    if (j.contains("clips") && j["clips"].is_array()) {
      for (const auto &cj : j["clips"]) {
        AudioClipSlot s;
        s.name = cj.value("name", std::string());
        s.path = cj.value("path", std::string());
        s.volume = cj.value("volume", 1.0f);
        s.loop = cj.value("loop", false);
        const int sp = cj.value("spatial", static_cast<int>(AudioSpatialMode::Inherit));
        s.spatial = (sp >= 0 && sp < static_cast<int>(AudioSpatialMode::Count)) ? static_cast<AudioSpatialMode>(sp)
                                                                                 : AudioSpatialMode::Inherit;
        clips.push_back(std::move(s));
      }
    }
  }

private:
  /// @struct SeVoice
  /// @brief 自分が鳴らした SE ボイス 1 本分の記録
  struct SeVoice {
    int id = -1;          ///< AudioEngine のボイス ID
    std::string slot;     ///< 鳴らしたスロット名（2D/3D 判定を毎フレーム追従させるため）
    bool spatial = false; ///< 直前のフレームまで 3D で送っていたか（2D に戻す 1 回分のため）
  };

  /// @brief 鳴らしている SE をすべて止める
  void StopSe_() {
    auto &engine = AudioEngine::Get();
    for (const auto &v : seVoices_) {
      engine.StopVoice(v.id);
    }
    seVoices_.clear();
  }

  /// @brief 鳴り終わった SE の ID を捨てる
  void PruneVoices_() {
    const auto &engine = AudioEngine::Get();
    for (size_t i = seVoices_.size(); i-- > 0;) {
      if (!engine.IsVoicePlaying(seVoices_[i].id)) {
        seVoices_.erase(seVoices_.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
  }

  /// @brief BGM の keep-alive と音量の同期
  void KeepBgm_() {
    if (bus != AudioBus::BGM || bgmSlotName_.empty()) {
      return;
    }
    const AudioClipSlot *s = FindSlot(bgmSlotName_);
    if (!s || !s->IsLoaded()) {
      bgmSlotName_.clear();
      return;
    }
    AudioEngine::Get().KeepBgmAlive(s->clipHandle, volume * s->volume * bgmVolScale_);
  }

  /// @brief 鳴らしている SE のうちループしているものだけ止め、単発の SE は鳴り切らせる
  /// @details エンティティ破棄時用。撃破音を鳴らした直後に Destroy() されても音が切れない。
  ///          ボイス自身がクリップの参照を持つので、コンポーネントが先に消えても安全。
  void DetachSe_() {
    auto &engine = AudioEngine::Get();
    for (const auto &v : seVoices_) {
      if (engine.IsVoiceLooping(v.id)) {
        engine.StopVoice(v.id);
      }
    }
    seVoices_.clear();
  }

  // ================================================================
  // 3D 音響
  // ================================================================

  /// @brief スロットの指定から実際の 2D/3D ブレンドを決める
  /// @param mode スロットの 2D / 3D 指定
  /// @return ブレンド値（0〜1。BGM バスは常に 0）
  float EffectiveBlend_(AudioSpatialMode mode) const {
    if (bus != AudioBus::SE) {
      return 0.0f; // BGM は常に 2D
    }
    switch (mode) {
    case AudioSpatialMode::Force2D: return 0.0f;
    case AudioSpatialMode::Force3D: return 1.0f;
    case AudioSpatialMode::Inherit:
    default: return std::clamp(spatialBlend, 0.0f, 1.0f);
    }
  }

  /// @brief 現在の設定と位置からエンジンへ渡すエミッターを作る
  /// @param out   エミッターの出力先
  /// @param blend このボイスの 2D/3D ブレンド（EffectiveBlend()）
  /// @return Transform が無ければ false（2D で鳴らす）
  bool BuildEmitter_(AudioEmitter &out, float blend) const {
    RC::Vector3 pos{};
    if (!GetWorldPosition(pos)) {
      return false;
    }
    out.position = pos;
    out.velocity = velocity_;
    out.spatialBlend = std::clamp(blend, 0.0f, 1.0f);
    out.minDistance = (std::max)(minDistance, 0.0f);
    out.maxDistance = (std::max)(maxDistance, out.minDistance + 0.01f);
    out.rolloff = rolloff;
    out.dopplerLevel = (std::max)(dopplerLevel, 0.0f);
    return true;
  }

  /// @brief 毎フレーム: 位置と速度を更新し、鳴っている SE ボイスへ送る
  /// @details ボイスごとに鳴らしたスロットの 2D/3D 指定を毎フレーム見直すので、再生中に
  ///          Inspector で切り替えてもその場で反映される。ずっと 2D のボイスには何も送らないので軽い。
  ///          3D → 2D に変わった直後は 1 回だけ「2D に戻せ」（blend 0）を送る。
  /// @param playing 再生中か（false なら速度を 0 にする）
  /// @param dt      前フレームからの経過時間（秒）
  void UpdateSpatial_(bool playing, float dt) {
    if (bus != AudioBus::SE) {
      ResetMotion_();
      return;
    }
    RC::Vector3 pos{};
    if (!GetWorldPosition(pos)) {
      ResetMotion_();
      return;
    }

    // 速度は再生中だけ推定する（編集モードでギズモを動かしてもドップラーがかからないように）。
    // 1 フレームで音速の半分以上動いたらテレポートとみなして 0 にする
    velocity_ = {0.0f, 0.0f, 0.0f};
    if (playing && hasPrevPos_ && dt > 0.0f) {
      const RC::Vector3 v = {(pos.x - prevPos_.x) / dt, (pos.y - prevPos_.y) / dt, (pos.z - prevPos_.z) / dt};
      const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
      if (std::isfinite(speed) && speed <= AudioEngine::Get().GetSpeedOfSound() * 0.5f) {
        velocity_ = v;
      }
    }
    prevPos_ = pos;
    hasPrevPos_ = true;

    if (seVoices_.empty()) {
      return;
    }
    AudioEmitter emitter{};
    if (!BuildEmitter_(emitter, 0.0f)) {
      return;
    }
    auto &engine = AudioEngine::Get();
    for (auto &v : seVoices_) {
      const AudioClipSlot *s = FindSlot(v.slot); // スロットが消えていればコンポーネント設定に従う
      const float blend = EffectiveBlend_(s ? s->spatial : AudioSpatialMode::Inherit);
      const bool spatial = blend > 0.0f;
      if (!spatial && !v.spatial) {
        continue; // ずっと 2D → 送るものが無い
      }
      emitter.spatialBlend = blend;
      engine.SetVoiceEmitter(v.id, emitter);
      v.spatial = spatial;
    }
  }

  /// @brief 速度推定の履歴を捨てる（非アクティブ化・解放時。次に動き出しても前の位置と差分を取らない）
  void ResetMotion_() {
    hasPrevPos_ = false;
    velocity_ = {0.0f, 0.0f, 0.0f};
  }

  bool awakeFired_ = false;      ///< playOnAwake を発火済みか（Stopped に戻ると false）
  std::string bgmSlotName_;      ///< BGM バスで鳴らし始めたスロット名（keep-alive 対象）
  float bgmVolScale_ = 1.0f;     ///< BGM を Play() したときの音量倍率（keep-alive 時にも適用）
  std::vector<SeVoice> seVoices_; ///< 鳴らした SE のボイス
  std::string lastMissingSlot_;  ///< 直前に警告した存在しないスロット名（ログ連打防止）

  // --- 3D 音響のランタイム状態（シリアライズしない）---
  RC::Vector3 prevPos_{0.0f, 0.0f, 0.0f};  ///< 前フレームのワールド座標（速度推定用）
  RC::Vector3 velocity_{0.0f, 0.0f, 0.0f}; ///< 推定した速度（ワールド単位/秒）
  bool hasPrevPos_ = false;                ///< prevPos_ が有効か
};
