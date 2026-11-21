#pragma once
#include "AudioHrtf.h"      // Steam Audio の HRTF エフェクト
#include "Math/MathTypes.h" // RC::Vector3 / RC::Matrix4x4
#include "Sound.h"          // SoundData / SoundLoadAudio / SoundUnload
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include <xaudio2.h>

/// @brief 音を流す先のバス（ミキサーのグループ）
/// @details バスごとに音量を持ち、再生の性質もバスで決まる。
///          BGM      … 常に 1 本だけ鳴る。同じクリップの再要求は「継続」になるので、
///                     シーンを跨いで同じ BGM を置いておけば途切れない。常に 2D。
///          SE       … 重ね再生。鳴り終わったボイスは Update() で自動回収される。
///          Voice    … セリフ・囁き。SE と同じく重ね再生・3D 可。鳴っている間は
///                     ダッキング対象のバス（既定: BGM / Ambience）の音量を自動で下げる。
///          Ambience … 環境音（雨・焚き火・部屋鳴りなど）。重ね再生・3D 可。
///          Foley    … 耳かき・ブラッシング・タッピングなどの効果音。重ね再生・3D 可。
/// @note 数値はシーン JSON に保存されるので変えないこと（追加は末尾へ）。
enum class AudioBus : int {
  BGM = 0,      ///< 背景音楽
  SE = 1,       ///< 効果音
  Voice = 2,    ///< セリフ・囁き
  Ambience = 3, ///< 環境音
  Foley = 4,    ///< 耳かき・ブラッシングなどの効果音
  Count         ///< 要素数
};

/// @brief バスの表示名（ログ・Inspector 用）
/// @param bus バス
/// @return 英語の短い名前
inline const char *AudioBusName(AudioBus bus) {
  switch (bus) {
  case AudioBus::BGM: return "BGM";
  case AudioBus::SE: return "SE";
  case AudioBus::Voice: return "Voice";
  case AudioBus::Ambience: return "Ambience";
  case AudioBus::Foley: return "Foley";
  default: return "?";
  }
}

/// @brief バスのビットマスク（ダッキング対象の指定などに使う）
/// @param bus バス
/// @return 1 << bus
constexpr uint32_t AudioBusBit(AudioBus bus) { return 1u << static_cast<uint32_t>(bus); }

/// @brief 3D 音響の距離減衰カーブ
enum class AudioRolloff : int {
  Logarithmic = 0, ///< 距離に反比例（現実的）。minDistance まで最大、maxDistance で無音
  Linear = 1,      ///< 直線的に減衰。minDistance → maxDistance で 1 → 0
  Count            ///< 要素数
};

/// @brief クリップの読み込み方
enum class AudioLoadMode : int {
  Auto = 0,   ///< 長さで自動判定（AudioEngine::SetStreamThreshold() 秒より長ければストリーミング）
  Memory = 1, ///< 全体を PCM に展開してメモリに置く（短い SE 向け。同時に何本鳴らしても軽い）
  Stream = 2, ///< 少しずつ読みながら再生する（長いボイス・環境音・BGM 向け。常駐メモリが小さい）
  Count       ///< 要素数
};

/// @brief 部屋の響き（共有リバーブのプリセット）
/// @details 各ボイスは reverbSend の量だけ共有リバーブに送られる。None なら響かない。
/// @note 数値は保存されることがあるので変えないこと（追加は末尾へ）。
enum class AudioRoom : int {
  None = 0,       ///< 響きなし
  SmallRoom = 1,  ///< 小部屋
  MediumRoom = 2, ///< 中くらいの部屋
  LargeRoom = 3,  ///< 広い部屋
  Bathroom = 4,   ///< 浴室（よく響く）
  LivingRoom = 5, ///< 居間（家具で響きが少ない）
  StoneRoom = 6,  ///< 石造りの部屋
  Hallway = 7,    ///< 廊下
  Cave = 8,       ///< 洞窟
  Forest = 9,     ///< 森
  Underwater = 10,///< 水中
  PaddedCell = 11,///< 吸音された小部屋（ほぼデッド）
  Count           ///< 要素数
};

/// @brief 部屋の表示名
/// @param room 部屋
/// @return 日本語の名前
inline const char *AudioRoomName(AudioRoom room) {
  switch (room) {
  case AudioRoom::None: return "なし";
  case AudioRoom::SmallRoom: return "小部屋";
  case AudioRoom::MediumRoom: return "中部屋";
  case AudioRoom::LargeRoom: return "広い部屋";
  case AudioRoom::Bathroom: return "浴室";
  case AudioRoom::LivingRoom: return "居間";
  case AudioRoom::StoneRoom: return "石の部屋";
  case AudioRoom::Hallway: return "廊下";
  case AudioRoom::Cave: return "洞窟";
  case AudioRoom::Forest: return "森";
  case AudioRoom::Underwater: return "水中";
  case AudioRoom::PaddedCell: return "吸音室";
  default: return "?";
  }
}

/// @brief 3D 音響の方式
/// @details HRTF はモノラル音源にだけ使う（ステレオ音源・BGM は従来どおり）。
///          切り替えると、鳴っている 3D の音もその位置から作り直して反映する。
enum class AudioSpatializer : int {
  Auto = 0,    ///< 出力がステレオ（ヘッドホン想定）で Steam Audio が使えれば HRTF、それ以外はパンニング
  Panning = 1, ///< 常に X3DAudio のパンニング（左右の音量差のみ。軽い）
  Hrtf = 2,    ///< 使えるなら常に HRTF（5.1ch 等でも前方 L/R に出す。スピーカーでは不自然になる）
  Count        ///< 要素数
};

/// @brief 出力デバイスの種類（ASMR では「ヘッドホン推奨」の案内などに使う）
enum class AudioOutputType : int {
  Unknown = 0,    ///< 判別できない
  Speakers = 1,   ///< スピーカー（内蔵・外付け・HDMI など）
  Headphones = 2, ///< ヘッドホン・ヘッドセット
};

/// @brief 3D 音響の音源（エミッター）パラメータ
/// @details AudioSourceComponent が毎フレーム作って Play() / SetVoiceEmitter() に渡す。
///          位置・速度はワールド座標。距離の単位はワールド単位（このエンジンでは 1 = 1m 想定）。
struct AudioEmitter {
  RC::Vector3 position{0.0f, 0.0f, 0.0f}; ///< 音源のワールド座標
  RC::Vector3 velocity{0.0f, 0.0f, 0.0f}; ///< 音源の速度（ワールド単位/秒）。ドップラー効果に使う
  float spatialBlend = 1.0f;              ///< 0 = 2D（位置に関係なく鳴る）、1 = 完全に 3D。中間はブレンド
  float minDistance = 1.0f;               ///< この距離までは減衰しない（最大音量）
  float maxDistance = 50.0f;              ///< この距離で無音になる
  AudioRolloff rolloff = AudioRolloff::Logarithmic; ///< 減衰カーブ
  float dopplerLevel = 0.0f;              ///< ドップラー効果の強さ。0 で無効、1 で物理的に正しい値
  /// @brief この半径の内側では音が全方向に滲む（真横・真上を通過してもパンが暴れない）
  /// @details 以前は minDistance と同じ値に固定されていた。耳元（数 cm）の定位を残したい
  ///          ASMR では 0.01〜0.03 程度に小さくする。
  float innerRadius = 1.0f;
  /// @brief 近接効果が効き始める距離（0 で無効。ステレオ出力のときだけ働く）
  /// @details これより近く、かつ横にある音源ほど、遠い側の耳を nearFieldIldDb まで減衰させる
  ///          （頭が音を遮る効果の簡易版。片耳だけに囁かれる感覚が出る）。
  float nearFieldDistance = 0.0f;
  float nearFieldIldDb = 12.0f; ///< 近接効果で遠い側の耳を最大どれだけ下げるか（dB）
};

/// @brief 3D 音響のリスナー（聞き手）の姿勢
/// @details 通常は描画中のカメラ。AudioListenerComponent を置けばそのエンティティになる。
struct AudioListenerPose {
  RC::Vector3 position{0.0f, 0.0f, 0.0f}; ///< ワールド座標
  RC::Vector3 forward{0.0f, 0.0f, 1.0f};  ///< 正面方向（正規化済み）
  RC::Vector3 up{0.0f, 1.0f, 0.0f};       ///< 上方向（正規化済み、forward と直交）
  RC::Vector3 velocity{0.0f, 0.0f, 0.0f}; ///< 速度（ワールド単位/秒）。ドップラー効果に使う
};

/// @brief ダッキング（Voice バスが鳴っている間、他のバスを自動で下げる）の設定
struct AudioDuckingSettings {
  bool enabled = true;      ///< 有効か
  float depthDb = -9.0f;    ///< 下げる量（dB。負の値）
  float attackSec = 0.15f;  ///< 下げ始めてから depthDb に達するまでの目安（秒）
  float releaseSec = 0.8f;  ///< Voice が止んでから元に戻るまでの目安（秒）
  uint32_t targetMask = AudioBusBit(AudioBus::BGM) | AudioBusBit(AudioBus::Ambience); ///< 下げるバス
};

/// @brief マスターのリミッター（突発的な大音量で音割れ・耳を痛めるのを防ぐ）の設定
struct AudioLimiterSettings {
  bool enabled = true;     ///< 有効か
  uint32_t release = 6;    ///< リリース（1〜20。大きいほどゆっくり戻る）
  uint32_t loudness = 1000; ///< ラウドネス（1〜1800。大きいほど持ち上げる。1000 が標準）
};

/// @brief XAudio2 を 1 本だけ持つオーディオエンジン（シングルトン）
/// @details 役割は次のとおり。
///          1. クリップのキャッシュ。短いものはデコード済み PCM を持ち、長いものはストリーミング
///             （再生中に少しずつデコード）する。同じパスは 1 回しかロードしない。
///             参照カウントで管理し、参照が 0 になっても PurgeUnusedClips() まではキャッシュに残す。
///          2. ボイス（再生中の音）の生成と回収。一時停止・フェード・再生位置の取得・シークに対応する。
///             ゲーム全体の一時停止（SetPaused）では BGM 以外のバスが止まる。
///          3. BGM ボイス（1 本）の管理。持ち主（AudioSourceComponent）が毎フレーム
///             KeepBgmAlive() を呼ぶ keep-alive 方式で、数フレーム誰も呼ばなければ自動停止する。
///             曲の差し替えはクロスフェードできる。
///          4. 3D 音響。ボイスに AudioEmitter を与えると、リスナーとの位置関係から
///             定位・距離減衰・ドップラー効果・耳元の近接効果を毎フレーム Update() で反映する。
///             モノラル音源はヘッドホン出力なら Steam Audio の HRTF（前後・上下も分かる）、
///             それ以外は X3DAudio のパンニングで定位させる（SetSpatializer で切り替え可）。
///          5. ミキサー。バス音量、Voice バスによるダッキング、マスターのリミッター。
///          6. 出力デバイスの変更（ヘッドホンの抜き差しなど）への追従。デバイスが変わると
///             エンジンを作り直し、鳴っていた音を同じ位置から再開する。
/// @note 全メソッドは Init() 前・Term() 後に呼ばれても安全（何もしない）。メインスレッドから呼ぶこと。
class AudioEngine {
public:
  /// @brief シングルトンを取得する
  /// @return AudioEngine の唯一のインスタンス
  /// @note 静的破棄順の問題を避けるため、インスタンスは意図的に解放しない。
  ///       リソースの解放は Term() で明示的に行う。
  static AudioEngine &Get();

  /// @brief 初期化する（XAudio2 / マスター / バスのサブミックス / リミッター / X3DAudio / ストリーミングスレッド）
  /// @return 成功したら true。オーディオデバイスが無い環境では false を返し、以降は無音で動作する
  ///         （後からデバイスが接続されれば Update() で自動的に初期化し直す）
  bool Init();

  /// @brief 終了処理（全ボイスを止め、全クリップを解放する）
  void Term();

  /// @brief 毎フレーム呼ぶ。鳴り終わった音の回収、一時停止とフェードの進行、BGM の keep-alive、
  ///        3D ボイスの定位更新、ダッキング、デバイス変更への追従を行う
  void Update();

  /// @brief 初期化済みか
  /// @return XAudio2 を生成済みなら true
  bool IsInitialized() const { return xaudio_ != nullptr; }

  // ================================================================
  // クリップ
  // ================================================================

  /// @brief 音声ファイルをロードしてクリップハンドルを返す（キャッシュ済みなら同じハンドル）
  /// @param path ファイルパス（.wav / .mp3 など）
  /// @param mode 読み込み方（既定は長さで自動判定。キャッシュ済みなら最初の読み込み方のまま）
  /// @return クリップハンドル。失敗時 -1
  /// @note 返ったハンドルは使い終わったら UnloadClip() で参照を返すこと。
  int LoadClip(const std::string &path, AudioLoadMode mode = AudioLoadMode::Auto);

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
  /// @return 長さ（秒。無効・不明なら 0）
  float ClipDuration(int clip) const;

  /// @brief クリップのチャンネル数を取得する
  /// @param clip クリップハンドル
  /// @return チャンネル数（無効なら 0）
  /// @note 3D 音響はモノラル音源が最も自然に定位する。
  int ClipChannels(int clip) const;

  /// @brief クリップがストリーミング再生か
  /// @param clip クリップハンドル
  /// @return ストリーミングなら true（メモリ展開・無効なら false）
  bool IsClipStreamed(int clip) const;

  /// @brief キャッシュ中のクリップ数（デバッグ表示用）
  /// @return 使用中のスロット数
  size_t ClipCount() const;

  /// @brief AudioLoadMode::Auto でストリーミングに切り替える長さ（秒）を設定する
  /// @param seconds この秒数より長いクリップはストリーミングになる（既定 20 秒）
  void SetStreamThreshold(float seconds) { streamThresholdSec_ = (seconds > 0.0f) ? seconds : 0.0f; }

  /// @brief AudioLoadMode::Auto でストリーミングに切り替える長さ（秒）
  /// @return 秒数
  float GetStreamThreshold() const { return streamThresholdSec_; }

  // ================================================================
  // 再生（全バス共通）
  // ================================================================

  /// @brief 重ね再生のバス（SE / Voice / Ambience / Foley）で鳴らす
  /// @param clip       クリップハンドル
  /// @param bus        送り先のバス（BGM を指定した場合は PlayBgm() と同じ扱い）
  /// @param volume     音量（0.0〜1.0、フェード・バス音量・マスター音量とは別に乗算される）
  /// @param loop       ループ再生するか（止めるには StopVoice が必要）
  /// @param emitter    3D 音響のパラメータ。nullptr なら 2D で鳴る。
  ///                   渡すと最初のサンプルから正しい定位・音量で鳴り始める
  /// @param fadeInSec  フェードインの秒数（0 なら即座に最大音量）
  /// @param lowPassHz  ローパス（こもり）のカットオフ Hz（0 で無効）。最初のサンプルから掛かる
  /// @param reverbSend 部屋の響きへの送り量（0〜1）
  /// @return ボイス ID。失敗時 -1
  int Play(int clip, AudioBus bus, float volume = 1.0f, bool loop = false, const AudioEmitter *emitter = nullptr,
           float fadeInSec = 0.0f, float lowPassHz = 0.0f, float reverbSend = 0.0f);

  /// @brief SE を再生する（Play(clip, AudioBus::SE, ...) と同じ。互換用）
  /// @param clip    クリップハンドル
  /// @param volume  音量
  /// @param loop    ループ再生するか
  /// @param emitter 3D 音響のパラメータ（nullptr なら 2D）
  /// @return ボイス ID。失敗時 -1
  int PlaySe(int clip, float volume = 1.0f, bool loop = false, const AudioEmitter *emitter = nullptr) {
    return Play(clip, AudioBus::SE, volume, loop, emitter, 0.0f);
  }

  /// @brief ボイスを停止する（既に鳴り終わっていれば何もしない）
  /// @param voiceId    ボイス ID
  /// @param fadeOutSec フェードアウトの秒数（0 なら即時停止。一時停止中のボイスは常に即時）
  void StopVoice(int voiceId, float fadeOutSec = 0.0f);

  /// @brief ボイスが存在し、鳴り終わっていないか（一時停止中・フェードアウト中も true）
  /// @param voiceId ボイス ID
  /// @return 鳴っていれば true
  bool IsVoicePlaying(int voiceId) const;

  /// @brief 再生中のボイスがループ再生か
  /// @param voiceId ボイス ID
  /// @return ループ再生中なら true（鳴り終わっていれば false）
  bool IsVoiceLooping(int voiceId) const;

  /// @brief ボイスがストリーミング再生か
  /// @param voiceId ボイス ID
  /// @return ストリーミングなら true
  bool IsVoiceStreaming(int voiceId) const;

  /// @brief 再生中ボイスの音量を変更する（フェードとは独立した「基本音量」）
  /// @param voiceId ボイス ID
  /// @param volume  音量（負なら 0 に丸める）
  void SetVoiceVolume(int voiceId, float volume);

  /// @brief ボイスの音量をフェードさせる（基本音量に掛かるフェード倍率を動かす）
  /// @param voiceId    ボイス ID
  /// @param targetGain 目標の倍率（0.0〜1.0）
  /// @param seconds    かける秒数（0 なら即座に反映）
  void FadeVoice(int voiceId, float targetGain, float seconds);

  /// @brief ボイスにローパスフィルタを掛ける（こもった音・布越し・壁越し）
  /// @param voiceId   ボイス ID
  /// @param cutoffHz  カットオフ周波数（Hz）。0 以下か kLowPassOpenHz 以上で無効
  /// @param immediate true なら即座に反映（鳴らし始め用）。false なら約 0.1 秒かけて滑らかに変える
  /// @note XAudio2 のフィルタはサンプリングレートの 1/6（48kHz で 8kHz）より上を指定できないので、
  ///       それ以上は「無効」と同じ扱いになる。
  void SetVoiceLowPass(int voiceId, float cutoffHz, bool immediate = false);

  /// @brief ボイスを共有リバーブ（部屋の響き）へ送る量を設定する
  /// @param voiceId ボイス ID
  /// @param level   送り量（0〜1。0 で響かない）
  void SetVoiceReverbSend(int voiceId, float level);

  /// @brief ボイスを一時停止する（クリック音が出ないよう数十 ms で絞ってから止める）
  /// @param voiceId ボイス ID
  void PauseVoice(int voiceId);

  /// @brief 一時停止したボイスを再開する
  /// @param voiceId ボイス ID
  /// @note ゲーム全体の一時停止（SetPaused）中は、こちらで再開してもゲーム側の再開を待つ。
  void ResumeVoice(int voiceId);

  /// @brief ボイスが一時停止中か（個別の一時停止とゲーム全体の一時停止のどちらでも true）
  /// @param voiceId ボイス ID
  /// @return 一時停止中なら true
  bool IsVoicePaused(int voiceId) const;

  /// @brief ボイスが PauseVoice() で一時停止されているか（ゲーム全体の一時停止は含まない）
  /// @param voiceId ボイス ID
  /// @return PauseVoice() で止めていれば true
  bool IsVoicePausedByUser(int voiceId) const;

  /// @brief 再生位置を取得する
  /// @param voiceId ボイス ID
  /// @return クリップ先頭からの秒数（ループ中は 1 周内の位置。無効なら 0）
  float GetVoiceTime(int voiceId) const;

  /// @brief ボイスがシークされた回数（キュー判定で「シークで飛んだ」のか「進んだ」のかを区別するため）
  /// @param voiceId ボイス ID
  /// @return 回数（無効なら 0）
  uint32_t GetVoiceSeekCount(int voiceId) const;

  /// @brief ボイスが鳴らしているクリップの長さ
  /// @param voiceId ボイス ID
  /// @return 秒数（無効・不明なら 0）
  float GetVoiceDuration(int voiceId) const;

  /// @brief 再生位置を移動する（一時停止中ならそのまま一時停止を保つ）
  /// @param voiceId ボイス ID
  /// @param seconds クリップ先頭からの秒数（範囲外は丸める）
  void SeekVoice(int voiceId, float seconds);

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

  /// @brief BGM 以外の全ボイスを停止する
  /// @param fadeOutSec フェードアウトの秒数（0 なら即時）
  void StopAllSe(float fadeOutSec = 0.0f);

  /// @brief 再生中の BGM 以外のボイス数（デバッグ表示用）
  /// @return ボイス数
  size_t ActiveSeCount() const;

  /// @brief 再生中の 3D ボイス数（デバッグ表示用）
  /// @return 3D 音響で鳴っているボイス数
  size_t ActiveSpatialCount() const;

  /// @brief 再生中のストリーミングボイス数（デバッグ表示用）
  /// @return ストリーミングのボイス数
  size_t ActiveStreamCount() const;

  // ================================================================
  // BGM（常に 1 本）
  // ================================================================

  /// @brief BGM を再生する。同じクリップが既に鳴っていれば継続（音量だけ反映）、違えば差し替える
  /// @param clip    クリップハンドル
  /// @param volume  音量（0.0〜1.0）
  /// @param loop    ループ再生するか
  /// @param fadeSec 差し替え時のクロスフェード秒数（0 なら即座に切り替え）。初回再生ではフェードイン
  /// @param lowPassHz  ローパス（こもり）のカットオフ Hz（0 で無効。新しく鳴らすときだけ使う）
  /// @param reverbSend 部屋の響きへの送り量（新しく鳴らすときだけ使う）
  void PlayBgm(int clip, float volume = 1.0f, bool loop = true, float fadeSec = 0.0f, float lowPassHz = 0.0f,
               float reverbSend = 0.0f);

  /// @brief BGM の持ち主が「まだ必要」と伝える（毎フレーム呼ぶ）
  /// @param clip       自分が鳴らしているクリップ。現在の BGM と一致しなければ何もしない
  /// @param volume     現在の希望音量（Inspector での変更をリアルタイム反映するため）
  /// @param fadeOutSec 持ち主がいなくなったときのフェードアウト秒数
  /// @details 呼ばれなくなってから kBgmGraceFrames 回目の Update() で BGM を止める
  ///          （それまでの数フレームは鳴り続ける）。
  ///          「シーンから BGM を持つエンティティが消えたら自然に止まる」を、
  ///          明示的な所有権管理なしで実現するための仕組み。
  void KeepBgmAlive(int clip, float volume, float fadeOutSec = 0.0f);

  /// @brief BGM を停止する
  /// @param fadeOutSec フェードアウトの秒数（0 なら即時）
  void StopBgm(float fadeOutSec = 0.0f);

  /// @brief BGM が鳴っているか
  /// @return BGM ボイスがあれば true（フェードアウトで止めている途中は false）
  bool IsBgmPlaying() const { return FindVoice_(bgmVoiceId_) != nullptr; }

  /// @brief 現在鳴っている BGM のクリップハンドルを取得する
  /// @return クリップハンドル（鳴っていなければ -1）
  int CurrentBgmClip() const;

  /// @brief 現在鳴っている BGM のボイス ID（一時停止・シーク・再生位置の取得に使う）
  /// @return ボイス ID（鳴っていなければ -1）
  int CurrentBgmVoice() const { return IsBgmPlaying() ? bgmVoiceId_ : -1; }

  // ================================================================
  // 一時停止（ゲーム全体）
  // ================================================================

  /// @brief ゲーム全体を一時停止する（ポーズメニューなど）
  /// @details その時点で鳴っている BGM 以外のボイスを一時停止する（SetPauseAffectsBgm で BGM も対象にできる）。
  ///          一時停止中に新しく鳴らした音（メニューの UI 音など）は止まらない。
  /// @param paused 一時停止するなら true
  void SetPaused(bool paused);

  /// @brief ゲーム全体が一時停止中か（エディタの一時停止を含む）
  /// @return 一時停止中なら true
  bool IsPaused() const { return paused_ || editorPaused_; }

  /// @brief エディタの一時停止状態を伝える（シーンが毎フレーム呼ぶ。SetPaused とは独立）
  /// @param paused エディタが Paused なら true
  void SetEditorPaused(bool paused);

  /// @brief ゲーム全体の一時停止で BGM も止めるか（既定 false = BGM は流し続ける）
  /// @param affects 止めるなら true
  void SetPauseAffectsBgm(bool affects) { pauseAffectsBgm_ = affects; }

  /// @brief ゲーム全体の一時停止で BGM も止めるか
  /// @return 止めるなら true
  bool GetPauseAffectsBgm() const { return pauseAffectsBgm_; }

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
  // ミキサー（バス / マスター / ダッキング / リミッター）
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

  /// @brief バス全体にローパスフィルタを掛ける（「耳をふさがれた」「水に潜った」などの演出）
  /// @param bus      対象のバス
  /// @param cutoffHz カットオフ周波数（Hz）。0 以下か kLowPassOpenHz 以上で無効。約 0.1 秒かけて滑らかに変わる
  void SetBusLowPass(AudioBus bus, float cutoffHz);

  /// @brief バスのローパスの目標値
  /// @param bus 対象のバス
  /// @return カットオフ周波数（Hz。無効なら kLowPassOpenHz）
  float GetBusLowPass(AudioBus bus) const;

  /// @brief 部屋の響き（共有リバーブのプリセット）を切り替える
  /// @param room 部屋
  void SetRoom(AudioRoom room);

  /// @brief 部屋の響き
  /// @return 現在のプリセット
  AudioRoom GetRoom() const { return room_; }

  /// @brief 共有リバーブが使えるか（出力が 48kHz を超える環境などでは作れない）
  /// @return 使えるなら true
  bool IsReverbAvailable() const { return reverb_ != nullptr; }

  /// @brief ローパスを「無効」とみなす周波数（Hz）
  static constexpr float kLowPassOpenHz = 20000.0f;

  /// @brief ダッキングの設定を変える
  /// @param settings 新しい設定
  void SetDucking(const AudioDuckingSettings &settings);

  /// @brief ダッキングの設定
  /// @return 現在の設定
  const AudioDuckingSettings &GetDucking() const { return ducking_; }

  /// @brief 現在のダッキング倍率（1 = 下げていない。デバッグ表示用）
  /// @return 倍率（0.0〜1.0）
  float CurrentDuckGain() const { return duckGain_; }

  /// @brief リミッターの設定を変える
  /// @param settings 新しい設定
  void SetLimiter(const AudioLimiterSettings &settings);

  /// @brief リミッターの設定
  /// @return 現在の設定
  const AudioLimiterSettings &GetLimiter() const { return limiter_; }

  /// @brief リミッターが使えるか（作成に成功したか）
  /// @return 使えるなら true
  bool IsLimiterAvailable() const { return limiterReady_; }

  // ================================================================
  // HRTF（Steam Audio）
  // ================================================================

  /// @brief 3D 音響の方式を設定する（鳴っている 3D の音にもその場で反映する）
  /// @param mode 方式
  void SetSpatializer(AudioSpatializer mode);

  /// @brief 3D 音響の方式
  /// @return 設定値
  AudioSpatializer GetSpatializer() const { return spatializer_; }

  /// @brief Steam Audio（phonon.dll）が使えるか
  /// @return 使えるなら true
  bool IsHrtfAvailable() const;

  /// @brief 今 Play() するモノラルの 3D 音に HRTF が使われるか（方式・出力チャンネル数・DLL の有無から判定）
  /// @return 使われるなら true
  bool IsHrtfActive() const;

  /// @brief HRTF で鳴っているボイス数（デバッグ表示用）
  /// @return ボイス数
  size_t ActiveHrtfCount() const;

  /// @brief BGM・全ボイスを即時停止する
  void StopAll();

  // ================================================================
  // 出力デバイス
  // ================================================================

  /// @brief 既定の出力デバイスの種類
  /// @return ヘッドホン / スピーカー / 不明
  AudioOutputType GetOutputType() const { return outputType_; }

  /// @brief 出力のチャンネル数
  /// @return チャンネル数（未初期化なら 0）
  uint32_t GetOutputChannels() const { return dstChannels_; }

  /// @brief 出力のサンプリングレート
  /// @return Hz（未初期化なら 0）
  uint32_t GetOutputSampleRate() const { return outputSampleRate_; }

  /// @brief デバイス変更でエンジンを作り直した回数（デバッグ表示用）
  /// @return 回数
  int DeviceResetCount() const { return deviceResetCount_; }

  /// @brief 次の Update() でエンジンを作り直すよう要求する（設定画面の「出力デバイスを再検出」など）
  void RequestDeviceReset() { manualResetRequested_ = true; }

  /// @brief BGM が持ち主なしで生き残るフレーム数（KeepBgmAlive が呼ばれなくなってからの猶予）
  static constexpr int kBgmGraceFrames = 3;

  /// @brief ストリーミングで XAudio2 に積んでおくバッファの数
  static constexpr uint32_t kStreamBufferCount = 4;
  /// @brief ストリーミングのバッファ 1 つあたりの長さ（秒）。kStreamBufferCount 倍が先読み量になる
  static constexpr float kStreamBufferSeconds = 0.25f;

private:
  AudioEngine() = default;
  ~AudioEngine();
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

  /// @brief ストリーミング 1 本分の状態（AudioEngine.cpp で定義）
  struct StreamState;
  /// @brief デバイス変更の監視（IMMNotificationClient / IXAudio2EngineCallback。AudioEngine.cpp で定義）
  struct DeviceWatcher;
  /// @brief DeviceWatcher のデリータ（不完全型のまま unique_ptr に持たせるため）
  struct DeviceWatcherDeleter {
    void operator()(DeviceWatcher *w) const noexcept;
  };

  /// @brief キャッシュ中のクリップ
  struct Clip {
    std::string path;          ///< 正規化済みパス（キャッシュのキー）
    SoundData data{};          ///< PCM データ（ストリーミングなら空）
    WAVEFORMATEX fmt{};        ///< PCM の形式
    uint64_t totalFrames = 0;  ///< 全体のフレーム数（不明なら 0）
    bool streamed = false;     ///< ストリーミング再生か
    int refCount = 0;          ///< コンポーネントからの参照数 + 再生中ボイス数
    bool used = false;         ///< スロットが使用中か（false なら再利用可）
  };

  /// @brief フェードが終わったときの動作
  enum class FadeEnd : int {
    None = 0, ///< 何もしない
    Stop = 1, ///< ボイスを破棄する（フェードアウトして止める）
  };

  /// @brief 再生中のボイス
  struct Voice {
    int id = -1;               ///< ボイス ID
    int clip = -1;             ///< 参照しているクリップ（refCount を 1 つ持つ）
    AudioBus bus = AudioBus::SE; ///< 送り先のバス
    float volume = 1.0f;       ///< 基本音量
    bool loop = false;         ///< ループ再生か
    SourceVoicePtr voice{};    ///< XAudio2 のボイス本体
    std::shared_ptr<StreamState> stream; ///< ストリーミングの状態（メモリ再生なら null）

    // --- フェード ---
    float fadeGain = 1.0f;     ///< 現在のフェード倍率
    float fadeFrom = 1.0f;     ///< フェード開始時の倍率
    float fadeTo = 1.0f;       ///< フェードの目標倍率
    float fadeTime = 0.0f;     ///< フェード開始からの経過秒
    float fadeDuration = 0.0f; ///< フェードの長さ（0 ならフェードしていない）
    FadeEnd fadeEnd = FadeEnd::None; ///< フェードが終わったときの動作

    // --- 一時停止 ---
    bool pausedUser = false;   ///< PauseVoice() で止めている
    bool pausedGame = false;   ///< SetPaused() / エディタの一時停止で止めている
    bool stopped = false;      ///< XAudio2 のボイスを Stop() 済みか
    float pauseGain = 1.0f;    ///< 一時停止のクリック防止用の倍率（止める前に 0 へ絞る）
    uint64_t playedAtStop = 0; ///< Stop() した時点の SamplesPlayed
    bool checkPlayedReset = false; ///< Start() 後に SamplesPlayed が戻っていないか確認する
    uint64_t playedOffset = 0; ///< SamplesPlayed が Start() で 0 に戻った場合の補正

    // --- 再生位置 ---
    uint64_t startFrame = 0;   ///< この XAudio2 ボイスを鳴らし始めたクリップ内の位置（メモリ再生）

    // --- フィルタ / リバーブ ---
    float lowPassTarget = kLowPassOpenHz;  ///< ローパスの目標（Hz）
    float lowPassCurrent = kLowPassOpenHz; ///< 反映中のローパス（Hz）
    float reverbSend = 0.0f;               ///< リバーブへの送り量（0〜1）
    float appliedReverbGain = -1.0f;       ///< 反映済みの送り行列の倍率（-1 なら未反映）
    float appliedSendLowPass = -1.0f;      ///< リバーブ送りに反映済みのバスのローパス（-1 なら未反映）
    float spatialGain = 1.0f;              ///< 3D の距離減衰（リバーブ送りにも掛ける）
    uint32_t seekCount = 0;                ///< シークした回数

    // --- 3D 音響 ---
    AudioHrtf::EffectPtr hrtf;      ///< HRTF エフェクト（null ならパンニング）。XAudio2 のボイスより後に破棄する
    float hrtfTailSec = 0.0f;       ///< 鳴り終わってから破棄までに待つ残り時間（HRTF の末尾を出し切るため）
    IXAudio2Voice *dest = nullptr;  ///< 送り先（SetOutputMatrix 用）。nullptr ならマスター直結
    uint32_t srcChannels = 0;       ///< ボイスの出力チャンネル数（音源のチャンネル数。HRTF なら 2）
    bool spatial = false;           ///< 3D 音響で鳴っているか（emitter を反映中か）
    AudioEmitter emitter{};         ///< 最新のエミッター（毎フレーム SetVoiceEmitter で更新）
    std::vector<float> flatMatrix;  ///< 2D のときの出力行列（作成直後の既定値。ブレンドの基準）
    std::vector<float> workMatrix;  ///< 計算用の出力行列
  };

  // --- 初期化 ---
  /// @brief XAudio2 / マスター / サブミックス / リミッター / X3DAudio を作る
  /// @return 成功したら true
  bool InitCore_();
  /// @brief InitCore_ で作ったものを破棄する（ボイスは先に破棄しておくこと）
  void TermCore_();
  /// @brief リミッターをマスターに挿す
  void InitLimiter_();
  /// @brief 既定の出力デバイスの種類を調べる
  void DetectOutputType_();
  /// @brief デバイスが変わった / 失われたときにエンジンを作り直し、鳴っていた音を再開する
  void ResetDevice_();
  /// @brief デバイス監視のフラグを見て、必要ならエンジンを作り直す
  /// @param dt 経過秒（通知の連続をまとめる待ち時間に使う）
  void HandleDeviceEvents_(float dt);

  // --- ボイス ---
  /// @brief クリップ用の XAudio2 ソースボイスを作る（バッファはまだ積まない）
  /// @param clip クリップハンドル
  /// @param bus  送り先のバス
  /// @param hrtf 挿す HRTF エフェクト（null なら挿さない）。挿すと出力は 2ch になる
  /// @return ソースボイス。失敗時 nullptr
  SourceVoicePtr CreateSourceVoice_(int clip, AudioBus bus, AudioHrtf::Effect *hrtf = nullptr);
  /// @brief 3D で鳴っているモノラルのボイスを今の位置から作り直す（方式の切り替えを反映する）
  void RebuildSpatialVoices_();
  /// @brief ボイスの XAudio2 側を (作り直して) 指定位置から鳴らせる状態にする
  /// @param v     対象のボイス（clip / bus / loop / stream が設定済みであること）
  /// @param frame クリップ内の開始位置（フレーム）
  /// @return 成功したら true（失敗時 v.voice は null）
  bool BuildVoice_(Voice &v, uint64_t frame);
  /// @brief ボイスを破棄し、クリップの参照を返す
  /// @param v 対象のボイス
  void DestroyVoice_(Voice &v);
  /// @brief XAudio2 側のボイスだけを破棄する（ストリームの登録も外す）
  /// @param v 対象のボイス
  void ReleaseSourceVoice_(Voice &v);
  /// @brief ストリームをストリーミングスレッドの補充対象から外す
  /// @param v 対象のボイス（ストリーミングでなければ何もしない）
  void UnregisterStream_(Voice &v);
  /// @brief ボイス側からクリップの参照を返す（挙動は UnloadClip と同じ）
  /// @param clip クリップハンドル
  void ReleaseClipRef_(int clip);
  /// @brief クリップの PCM を解放し、スロットを空きに戻す
  /// @param c 対象のクリップ
  void FreeClip_(Clip &c);
  /// @brief 空きスロットを探す（無ければ末尾に追加する）
  /// @return スロットのインデックス（＝クリップハンドル）
  int AllocClipSlot_();
  /// @brief ID からボイスを探す
  /// @param voiceId ボイス ID
  /// @return 見つかったボイス（無ければ nullptr）
  Voice *FindVoice_(int voiceId);
  /// @brief ID からボイスを探す（const 版）
  /// @param voiceId ボイス ID
  /// @return 見つかったボイス（無ければ nullptr）
  const Voice *FindVoice_(int voiceId) const;
  /// @brief ボイスが鳴り終わったか
  /// @param v 対象のボイス
  /// @return 鳴り終わっていれば true（ボイスが無い場合も true）
  bool IsVoiceFinished_(const Voice &v) const;
  /// @brief ボイスのローパスを反映する
  /// @param v 対象のボイス
  void ApplyVoiceFilter_(Voice &v);
  /// @brief ボイスのリバーブ送り量を反映する（変わっていなければ何もしない）
  /// @param v     対象のボイス
  /// @param force true なら変わっていなくても反映する
  void ApplyReverbSend_(Voice &v, bool force = false);
  /// @brief 共有リバーブのサブミックスを作る
  void InitReverb_();
  /// @brief 部屋のプリセットをリバーブに反映する
  void ApplyRoom_();
  /// @brief バスのローパスを滑らかに目標へ近づけて反映する
  /// @param dt 経過秒（0 なら即座に目標値）
  void UpdateBusFilters_(float dt);
  /// @brief バスの実効音量（バス音量 × ダッキング）
  /// @param bus バス
  /// @return 倍率
  float BusGain_(AudioBus bus) const;
  /// @brief SamplesPlayed（補正込み）
  /// @param v 対象のボイス
  /// @return この XAudio2 ボイスで再生したサンプル数
  uint64_t PlayedSamples_(const Voice &v) const;
  /// @brief 現在の再生位置（クリップ内のフレーム）
  /// @param v 対象のボイス
  /// @return フレーム位置
  uint64_t CurrentFrame_(const Voice &v) const;
  /// @brief 最終的な音量（基本音量 × フェード × 一時停止の絞り）をボイスに反映する
  /// @param v 対象のボイス
  void ApplyVolume_(Voice &v);
  /// @brief 一時停止の絞り込みと Stop / Start を進める
  /// @param v  対象のボイス
  /// @param dt 経過秒
  void UpdatePause_(Voice &v, float dt);
  /// @brief フェードを進める
  /// @param v  対象のボイス
  /// @param dt 経過秒
  /// @return ボイスを破棄すべきなら true
  bool UpdateFade_(Voice &v, float dt);
  /// @brief フェードを設定する
  /// @param v       対象のボイス
  /// @param target  目標の倍率
  /// @param seconds 秒数（0 なら即時）
  /// @param end     終わったときの動作
  static void StartFade_(Voice &v, float target, float seconds, FadeEnd end);
  /// @brief ゲーム全体の一時停止の実効値が変わったときに各ボイスへ反映する
  void ApplyGamePause_();
  /// @brief ダッキングを進め、サブミックスの音量に反映する
  /// @param dt 経過秒
  void UpdateDucking_(float dt);
  /// @brief サブミックス（バス）の音量を反映する
  void ApplyBusVolumes_();

  // --- ストリーミング ---
  /// @brief ストリーミングスレッド本体
  void StreamThread_();
  /// @brief ストリームのバッファを補充する（s.mutex を持った状態で呼ぶ）
  /// @param s         対象のストリーム
  /// @param maxQueued XAudio2 に積んでおくバッファ数の上限
  void PumpStream_(StreamState &s, uint32_t maxQueued = kStreamBufferCount);

  // --- パス ---
  /// @brief 区切り文字を '/' に揃え、"." や ".." を畳んだパスにする
  /// @param path 元のパス
  /// @return 正規化したパス
  static std::string NormalizePath_(const std::string &path);
  /// @brief キャッシュのキーを作る（Windows は大文字小文字を区別しないので小文字に寄せる）
  /// @param normalizedPath 正規化済みパス
  /// @return キャッシュのキー
  static std::string CacheKey_(const std::string &normalizedPath);

  // --- 3D ---
  /// @brief 現在のスピーカー構成と音速で X3DAudio を初期化する
  /// @return 成功したら true
  bool InitX3D_();
  /// @brief 2D のときの出力行列（作成直後の既定値）を取得して保存する
  /// @param v 対象のボイス
  /// @return 取得できたら（または取得済みなら）true
  bool CaptureFlatMatrix_(Voice &v);
  /// @brief リスナーとエミッターから定位・距離減衰・ドップラー・近接効果を計算してボイスに反映する
  /// @param v 対象のボイス
  void ApplySpatial_(Voice &v);
  /// @brief 出力行列とピッチを 2D の状態に戻す
  /// @param v 対象のボイス
  void ResetSpatial_(Voice &v);

  // --- XAudio2 ---
  Microsoft::WRL::ComPtr<IXAudio2> xaudio_; ///< XAudio2 本体
  MasterVoicePtr master_{};                 ///< マスタリングボイス
  SubmixVoicePtr submix_[static_cast<int>(AudioBus::Count)]{}; ///< バスごとのサブミックス
  float busVolume_[static_cast<int>(AudioBus::Count)] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; ///< バス音量
  float masterVolume_ = 1.0f;                                          ///< マスター音量
  uint32_t outputSampleRate_ = 0;           ///< 出力のサンプリングレート

  // --- ミキサー ---
  AudioDuckingSettings ducking_{};  ///< ダッキングの設定
  float duckGain_ = 1.0f;           ///< 現在のダッキング倍率
  AudioLimiterSettings limiter_{};  ///< リミッターの設定
  bool limiterReady_ = false;       ///< リミッターを挿せたか
  AudioSpatializer spatializer_ = AudioSpatializer::Auto; ///< 3D 音響の方式
  SubmixVoicePtr reverb_{};         ///< 共有リバーブのサブミックス（作れなければ null）
  uint32_t reverbChannels_ = 0;     ///< リバーブのチャンネル数（1 か 2）
  AudioRoom room_ = AudioRoom::None; ///< 部屋のプリセット
  float busLowPassTarget_[static_cast<int>(AudioBus::Count)] = {kLowPassOpenHz, kLowPassOpenHz, kLowPassOpenHz,
                                                                kLowPassOpenHz, kLowPassOpenHz}; ///< バスのローパス（目標）
  float busLowPassCurrent_[static_cast<int>(AudioBus::Count)] = {kLowPassOpenHz, kLowPassOpenHz, kLowPassOpenHz,
                                                                 kLowPassOpenHz, kLowPassOpenHz}; ///< バスのローパス（反映中）
  bool hrtfInitTried_ = false;      ///< AudioHrtf::Init() を試したか

  // --- クリップ ---
  std::vector<Clip> clips_;                          ///< クリップ（ハンドル = インデックス）
  std::unordered_map<std::string, int> clipByPath_;  ///< パス → ハンドル
  float streamThresholdSec_ = 20.0f;                 ///< Auto でストリーミングにする長さ（秒）

  // --- ボイス ---
  std::vector<Voice> voices_;   ///< 再生中のボイス（BGM・フェードアウト中の旧 BGM を含む）
  int bgmVoiceId_ = -1;         ///< 現在の BGM のボイス ID（-1 なら無し）
  int bgmKeepAlive_ = 0;        ///< 残り猶予フレーム数。0 で BGM を止める
  float bgmFadeOutSec_ = 0.0f;  ///< 持ち主がいなくなったときのフェードアウト秒数
  int nextVoiceId_ = 1;         ///< 次に発行するボイス ID
  std::chrono::steady_clock::time_point lastUpdate_{}; ///< 前回 Update() の時刻
  bool hasLastUpdate_ = false;  ///< lastUpdate_ が有効か

  // --- 一時停止 ---
  bool paused_ = false;          ///< SetPaused() の状態
  bool editorPaused_ = false;    ///< SetEditorPaused() の状態
  bool appliedGamePause_ = false; ///< ボイスへ反映済みの一時停止状態
  bool pauseAffectsBgm_ = false; ///< ゲーム全体の一時停止で BGM も止めるか

  // --- ストリーミング ---
  std::thread streamThread_;                         ///< バッファを補充するスレッド
  mutable std::mutex streamMutex_;                   ///< streams_（補充対象の一覧）と streamQuit_ を守る
  std::condition_variable streamCv_;                 ///< スレッドの起床用
  bool streamQuit_ = false;                          ///< スレッドの終了要求
  std::vector<std::shared_ptr<StreamState>> streams_; ///< 補充対象のストリーム

  // --- デバイス ---
  std::unique_ptr<DeviceWatcher, DeviceWatcherDeleter> watcher_; ///< デバイス変更の監視
  bool initRequested_ = false;    ///< Init() が呼ばれたか（デバイスが無くても後から初期化し直すため）
  bool comInitialized_ = false;   ///< Init() で CoInitializeEx に成功したか（Term で対にする）
  bool manualResetRequested_ = false; ///< RequestDeviceReset() が呼ばれた
  float deviceEventDelay_ = 0.0f; ///< デバイス変更通知から作り直しまでの待ち（通知が連続するため）
  int deviceResetCount_ = 0;      ///< 作り直した回数
  AudioOutputType outputType_ = AudioOutputType::Unknown; ///< 既定の出力デバイスの種類

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
