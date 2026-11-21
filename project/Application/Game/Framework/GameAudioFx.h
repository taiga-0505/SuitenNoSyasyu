#pragma once
#include "Audio/AudioEngine.h"
#include "Game/Framework/StageProgress.h"
#include <algorithm>
#include <cmath>
#include <string>

/// @brief 環境に合わせた音の掛け方（水中のこもり・洞窟の響き）をまとめる
/// @details 見た目の「水中」「洞窟」と音を一か所で対応させるための薄い窓口。
///            - 水中: バス全体にローパスを掛ける（UnderwaterLook::SetLerp / RemoveStack から呼ばれる）
///                    水上⇔水中のブレンド率に合わせてカットオフを対数で動かすので、潜る途中でだんだんこもる。
///            - 洞窟: 共有リバーブを AudioRoom::Cave にする（StageProgress の cave フラグ /
///                    CaveLightZoneScript の境界で切り替える）。
///          リバーブはボイスごとの送り量（AudioSourceComponent::reverbSend など）が 0 だと響かない。
///          洞窟で響かせたい音には送り量を設定しておくこと（ローパスはバス単位なので設定不要）。
namespace GameAudioFx {

/// @brief 水中で各バスをどこまでこもらせるか（Hz）
/// @details BGM は曲として聞こえるよう浅めに、効果音・環境音は水の中らしく深めに削る。
inline constexpr float kUnderwaterCutoffBgm = 1500.0f;
inline constexpr float kUnderwaterCutoffSe = 700.0f;

/// @brief 洞窟の響きに切り替えたときの部屋
inline constexpr AudioRoom kCaveRoom = AudioRoom::Cave;

/// @brief 水上⇔水中のブレンド率に合わせてバスのローパスを掛ける
/// @param blend 0: 水上（フィルタなし） 〜 1: 完全に水中（範囲外は丸める）
inline void SetUnderwater(float blend) {
  blend = std::clamp(blend, 0.0f, 1.0f);
  auto &audio = AudioEngine::Get();
  // 耳の感じ方に合わせ、周波数は対数で補間する（直線だと潜り始めでほとんど変わらない）
  auto cutoff = [blend](float target) {
    if (blend <= 0.001f) return 0.0f; // 0 = フィルタなし
    const float open = std::log(AudioEngine::kLowPassOpenHz);
    return std::exp(open + (std::log(target) - open) * blend);
  };
  audio.SetBusLowPass(AudioBus::BGM, cutoff(kUnderwaterCutoffBgm));
  audio.SetBusLowPass(AudioBus::SE, cutoff(kUnderwaterCutoffSe));
  audio.SetBusLowPass(AudioBus::Ambience, cutoff(kUnderwaterCutoffSe));
  audio.SetBusLowPass(AudioBus::Foley, cutoff(kUnderwaterCutoffSe));
  audio.SetBusLowPass(AudioBus::Voice, cutoff(kUnderwaterCutoffSe));
}

/// @brief 洞窟の響きを入れる／外す
/// @param inCave 洞窟の中なら true
inline void SetCave(bool inCave) {
  auto &audio = AudioEngine::Get();
  const AudioRoom room = inCave ? kCaveRoom : AudioRoom::None;
  if (audio.GetRoom() != room) audio.SetRoom(room);
}

/// @brief シーンに入ったときに音の環境を初期化する
/// @details 前のシーンの水中フィルタを持ち越さないよう外し、ステージなら StageProgress の
///          cave フラグで響きを決める（Stage2 / Stage5 は途中で CaveLightZoneScript が切り替える）。
/// @param sceneName 入ったシーンの名前
inline void ResetForScene(const std::string &sceneName) {
  SetUnderwater(0.0f);
  const int stage = StageProgress::IndexOf(sceneName);
  SetCave(stage >= 0 && StageProgress::Info(stage).cave);
}

} // namespace GameAudioFx
