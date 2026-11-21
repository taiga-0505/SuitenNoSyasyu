#pragma once
#include "Math/MathTypes.h" // RC::Vector3 / RC::Matrix4x4
#include "Sound.h"          // SoundData / SoundLoadAudio / SoundUnload
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include <xaudio2.h>

/// @brief 音を流す先のバス（ミキサーのグループ）
/// @details バスごとに音量を持ち、再生の性質もバスで決まる。
///          BGM … 常に 1 本だけ鳴る。同じクリップの再要求は「継続」になるので、
///                シーンを跨いで同じ BGM を置いておけば途切れない。
///          SE  … 重ね再生。鳴り終わったボイスは Update() で自動回収される。
enum class AudioBus : int {
  BGM = 0, ///< 背景音楽
  SE = 1,  ///< 効果音
  Count    ///< 要素数
};

/// @brief 3D 音響の距離減衰カーブ
enum class AudioRolloff : int {
  Logarithmic = 0, ///< 距離に反比例（現実的）。minDistance まで最大、maxDistance で無音
  Linear = 1,      ///< 直線的に減衰。minDistance → maxDistance で 1 → 0
  Count            ///< 要素数
};

/// @brief 3D 音響の音源（エミッター）パラメータ
/// @details AudioSourceComponent が毎フレーム作って PlaySe() / SetVoiceEmitter() に渡す。
///          位置・速度はワールド座標。距離の単位はワールド単位（このエンジンでは 1 = 1m 想定）。
struct AudioEmitter {
  RC::Vector3 position{0.0f, 0.0f, 0.0f}; ///< 音源のワールド座標
  RC::Vector3 velocity{0.0f, 0.0f, 0.0f}; ///< 音源の速度（ワールド単位/秒）。ドップラー効果に使う
  float spatialBlend = 1.0f;              ///< 0 = 2D（位置に関係なく鳴る）、1 = 完全に 3D。中間はブレンド
  float minDistance = 1.0f;               ///< この距離までは減衰しない（最大音量）
  float maxDistance = 50.0f;              ///< この距離で無音になる
  AudioRolloff rolloff = AudioRolloff::Logarithmic; ///< 減衰カーブ
  float dopplerLevel = 1.0f;              ///< ドップラー効果の強さ。0 で無効、1 で物理的に正しい値
};

/// @brief 3D 音響のリスナー（聞き手）の姿勢
/// @details 通常は描画中のカメラ。AudioListenerComponent を置けばそのエンティティになる。
struct AudioListenerPose {
  RC::Vector3 position{0.0f, 0.0f, 0.0f}; ///< ワールド座標
  RC::Vector3 forward{0.0f, 0.0f, 1.0f};  ///< 正面方向（正規化済み）
  RC::Vector3 up{0.0f, 1.0f, 0.0f};       ///< 上方向（正規化済み、forward と直交）
  RC::Vector3 velocity{0.0f, 0.0f, 0.0f}; ///< 速度（ワールド単位/秒）。ドップラー効果に使う
};

/// @brief XAudio2 を 1 本だけ持つオーディオエンジン（シングルトン）
/// @details 役割は 4 つ。
///          1. クリップ（デコード済み PCM）のキャッシュ。同じパスは 1 回しかロードしない。
///             参照カウントで管理し、参照が 0 になっても PurgeUnusedClips() まではキャッシュに残す
///             （エディタの再生/停止のたびに BGM を再デコードしないため）。
///          2. SE ボイスの生成と回収。
///          3. BGM ボイス（1 本）の管理。持ち主（AudioSourceComponent）が毎フレーム
///             KeepBgmAlive() を呼ぶ keep-alive 方式で、数フレーム誰も呼ばなければ自動停止する。
///             シーン遷移中に OnExit → OnEnter で持ち主が入れ替わっても曲が途切れない。
///          4. 3D 音響（X3DAudio）。SE ボイスに AudioEmitter を与えると、リスナーとの位置関係から
///             左右の定位（スピーカー構成に合わせた出力行列）・距離減衰・ドップラー効果を
///             毎フレーム Update() で計算してボイスに反映する。BGM は常に 2D。
/// @note 全メソッドは Init() 前・Term() 後に呼ばれても安全（何もしない）。
class AudioEngine {
public:
  /// @brief シングルトンを取得する
  /// @return AudioEngine の唯一のインスタンス
  /// @note 静的破棄順の問題を避けるため、インスタンスは意図的に解放しない。
  ///       リソースの解放は Term() で明示的に行う。
  static AudioEngine &Get();

  /// @brief 初期化する（XAudio2 / マスター / BGM・SE のサブミックス / X3DAudio を生成）
  /// @return 成功したら true。オーディオデバイスが無い環境では false を返し、以降は無音で動作する
  bool Init();

  /// @brief 終了処理（全ボイスを止め、全クリップを解放する）
  void Term();

  /// @brief 鳴り終わった SE の回収、持ち主のいない BGM の停止、3D ボイスの定位更新を行う（毎フレーム呼ぶ）
  void Update();

  /// @brief 初期化済みか
  /// @return XAudio2 を生成済みなら true
  bool IsInitialized() const { return xaudio_ != nullptr; }

  // ================================================================
  // クリップ（デコード済み PCM）
  // ================================================================

  /// @brief 音声ファイルをロードしてクリップハンドルを返す（キャッシュ済みなら同じハンドル）
  /// @param path ファイルパス（.wav / .mp3 など）
  /// @return クリップハンドル。失敗時 -1
  /// @note 返ったハンドルは使い終わったら UnloadClip() で参照を返すこと。
  int LoadClip(const std::string &path);

  /// @brief クリップの参照を 1 つ返す
  /// @details 参照が 0 になっても即座には解放しない（PurgeUnusedClips() で解放）。
  /// @param clip クリップハンドル（無効なら何もしない）
  void UnloadClip(int clip);

  /// @brief 誰も参照していないクリップをメモリから解放する
  /// @details シーン遷移の完了後（新シーンのロード後）に呼ぶと、前のシーンだけが
  ///          使っていたクリップが解放され、共有している BGM は残る。
  void PurgeUnusedClips();

  /// @brief 有効なクリップハンドルか
  /// @param clip クリップハンドル
  /// @return 範囲内かつ使用中のスロットなら true
  bool IsValidClip(int clip) const;

  /// @brief クリップのファイルパスを取得する
  /// @param clip クリップハンドル
  /// @return 正規化済みパス（無効なら空文字）
  const std::string &ClipPath(int clip) const;

  /// @brief クリップの長さを取得する
  /// @param clip クリップハンドル
  /// @return 長さ（秒。無効なら 0）
  float ClipDuration(int clip) const;

  /// @brief クリップのチャンネル数を取得する
  /// @param clip クリップハンドル
  /// @return チャンネル数（無効なら 0）
  /// @note 3D 音響はモノラル音源が最も自然に定位する。
  int ClipChannels(int clip) const;

  /// @brief キャッシュ中のクリップ数（デバッグ表示用）
  /// @return 使用中のスロット数
  size_t ClipCount() const;

  // ================================================================
  // SE（重ね再生）
  // ================================================================

  /// @brief SE を再生する
  /// @param clip    クリップハンドル
  /// @param volume  音量（0.0〜1.0、バス音量・マスター音量とは別に乗算される）
  /// @param loop    ループ再生するか（環境音など。止めるには StopVoice が必要）
  /// @param emitter 3D 音響のパラメータ。nullptr なら 2D で鳴る。
  ///                渡すと最初のサンプルから正しい定位・音量で鳴り始める（1 フレームだけ 2D で
  ///                大きく鳴ることがない）。以降は毎フレーム SetVoiceEmitter() で位置を更新する
  /// @return ボイス ID。失敗時 -1
  int PlaySe(int clip, float volume = 1.0f, bool loop = false, const AudioEmitter *emitter = nullptr);

  /// @brief ボイスを停止・破棄する（既に鳴り終わっていれば何もしない）
  /// @param voiceId ボイス ID
  void StopVoice(int voiceId);

  /// @brief ボイスが再生中か
  /// @param voiceId ボイス ID
  /// @return 再生中なら true
  bool IsVoicePlaying(int voiceId) const;

  /// @brief 再生中のボイスがループ再生か
  /// @param voiceId ボイス ID
  /// @return ループ再生中なら true（鳴り終わっていれば false）
  bool IsVoiceLooping(int voiceId) const;

  /// @brief 再生中ボイスの音量を変更する
  /// @param voiceId ボイス ID
  /// @param volume  音量（負なら 0 に丸める）
  void SetVoiceVolume(int voiceId, float volume);

  /// @brief 再生中ボイスの 3D パラメータ（位置など）を更新する（毎フレーム呼ぶ）
  /// @details spatialBlend が 0 なら 2D に戻る（元々 2D のボイスに 0 を渡しても何もしない）。
  ///          実際の計算は次の Update() でまとめて行う。
  /// @param voiceId ボイス ID
  /// @param emitter 最新のエミッターパラメータ
  void SetVoiceEmitter(int voiceId, const AudioEmitter &emitter);

  /// @brief ボイスが 3D 音響で鳴っているか
  /// @param voiceId ボイス ID
  /// @return 3D 音響で鳴っていれば true
  bool IsVoiceSpatial(int voiceId) const;

  /// @brief 全 SE を停止する
  void StopAllSe();

  /// @brief 再生中の SE ボイス数（デバッグ表示用）
  /// @return SE ボイス数
  size_t ActiveSeCount() const { return seVoices_.size(); }

  /// @brief 再生中の 3D SE ボイス数（デバッグ表示用）
  /// @return 3D 音響で鳴っている SE ボイス数
  size_t ActiveSpatialCount() const;

  // ================================================================
  // BGM（常に 1 本）
  // ================================================================

  /// @brief BGM を再生する。同じクリップが既に鳴っていれば継続（音量だけ反映）、違えば差し替える
  /// @param clip   クリップハンドル
  /// @param volume 音量（0.0〜1.0）
  /// @param loop   ループ再生するか
  void PlayBgm(int clip, float volume = 1.0f, bool loop = true);

  /// @brief BGM の持ち主が「まだ必要」と伝える（毎フレーム呼ぶ）
  /// @param clip   自分が鳴らしているクリップ。現在の BGM と一致しなければ何もしない
  /// @param volume 現在の希望音量（Inspector での変更をリアルタイム反映するため）
  /// @details 呼ばれなくなってから kBgmGraceFrames 回目の Update() で BGM を止める
  ///          （それまでの数フレームは鳴り続ける）。
  ///          「シーンから BGM を持つエンティティが消えたら自然に止まる」を、
  ///          明示的な所有権管理なしで実現するための仕組み。
  void KeepBgmAlive(int clip, float volume);

  /// @brief BGM を即時停止する
  void StopBgm();

  /// @brief BGM が鳴っているか
  /// @return BGM ボイスがあれば true
  bool IsBgmPlaying() const { return bgm_.voice != nullptr; }

  /// @brief 現在鳴っている BGM のクリップハンドルを取得する
  /// @return クリップハンドル（鳴っていなければ -1）
  int CurrentBgmClip() const { return bgm_.voice ? bgm_.clip : -1; }

  // ================================================================
  // 3D 音響（リスナー）
  // ================================================================

  /// @brief 3D 音響が使えるか（X3DAudio の初期化に成功したか）
  /// @return 使えるなら true。false なら 3D 指定の音も 2D で鳴る
  bool Is3DAvailable() const { return x3dReady_; }

  /// @brief リスナー（聞き手）の姿勢を速度込みで設定する（毎フレーム呼ぶ）
  /// @param pose リスナーの姿勢（forward / up は内部で正規直交化する）
  void SetListener(const AudioListenerPose &pose);

  /// @brief リスナーの位置と向きを設定する。速度は前回位置との差分から自動で求める（毎フレーム呼ぶ）
  /// @param position ワールド座標
  /// @param forward  正面方向（正規化していなくてよい）
  /// @param up       上方向（正規化していなくてよい。forward と直交していなければ直交化する）
  /// @param dt       前フレームからの経過時間（秒）。0 以下なら速度は 0 とする
  void SetListenerPose(const RC::Vector3 &position, const RC::Vector3 &forward, const RC::Vector3 &up, float dt);

  /// @brief カメラのビュー行列からリスナーを設定する（AudioListenerComponent が無いシーン用）
  /// @param view ワールド → ビューの行列（CameraController::GetView()）
  /// @param dt   前フレームからの経過時間（秒）
  void SetListenerFromView(const RC::Matrix4x4 &view, float dt);

  /// @brief 現在のリスナーを取得する
  /// @return リスナーの姿勢
  const AudioListenerPose &GetListener() const { return listener_; }

  /// @brief 音速を設定する（ドップラー効果の強さに関わる。既定は 343.5（空気中・m 単位））
  /// @details ワールドの 1 単位が 1m でないゲームでは、その比率に合わせて変える。
  /// @param unitsPerSecond 音速（ワールド単位/秒。1 未満は 1 に丸める）
  void SetSpeedOfSound(float unitsPerSecond);

  /// @brief 音速を取得する
  /// @return 音速（ワールド単位/秒）
  float GetSpeedOfSound() const { return speedOfSound_; }

  /// @brief エンティティのワールド行列からリスナー姿勢を作る（Transform の +Z を正面、+Y を上とする）
  /// @param world ワールド行列
  /// @return リスナー姿勢（速度は 0）
  static AudioListenerPose PoseFromWorldMatrix(const RC::Matrix4x4 &world);

  /// @brief カメラのビュー行列からリスナー姿勢を作る
  /// @param view ワールド → ビューの行列
  /// @return リスナー姿勢（速度は 0）
  static AudioListenerPose PoseFromViewMatrix(const RC::Matrix4x4 &view);

  /// @brief 距離減衰を計算する（Inspector のプレビュー表示などにも使える）
  /// @param distance    音源とリスナーの距離
  /// @param minDistance この距離までは減衰しない
  /// @param maxDistance この距離で無音になる
  /// @param rolloff     減衰カーブ
  /// @return 音量倍率（0.0〜1.0）
  static float ComputeRolloff(float distance, float minDistance, float maxDistance, AudioRolloff rolloff);

  // ================================================================
  // バス / マスター音量
  // ================================================================

  /// @brief バス音量を設定する
  /// @param bus    対象のバス
  /// @param volume 音量（0.0〜1.0 に丸める）
  void SetBusVolume(AudioBus bus, float volume);

  /// @brief バス音量を取得する
  /// @param bus 対象のバス
  /// @return 音量（範囲外のバスなら 0）
  float GetBusVolume(AudioBus bus) const;

  /// @brief マスター音量を設定する
  /// @param volume 音量（0.0〜1.0 に丸める）
  void SetMasterVolume(float volume);

  /// @brief マスター音量を取得する
  /// @return 音量（0.0〜1.0）
  float GetMasterVolume() const { return masterVolume_; }

  /// @brief BGM・SE をすべて停止する
  void StopAll();

  /// @brief BGM が持ち主なしで生き残るフレーム数（KeepBgmAlive が呼ばれなくなってからの猶予）
  static constexpr int kBgmGraceFrames = 3;

private:
  AudioEngine() = default;
  ~AudioEngine() = default;
  AudioEngine(const AudioEngine &) = delete;
  AudioEngine &operator=(const AudioEngine &) = delete;

  /// @brief SourceVoice のカスタムデリータ
  struct SourceVoiceDeleter {
    void operator()(IXAudio2SourceVoice *v) const noexcept {
      if (v) {
        v->Stop(0);
        v->FlushSourceBuffers();
        v->DestroyVoice(); // 音声スレッドがこのボイスを使い終わるまでブロックする
      }
    }
  };
  /// @brief SubmixVoice のカスタムデリータ
  struct SubmixVoiceDeleter {
    void operator()(IXAudio2SubmixVoice *v) const noexcept {
      if (v) {
        v->DestroyVoice();
      }
    }
  };
  /// @brief MasteringVoice のカスタムデリータ
  struct MasterVoiceDeleter {
    void operator()(IXAudio2MasteringVoice *v) const noexcept {
      if (v) {
        v->DestroyVoice();
      }
    }
  };
  using SourceVoicePtr = std::unique_ptr<IXAudio2SourceVoice, SourceVoiceDeleter>;
  using SubmixVoicePtr = std::unique_ptr<IXAudio2SubmixVoice, SubmixVoiceDeleter>;
  using MasterVoicePtr = std::unique_ptr<IXAudio2MasteringVoice, MasterVoiceDeleter>;

  /// @brief キャッシュ中のクリップ
  struct Clip {
    std::string path;   ///< 正規化済みパス（キャッシュのキー）
    SoundData data{};   ///< PCM データ
    int refCount = 0;   ///< コンポーネントからの参照数 + 再生中ボイス数
    bool used = false;  ///< スロットが使用中か（false なら再利用可）
  };

  /// @brief 再生中のボイス
  struct Voice {
    int id = -1;            ///< ボイス ID
    int clip = -1;          ///< 参照しているクリップ（refCount を 1 つ持つ）
    float volume = 1.0f;    ///< 現在の音量
    bool loop = false;      ///< ループ再生か
    SourceVoicePtr voice{}; ///< XAudio2 のボイス本体

    // --- 3D 音響 ---
    IXAudio2Voice *dest = nullptr;  ///< 送り先（SetOutputMatrix 用）。nullptr ならマスター直結
    uint32_t srcChannels = 0;       ///< 音源のチャンネル数
    bool spatial = false;           ///< 3D 音響で鳴っているか（emitter を反映中か）
    AudioEmitter emitter{};         ///< 最新のエミッター（毎フレーム SetVoiceEmitter で更新）
    std::vector<float> flatMatrix;  ///< 2D のときの出力行列（作成直後の既定値。ブレンドの基準）
    std::vector<float> workMatrix;  ///< 計算用の出力行列
  };

  /// @brief クリップを積んだソースボイスを作る（まだ鳴らさない）
  /// @param clip   クリップハンドル
  /// @param bus    送り先のバス（サブミックスが無ければマスターへ直接送る）
  /// @param volume 音量
  /// @param loop   ループ再生するか
  /// @return ソースボイス。失敗時 nullptr
  /// @note 3D の初期定位を先に反映できるよう、Start() は呼び出し側で行う。
  SourceVoicePtr CreateVoice_(int clip, AudioBus bus, float volume, bool loop);
  /// @brief ボイスの再生を開始する
  /// @param v 対象のボイス
  /// @return 開始できたら true
  bool StartVoice_(Voice &v);
  /// @brief ボイスを破棄し、クリップの参照を返す
  /// @param v 対象のボイス
  void DestroyVoice_(Voice &v);
  /// @brief ボイス側からクリップの参照を返す（挙動は UnloadClip と同じ）
  /// @param clip クリップハンドル
  void ReleaseClipRef_(int clip);
  /// @brief クリップの PCM を解放し、スロットを空きに戻す
  /// @param c 対象のクリップ
  void FreeClip_(Clip &c);
  /// @brief 空きスロットを探す（無ければ末尾に追加する）
  /// @return スロットのインデックス（＝クリップハンドル）
  int AllocClipSlot_();
  /// @brief ID から再生中の SE ボイスを探す
  /// @param voiceId ボイス ID
  /// @return 見つかったボイス（無ければ nullptr）
  Voice *FindSeVoice_(int voiceId);
  /// @brief ID から再生中の SE ボイスを探す（const 版）
  /// @param voiceId ボイス ID
  /// @return 見つかったボイス（無ければ nullptr）
  const Voice *FindSeVoice_(int voiceId) const;
  /// @brief ボイスが鳴り終わったか（キューにバッファが残っていないか）
  /// @param v 対象のボイス
  /// @return 鳴り終わっていれば true（ボイスが無い場合も true）
  static bool IsVoiceFinished_(const Voice &v);
  /// @brief 区切り文字を '/' に揃え、"." や ".." を畳んだパスにする
  /// @param path 元のパス
  /// @return 正規化したパス
  static std::string NormalizePath_(const std::string &path);
  /// @brief キャッシュのキーを作る（Windows は大文字小文字を区別しないので小文字に寄せる）
  /// @param normalizedPath 正規化済みパス
  /// @return キャッシュのキー
  static std::string CacheKey_(const std::string &normalizedPath);

  /// @brief 現在のスピーカー構成と音速で X3DAudio を初期化する
  /// @return 成功したら true
  bool InitX3D_();
  /// @brief 2D のときの出力行列（作成直後の既定値）を取得して保存する
  /// @param v 対象のボイス
  /// @return 取得できたら（または取得済みなら）true
  bool CaptureFlatMatrix_(Voice &v);
  /// @brief リスナーとエミッターから定位・距離減衰・ドップラーを計算してボイスに反映する
  /// @param v 対象のボイス
  void ApplySpatial_(Voice &v);
  /// @brief 出力行列とピッチを 2D の状態に戻す
  /// @param v 対象のボイス
  void ResetSpatial_(Voice &v);

  Microsoft::WRL::ComPtr<IXAudio2> xaudio_; ///< XAudio2 本体
  MasterVoicePtr master_{};                 ///< マスタリングボイス
  SubmixVoicePtr submix_[static_cast<int>(AudioBus::Count)]{}; ///< バスごとのサブミックス
  float busVolume_[static_cast<int>(AudioBus::Count)] = {1.0f, 1.0f}; ///< バス音量
  float masterVolume_ = 1.0f;                                          ///< マスター音量

  std::vector<Clip> clips_;                          ///< クリップ（ハンドル = インデックス）
  std::unordered_map<std::string, int> clipByPath_;  ///< パス → ハンドル

  std::vector<Voice> seVoices_; ///< 再生中の SE
  Voice bgm_{};                 ///< BGM（voice が null なら無音）
  int bgmKeepAlive_ = 0;        ///< 残り猶予フレーム数。0 で BGM を止める
  int nextVoiceId_ = 1;         ///< 次に発行するボイス ID

  // --- 3D 音響 ---
  /// @brief X3DAudio のインスタンスハンドル（X3DAUDIO_HANDLE = BYTE[20]）
  /// @details x3daudio.h（DirectXMath 込み）を全コンポーネントに波及させないため、生バイトで持つ。
  ///          サイズは AudioEngine.cpp の static_assert で検証している。
  alignas(16) unsigned char x3dInstance_[20] = {};
  bool x3dReady_ = false;                 ///< X3DAudioInitialize に成功したか
  uint32_t dstChannels_ = 0;              ///< 出力（マスター / サブミックス）のチャンネル数
  uint32_t channelMask_ = 0;              ///< スピーカー構成（WAVEFORMATEXTENSIBLE.dwChannelMask 形式）
  float speedOfSound_ = 343.5f;           ///< 音速（ワールド単位/秒）
  AudioListenerPose listener_{};          ///< 現在のリスナー
  RC::Vector3 listenerPrevPos_{0.0f, 0.0f, 0.0f}; ///< 前回のリスナー位置（速度推定用）
  bool listenerHasPrev_ = false;          ///< listenerPrevPos_ が有効か
};
